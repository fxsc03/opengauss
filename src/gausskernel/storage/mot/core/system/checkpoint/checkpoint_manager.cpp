/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * checkpoint_manager.cpp
 *    Interface for all checkpoint related tasks.
 *
 * IDENTIFICATION
 *    src/gausskernel/storage/mot/core/system/checkpoint/checkpoint_manager.cpp
 *
 * -------------------------------------------------------------------------
 */

#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "checkpoint_manager.h"
#include "utilities.h"
#include "row.h"
#include "sentinel.h"
#include "mot_engine.h"
#include "checkpoint_utils.h"
#include "table.h"
#include "index.h"
#include <list>
#include <algorithm>

namespace MOT {
DECLARE_LOGGER(CheckpointManager, Checkpoint);

CheckpointManager::CheckpointManager()
    : m_lock(),
      m_redoLogHandler(MOTEngine::GetInstance()->GetRedoLogHandler()),
      m_cntBit(0),
      m_phase(CheckpointPhase::REST),
      m_availableBit(true),
      m_numCpTasks(0),
      m_numThreads(GetGlobalConfiguration().m_checkpointWorkers),
      m_cpSegThreshold(GetGlobalConfiguration().m_checkpointSegThreshold),
      m_checkpointEnded(false),
      m_checkpointError(0),
      m_errorSet(false),
      m_counters{{0}},
      m_lsn(0),
      m_id(CheckpointControlFile::INVALID_ID),
      m_inProgressId(CheckpointControlFile::INVALID_ID),
      m_lastReplayLsn(0),
      m_workingDir(""),
      m_inProcessTxnsLsn(0),
      m_numSerializedEntries(0)
{}

bool CheckpointManager::Initialize()
{
    int initRc = pthread_rwlock_init(&m_fetchLock, NULL);
    if (initRc != 0) {
        MOT_LOG_ERROR("Failed to initialize CheckpointManager, could not init rwlock (%d)", initRc);
        return false;
    }
    if (GetGlobalConfiguration().m_enableCheckpoint) {
        m_checkpointers = new (std::nothrow) CheckpointWorkerPool(*this, m_cpSegThreshold);
        if (m_checkpointers == nullptr) {
            MOT_LOG_ERROR("Failed to allocate CheckpointWorkerPool");
            return false;
        }

        if (!m_checkpointers->Start()) {
            MOT_LOG_ERROR("Failed to spawn checkpoint worker threads");
            return false;
        }
    }
    return true;
}

void CheckpointManager::ResetFlags()
{
    m_checkpointEnded = false;
    m_errorSet = false;
}

CheckpointManager::~CheckpointManager()
{
    DestroyCheckpointers();
    (void)pthread_rwlock_destroy(&m_fetchLock);
    m_redoLogHandler = nullptr;
}

bool CheckpointManager::CreateSnapShot()
{
    if (!CheckpointManager::CreateCheckpointId(m_inProgressId)) {
        MOT_LOG_ERROR("Could not begin checkpoint, checkpoint id creation failed");
        OnError(
            CheckpointWorkerPool::ErrCodes::CALC, "Could not begin checkpoint, checkpoint id creation failed", nullptr);
        return false;
    }

    MOT_LOG_INFO("Creating MOT checkpoint snapshot: id: %lu", m_inProgressId);
    if (m_phase != CheckpointPhase::REST) {
        MOT_LOG_ERROR("Could not begin checkpoint, checkpoint is already running");
        OnError(
            CheckpointWorkerPool::ErrCodes::CALC, "Could not begin checkpoint, checkpoint is already running", nullptr);
        return false;
    }

    if (!CheckpointUtils::SetWorkingDir(m_workingDir, m_inProgressId)) {
        MOT_LOG_ERROR("failed to setup working dir");
        OnError(CheckpointWorkerPool::ErrCodes::FILE_IO, "failed to setup working dir", nullptr);
        return false;
    }

    if (!CheckpointManager::CreateCheckpointDir(m_workingDir)) {
        MOT_LOG_ERROR("failed to create working dir: %s", m_workingDir.c_str());
        OnError(CheckpointWorkerPool::ErrCodes::FILE_IO, "failed to create working dir", m_workingDir.c_str());
        return false;
    }

    ResetFlags();

    // Ensure that there are no transactions that started in Checkpoint COMPLETE
    // phase that are not yet completed
    WaitPrevPhaseCommittedTxnComplete();

    // Move to PREPARE phase
    m_lock.WrLock();
    MoveToNextPhase();
    m_lock.WrUnlock();

    while (m_phase != CheckpointPhase::RESOLVE) {
        (void)usleep(50000L);
    }

    // Ensure that all the transactions that started commit in PREPARE phase are completed.
    WaitPrevPhaseCommittedTxnComplete();

    // Lock the recovery manager from obtaining any more segments (relevant to standby only)
    // Flush will be a no-op on the primary
    GetRecoveryManager()->Lock();
    GetRecoveryManager()->Flush();

    // Now in RESOLVE phase, no transaction is allowed to start the commit.
    // It is safe now to obtain a list of all tables to included in this checkpoint.
    // The tables are read locked in order to avoid drop/truncate during checkpoint.
    FillTasksQueue();

    if (!CreatePendingRecoveryDataFile()) {
        MOT_LOG_ERROR("Failed to create the pending recovery data file");
        OnError(CheckpointWorkerPool::ErrCodes::FILE_IO, "Failed to create the pending recovery data file", nullptr);
        return false;
    }

    // Move to CAPTURE phase
    m_lock.WrLock();
    MoveToNextPhase();
    m_lock.WrUnlock();

    return !m_errorSet;
}

bool CheckpointManager::SnapshotReady(uint64_t lsn)
{
    MOT_LOG_INFO("MOT checkpoint snapshot ready [%lu:%lu]", m_inProgressId, lsn);
    if (m_phase != CheckpointPhase::CAPTURE) {
        MOT_LOG_ERROR("BAD Checkpoint state. Checkpoint ID: %lu, expected: 'CAPTURE', actual: %s",
            m_inProgressId,
            PhaseToString(m_phase));
        m_errorSet = true;
    } else {
        SetLsn(lsn);
        if (m_redoLogHandler != nullptr) {
            m_redoLogHandler->WrUnlock();
        }
        // release the recovery manager's lock
        GetRecoveryManager()->Unlock();
        MOT_LOG_DEBUG("Checkpoint snapshot ready. Checkpoint ID: %lu, LSN: %lu", m_inProgressId, GetLsn());
    }
    return !m_errorSet;
}

bool CheckpointManager::BeginCheckpoint()
{
    MOT_LOG_INFO("MOT begin checkpoint capture. id: %lu, lsn: %lu", m_inProgressId, m_lsn);
    Capture();

    while (!m_checkpointEnded) {
        (void)usleep(100000L);
        if (m_finishedTasks.empty() == false) {
            std::lock_guard<std::mutex> guard(m_tasksMutex);
            UnlockAndClearTables(m_finishedTasks);
        }
    }

    // No locking required here, as the checkpoint workers have already exited.
    UnlockAndClearTables(m_finishedTasks);

    // In the primary, no need to wait for transactions started in previous checkpoint phase
    // to complete since there are no transactions that start in RESOLVE phase.
    // But in the standby we need to make sure they are completed.
    WaitPrevPhaseCommittedTxnComplete();

    // Move to complete
    m_lock.WrLock();
    MoveToNextPhase();
    m_lock.WrUnlock();

    if (!m_errorSet) {
        CompleteCheckpoint();
    }

    // No locking required here, as the checkpoint workers have already exited.
    UnlockAndClearTables(m_tasksList);
    m_numCpTasks = 0;

    // Ensure that there are no transactions that started in Checkpoint CAPTURE
    // phase that are not yet completed before moving to REST phase
    WaitPrevPhaseCommittedTxnComplete();

    // Move to rest
    m_lock.WrLock();
    MoveToNextPhase();
    m_lock.WrUnlock();

    return !m_errorSet;
}

bool CheckpointManager::Abort()
{
    // Assumption is that Abort can only be called before BeginCheckpoint and after
    // Snapshot was already taken
    if (m_phase == CheckpointPhase::CAPTURE) {
        // Release the redo_lock write lock to enable other transactions to continue
        if (m_redoLogHandler != nullptr) {
            m_redoLogHandler->WrUnlock();
        }

        // In the primary, no need to wait for transactions started in previous checkpoint phase
        // to complete since there are no transactions that start in RESOLVE phase.
        // But in the standby we need to make sure they are completed.
        WaitPrevPhaseCommittedTxnComplete();

        // Move to complete
        m_lock.WrLock();
        MoveToNextPhase();
        m_lock.WrUnlock();

        // Ensure that there are no transactions that started in Checkpoint CAPTURE
        // phase that are not yet completed before moving to REST phase
        WaitPrevPhaseCommittedTxnComplete();

        // No locking required here, as there no checkpoint workers when the control reaches here.
        UnlockAndClearTables(m_tasksList);
        UnlockAndClearTables(m_finishedTasks);
        m_numCpTasks = 0;

        // Move to rest
        m_lock.WrLock();
        MoveToNextPhase();
        m_lock.WrUnlock();
    }
    return true;
}

void CheckpointManager::BeginCommit(TxnManager* txn)
{
    // In the Primary:
    // This deviates from the CALC algorithm. We don't want transactions
    // to begin in the RESOLVE phase. Transactions for which the commit
    // starts at the RESOLVE phase are not part of the checkpoint.
    // The RESOLVE phase is the "cutoff" phase. In our case, we avoid
    // transactions from starting at the RESOLVE phase because we don't
    // want them to be written to the redo-log before we take the
    // redo-log LSN (redo-point). The redo-point will be taken by the
    // envelope after we reach the CAPTURE phase.
    txn->SetCheckpointCommitEnded(false);
    m_lock.RdLock();
    while (!MOTEngine::GetInstance()->IsRecovering() && m_phase == CheckpointPhase::RESOLVE) {
        m_lock.RdUnlock();
        (void)usleep(5000);
        m_lock.RdLock();
    }
    txn->m_checkpointPhase = m_phase;
    txn->m_checkpointNABit = !m_availableBit;
    (void)m_counters[m_cntBit].fetch_add(1);
    m_lock.RdUnlock();
}

void CheckpointManager::FreePreAllocStableRows(TxnManager* txn)
{
    TxnOrderedSet_t& orderedSet = txn->m_accessMgr->GetOrderedRowSet();
    const Access* access = nullptr;
    for (const auto& ra_pair : orderedSet) {
        access = ra_pair.second;
        if (access->m_type == RD || (access->m_type == INS && access->m_params.IsUpgradeInsert() == false)) {
            continue;
        }
        if (access->m_params.IsPrimarySentinel()) {
            PrimarySentinel* s = access->GetRowFromHeader()->GetPrimarySentinel();
            MOT_ASSERT(s != nullptr);
            if (s->GetStablePreAllocStatus()) {
                CheckpointUtils::DestroyStableRow(s->GetStable());
                s->SetStable(nullptr);
                s->SetStablePreAllocStatus(false);
            }
        }
    }
}

void CheckpointManager::EndCommit(TxnManager* txn)
{
    if (txn->m_checkpointPhase == CheckpointPhase::NONE) {
        return;
    }

    txn->SetCheckpointCommitEnded(true);
    m_lock.RdLock();
    CheckpointPhase current_phase = m_phase;
    if (txn->m_checkpointPhase == m_phase) {  // current phase
        (void)m_counters[m_cntBit].fetch_sub(1);
    } else {  // previous phase
        (void)m_counters[!m_cntBit].fetch_sub(1);
    }
    if (txn->GetReplayLsn() != 0 && MOTEngine::GetInstance()->IsRecovering()) {
        // Update the last replay LSN in recovery manager in case of redo replay.
        // This is needed for getting the last replay LSN during checkpoint in standby.
        GetRecoveryManager()->SetLastReplayLsn(txn->GetReplayLsn());
    }
    m_lock.RdUnlock();
    if (m_counters[!m_cntBit] == 0 && IsAutoCompletePhase()) {
        m_lock.WrLock();
        // If the state was not change by another thread in the meanwhile,
        // I am the first one to change it
        if (current_phase == m_phase) {
            MoveToNextPhase();
        }
        m_lock.WrUnlock();
    }
}

void CheckpointManager::WaitPrevPhaseCommittedTxnComplete()
{
    m_lock.RdLock();
    while (m_counters[!m_cntBit] != 0) {
        m_lock.RdUnlock();
        (void)usleep(10000);
        m_lock.RdLock();
    }
    m_lock.RdUnlock();
}

void CheckpointManager::MoveToNextPhase()
{
    uint8_t nextPhase = ((uint8_t)m_phase + 1) % ((uint8_t)COMPLETE + 1);
    nextPhase = (!nextPhase ? 1 : nextPhase);

    MOT_LOG_DEBUG("CHECKPOINT: Move from %s to %s phase. current phase count: %u, previous phase count: %u",
        CheckpointManager::PhaseToString(m_phase),
        CheckpointManager::PhaseToString(static_cast<CheckpointPhase>(nextPhase)),
        (uint32_t)m_counters[m_cntBit],
        (uint32_t)m_counters[!m_cntBit]);

    // just before changing phase, switch available & not available bits
    if (nextPhase == REST) {
        SwapAvailableAndNotAvailable();
    }

    m_phase = (CheckpointPhase)nextPhase;
    m_cntBit = !m_cntBit;

    if (m_phase == CheckpointPhase::CAPTURE) {
        if (m_redoLogHandler != nullptr) {
            // hold the redo log lock to avoid inserting additional entries to the
            // log. Once snapshot is taken, this lock will be released in SnapshotReady().
            m_redoLogHandler->WrLock();
            // write all buffer entries before taking the LSN position
            // relevant for asynchronous logging or group commit
            m_redoLogHandler->Flush();
        }
        if (MOTEngine::GetInstance()->IsRecovering()) {
            // We are moving from RESOLVE to CAPTURE phase. No transaction is allowed to commit
            // as this point. This is the point where we take snapshot and any rows committed
            // after this point will not be included in this checkpoint.
            // Get the current last replay LSN from recovery manager and use it as m_lastReplayLsn
            // for the current checkpoint. If the system recovers from disk after this checkpoint,
            // it is safe to ignore any redo replay before this LSN.
            SetLastReplayLsn(GetRecoveryManager()->GetLastReplayLsn());
            MOT_LOG_TRACE("MOT checkpoint setting lastReplayLsn as %lu (standby)", m_lastReplayLsn);
        } else {
            MOT_LOG_TRACE("MOT checkpoint setting lastReplayLsn as %lu (primary)", m_lastReplayLsn);
        }
    }

    // there are no open transactions from previous phase, we can move forward to next phase
    if (m_counters[!m_cntBit] == 0 && IsAutoCompletePhase()) {
        MoveToNextPhase();
    }
}

const char* CheckpointManager::PhaseToString(CheckpointPhase phase)
{
    switch (phase) {
        case NONE:
            return "NONE";
        case REST:
            return "REST";
        case PREPARE:
            return "PREPARE";
        case RESOLVE:
            return "RESOLVE";
        case CAPTURE:
            return "CAPTURE";
        case COMPLETE:
            return "COMPLETE";
        default:
            break;
    }
    return "UNKNOWN";
}

bool CheckpointManager::PreAllocStableRow(TxnManager* txnMan, Row* origRow, AccessType type)
{
    CheckpointPhase startPhase = txnMan->m_checkpointPhase;
    MOT_ASSERT(startPhase != RESOLVE || MOTEngine::GetInstance()->IsRecovering());
    PrimarySentinel* s = static_cast<PrimarySentinel*>(origRow->GetPrimarySentinel());
    MOT_ASSERT(s != nullptr);
    if (origRow->GetRowType() == RowType::TOMBSTONE) {
        return true;
    }
    bool statusBit = s->GetStableStatus();
    if (startPhase == CAPTURE && statusBit == !m_availableBit) {
        MOT_ASSERT(s->GetStablePreAllocStatus() == false);
        if (!CheckpointUtils::CreateStableRow(origRow)) {
            MOT_LOG_ERROR("Failed to create stable row");
            return false;
        }
        s->SetStablePreAllocStatus(true);
    }

    return true;
}

void CheckpointManager::ApplyWrite(TxnManager* txnMan, Row* origRow, const Access* access)
{
    AccessType type = access->m_type;
    CheckpointPhase startPhase = txnMan->m_checkpointPhase;
    MOT_ASSERT(startPhase != RESOLVE || MOTEngine::GetInstance()->IsRecovering());
    PrimarySentinel* s = static_cast<PrimarySentinel*>(origRow->GetPrimarySentinel());
    MOT_ASSERT(s != nullptr);

    bool statusBit = s->GetStableStatus();
    switch (startPhase) {
        case REST:
        case PREPARE:
            if (type == INS) {
                s->SetStableStatus(!m_availableBit);
            }
            break;
        case RESOLVE:
            if (!MOTEngine::GetInstance()->IsRecovering()) {
                MOT_LOG_ERROR("No transactions are allowed to start commit in ckpt RESOVE phase");
                MOT_ASSERT(false);
            } else {
                // In the standby, we allow transactions to start commit in ckpt RESOLVE phase.
                // Handling is same as REST & PREPARE case.
                if (type == INS) {
                    s->SetStableStatus(!m_availableBit);
                }
            }
            break;
        case CAPTURE:
            if (type == INS && (access->m_params.IsUpgradeInsert() == false || origRow->IsRowDeleted())) {
                s->SetStableStatus(m_availableBit);
            } else {
                if (statusBit == !m_availableBit) {
                    MOT_ASSERT((s->GetStable() != nullptr) && (s->GetStablePreAllocStatus() == true));
                    s->SetStableStatus(m_availableBit);
                    s->SetStablePreAllocStatus(false);
                }
            }
            break;
        case COMPLETE:
            if (type == INS) {
                s->SetStableStatus(!txnMan->m_checkpointNABit);
            }
            break;
        default:
            MOT_LOG_ERROR("Unknown transaction start phase: %s", CheckpointManager::PhaseToString(startPhase));
            MOT_ASSERT(false);
    }
}

void CheckpointManager::FillTasksQueue()
{
    if (!m_tasksList.empty()) {
        MOT_LOG_ERROR("CheckpointManager::fillTasksQueue: queue is not empty!");
        OnError(
            CheckpointWorkerPool::ErrCodes::CALC, "CheckpointManager::fillTasksQueue: queue is not empty!", nullptr);
        return;
    }
    GetTableManager()->AddTablesToList(m_tasksList);
    m_numCpTasks = m_tasksList.size();
    m_mapfileInfo.clear();
    MOT_LOG_DEBUG("CheckpointManager::fillTasksQueue:: got %d tasks", m_tasksList.size());
}

void CheckpointManager::UnlockAndClearTables(std::list<Table*>& tables)
{
    std::list<Table*>::iterator it;
    for (it = tables.begin(); it != tables.end(); (void)++it) {
        Table* table = *it;
        if (table != nullptr) {
            table->Unlock();
        }
    }
    tables.clear();
}

void CheckpointManager::TaskDone(Table* table, uint32_t numSegs, bool success)
{
    MOT_ASSERT(table);
    if (success) { /* only successful tasks are added to the map file */
        if (table == nullptr) {
            OnError(CheckpointWorkerPool::ErrCodes::MEMORY, "Got a null table on task done", nullptr);
            return;
        }
        MapFileEntry* entry = new (std::nothrow) MapFileEntry();
        if (entry != nullptr) {
            entry->m_tableId = table->GetTableId();
            entry->m_maxSegId = numSegs;
            MOT_LOG_DEBUG("TaskDone %lu: %u %u segs", m_inProgressId, entry->m_tableId, numSegs);
            std::lock_guard<std::mutex> guard(m_tasksMutex);
            m_mapfileInfo.push_back(entry);
            m_finishedTasks.push_back(table);
        } else {
            OnError(CheckpointWorkerPool::ErrCodes::MEMORY, "Failed to allocate map file entry", nullptr);
            return;
        }
    }

    if (--m_numCpTasks == 0) {
        m_checkpointEnded = true;
        m_notifier.SetState(ThreadNotifier::ThreadState::SLEEP);
    }
}

void CheckpointManager::CompleteCheckpoint()
{
    CheckpointControlFile* ctrlFile = CheckpointControlFile::GetCtrlFile();
    if (ctrlFile == nullptr) {
        OnError(CheckpointWorkerPool::ErrCodes::MEMORY, "Failed to retrieve control file object", nullptr);
        return;
    }

    if (!CreateCheckpointMap()) {
        OnError(CheckpointWorkerPool::ErrCodes::FILE_IO, "Failed to create map file", nullptr);
        return;
    }

    if (!ctrlFile->IsValid()) {
        OnError(CheckpointWorkerPool::ErrCodes::FILE_IO, "Invalid control file", nullptr);
        return;
    }

    bool finishedUpdatingFiles = false;
    (void)pthread_rwlock_wrlock(&m_fetchLock);
    do {
        if (!CreateEndFile()) {
            OnError(CheckpointWorkerPool::ErrCodes::FILE_IO, "Failed to create completion file", nullptr);
            break;
        }

        if (!ctrlFile->Update(m_inProgressId, GetLsn(), GetLastReplayLsn(), GetTxnIdManager().GetCurrentId())) {
            OnError(CheckpointWorkerPool::ErrCodes::FILE_IO, "Failed to update control file", nullptr);
            break;
        }

        // Update checkpoint Id
        SetId(m_inProgressId);
        finishedUpdatingFiles = true;
    } while (0);
    (void)pthread_rwlock_unlock(&m_fetchLock);

    if (!finishedUpdatingFiles) {
        return;
    }

    RemoveOldCheckpoints(m_inProgressId);
    MOT_LOG_INFO("MOT checkpoint [%lu:%lu:%lu] completed", m_inProgressId, GetLsn(), GetLastReplayLsn());
}

void CheckpointManager::DestroyCheckpointers()
{
    if (m_checkpointers != nullptr) {
        m_notifier.Notify(ThreadNotifier::ThreadState::TERMINATE);
        delete m_checkpointers;
        m_checkpointers = nullptr;
    }
}

void CheckpointManager::Capture()
{
    if (m_numCpTasks == 0) {
        MOT_LOG_INFO("No tasks in queue - empty checkpoint");
        m_checkpointEnded = true;
    } else {
        m_notifier.Notify(ThreadNotifier::ThreadState::ACTIVE);
    }
}

void CheckpointManager::RemoveOldCheckpoints(uint64_t curCheckcpointId)
{
    std::string workingDir = "";
    if (!CheckpointUtils::GetWorkingDir(workingDir)) {
        MOT_LOG_ERROR("RemoveOldCheckpoints: failed to get the working dir");
        return;
    }

    DIR* dir = opendir(workingDir.c_str());
    if (dir) {
        struct dirent* p;
        while ((p = readdir(dir)) != nullptr) {
            /* Skip the names "." and ".." and anything that is not chkpt_ */
            if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, "..") ||
                strncmp(p->d_name, CheckpointUtils::CKPT_DIR_PREFIX, strlen(CheckpointUtils::CKPT_DIR_PREFIX)) ||
                strlen(p->d_name) <= strlen(CheckpointUtils::CKPT_DIR_PREFIX)) {
                continue;
            }

            /* Skip if the entry is not a directory. Handle DT_UNKNOWN for file systems that does not support d_type. */
            if (p->d_type != DT_DIR && p->d_type != DT_UNKNOWN) {
                continue;
            }

            uint64_t chkptId = strtoll(p->d_name + strlen(CheckpointUtils::CKPT_DIR_PREFIX), NULL, 10);
            if (chkptId == curCheckcpointId) {
                MOT_LOG_DEBUG("RemoveOldCheckpoints: exclude %lu", chkptId);
                continue;
            }
            MOT_LOG_DEBUG("RemoveOldCheckpoints: removing %lu", chkptId);
            RemoveCheckpointDir(chkptId);
        }
        (void)closedir(dir);
    } else {
        MOT_LOG_ERROR("RemoveOldCheckpoints: failed to open dir: %s, error %d - %s",
            workingDir.c_str(),
            errno,
            gs_strerror(errno));
    }
}

void CheckpointManager::RemoveCheckpointDir(uint64_t checkpointId)
{
    errno_t erc;
    char buf[CheckpointUtils::CHECKPOINT_MAX_PATH];
    std::string oldCheckpointDir;
    if (!CheckpointUtils::SetWorkingDir(oldCheckpointDir, checkpointId)) {
        MOT_LOG_ERROR("removeCheckpointDir: failed to set working directory");
        return;
    }

    DIR* dir = opendir(oldCheckpointDir.c_str());
    if (dir != nullptr) {
        struct dirent* p;
        while ((p = readdir(dir)) != nullptr) {
            /* Skip the names "." and ".." */
            if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, "..")) {
                continue;
            }

            struct stat statbuf = {0};
            erc = memset_s(buf, CheckpointUtils::CHECKPOINT_MAX_PATH, 0, CheckpointUtils::CHECKPOINT_MAX_PATH);
            securec_check(erc, "\0", "\0");
            erc = snprintf_s(buf,
                CheckpointUtils::CHECKPOINT_MAX_PATH,
                CheckpointUtils::CHECKPOINT_MAX_PATH - 1,
                "%s/%s",
                oldCheckpointDir.c_str(),
                p->d_name);
            securec_check_ss(erc, "\0", "\0");
            if (!stat(buf, &statbuf) && S_ISREG(statbuf.st_mode)) {
                MOT_LOG_DEBUG("removeCheckpointDir: deleting %s", buf);
                if (unlink(buf) != 0) {
                    MOT_LOG_ERROR(
                        "removeCheckpointDir: failed to remove file %s, error %d:%s", buf, errno, gs_strerror(errno));
                }
            }
        }
        (void)closedir(dir);
        MOT_LOG_DEBUG("removeCheckpointDir: removing dir %s", oldCheckpointDir.c_str());
        if (rmdir(oldCheckpointDir.c_str()) != 0) {
            MOT_LOG_ERROR("removeCheckpointDir: failed to remove directory %s, error %d:%s",
                oldCheckpointDir.c_str(),
                errno,
                gs_strerror(errno));
        }
    } else {
        MOT_LOG_ERROR("removeCheckpointDir: failed to open dir: %s, error %d - %s",
            oldCheckpointDir.c_str(),
            errno,
            gs_strerror(errno));
    }
}

bool CheckpointManager::CreateCheckpointMap()
{
    int fd = -1;
    std::string fileName;
    bool ret = false;

    do {
        CheckpointUtils::MakeMapFilename(fileName, m_workingDir, m_inProgressId);
        if (!CheckpointUtils::OpenFileWrite(fileName, fd)) {
            MOT_LOG_ERROR("createCheckpointMap: failed to create file '%s' - %d - %s",
                fileName.c_str(),
                errno,
                gs_strerror(errno));
            break;
        }

        CheckpointUtils::MapFileHeader mapFileHeader{CheckpointUtils::HEADER_MAGIC, m_mapfileInfo.size()};
        size_t wrStat =
            CheckpointUtils::WriteFile(fd, (const char*)&mapFileHeader, sizeof(CheckpointUtils::MapFileHeader));
        if (wrStat != sizeof(CheckpointUtils::MapFileHeader)) {
            MOT_LOG_ERROR(
                "createCheckpointMap: failed to write map file's header (%d) %d %s", wrStat, errno, gs_strerror(errno));
            (void)CheckpointUtils::CloseFile(fd);
            break;
        }

        size_t entryCount = 0;
        for (std::list<MapFileEntry*>::iterator it = m_mapfileInfo.begin(); it != m_mapfileInfo.end(); (void)++it) {
            MapFileEntry* entry = *it;
            if (CheckpointUtils::WriteFile(fd, (const char*)entry, sizeof(MapFileEntry)) != sizeof(MapFileEntry)) {
                MOT_LOG_ERROR("createCheckpointMap: failed to write map file entry");
                break;  // break out of for loop
            }
            delete entry;
            entryCount++;
        }

        if (entryCount != m_mapfileInfo.size()) {
            (void)CheckpointUtils::CloseFile(fd);
            break;
        }

        if (CheckpointUtils::FlushFile(fd)) {
            MOT_LOG_ERROR("createCheckpointMap: failed to flush map file");
            (void)CheckpointUtils::CloseFile(fd);
            break;
        }

        if (CheckpointUtils::CloseFile(fd)) {
            MOT_LOG_ERROR("createCheckpointMap: failed to close map file");
            break;
        }
        ret = true;
    } while (0);

    return ret;
}

void CheckpointManager::OnError(int errCode, const char* errMsg, const char* optionalMsg)
{
    std::lock_guard<spin_lock> lock(m_errorReportLock);
    if (!m_errorSet) {
        m_checkpointError = errCode;
        m_errorMessage.clear();
        (void)m_errorMessage.append(errMsg);
        if (optionalMsg != nullptr) {
            (void)m_errorMessage.append(" ");
            (void)m_errorMessage.append(optionalMsg);
        }
        m_errorSet = true;
        m_checkpointEnded = true;
    }
}

bool CheckpointManager::CreateCheckpointId(uint64_t& checkpointId)
{
    if (CheckpointControlFile::GetCtrlFile() == nullptr) {
        return false;
    }

    uint64_t curId = CheckpointControlFile::GetCtrlFile()->GetId();
    checkpointId = time(nullptr);
    while (true) {
        if (checkpointId == curId) {
            checkpointId += 10;
        } else {
            break;
        }
    }
    MOT_LOG_DEBUG("createCheckpointId: %lu, curId: %lu", checkpointId, curId);
    return true;
}

bool CheckpointManager::GetCheckpointDirName(std::string& dirName)
{
    if (!CheckpointUtils::SetDirName(dirName, GetId())) {
        MOT_LOG_ERROR("SetDirName failed");
        return false;
    }
    return true;
}

bool CheckpointManager::GetCheckpointWorkingDir(std::string& workingDir) const
{
    if (!CheckpointUtils::GetWorkingDir(workingDir)) {
        MOT_LOG_ERROR("Could not obtain working directory");
        return false;
    }
    return true;
}

bool CheckpointManager::CreateCheckpointDir(const std::string& dir)
{
    if (mkdir(dir.c_str(), S_IRWXU)) { /* 0700 */
        MOT_LOG_DEBUG("Failed to create dir %s (%d:%s)", dir.c_str(), errno, gs_strerror(errno));
        return false;
    }
    return true;
}

bool CheckpointManager::CreatePendingRecoveryDataFile()
{
    int fd = -1;
    std::string fileName;
    bool ret = false;

    do {
        CheckpointUtils::MakeIpdFilename(fileName, m_workingDir, m_inProgressId);
        if (!CheckpointUtils::OpenFileWrite(fileName, fd)) {
            MOT_LOG_ERROR("CreatePendingRecoveryDataFile: Failed to create file '%s' - %d - %s",
                fileName.c_str(),
                errno,
                gs_strerror(errno));
            break;
        }

        CheckpointUtils::PendingTxnDataFileHeader ptdFileHeader;
        ptdFileHeader.m_magic = CheckpointUtils::HEADER_MAGIC;
        ptdFileHeader.m_numEntries = 0;  // will be filled later on

        if (lseek(fd, sizeof(CheckpointUtils::PendingTxnDataFileHeader), SEEK_SET) !=
            sizeof(CheckpointUtils::PendingTxnDataFileHeader)) {
            MOT_LOG_ERROR("CreatePendingRecoveryDataFile: lseek (1) failed [%d %s]", errno, gs_strerror(errno));
            (void)CheckpointUtils::CloseFile(fd);
            break;
        }

        uint64_t serializeStatus = GetRecoveryManager()->SerializePendingRecoveryData(fd);
        if (serializeStatus == (uint64_t)(-1)) {
            MOT_LOG_ERROR(
                "CreatePendingRecoveryDataFile: Failed to serialize transactions [%d %s]", errno, gs_strerror(errno));
            (void)CheckpointUtils::CloseFile(fd);
            break;
        }

        ptdFileHeader.m_numEntries = serializeStatus;
        if (lseek(fd, 0, SEEK_SET) != 0) {
            MOT_LOG_ERROR("CreatePendingRecoveryDataFile: lseek (2) failed [%d %s]", errno, gs_strerror(errno));
            (void)CheckpointUtils::CloseFile(fd);
            break;
        }

        size_t wrStat = CheckpointUtils::WriteFile(
            fd, (const char*)&ptdFileHeader, sizeof(CheckpointUtils::PendingTxnDataFileHeader));
        if (wrStat != sizeof(CheckpointUtils::PendingTxnDataFileHeader)) {
            MOT_LOG_ERROR("CreatePendingRecoveryDataFile: Failed to write file's header (%d) [%d %s]",
                wrStat,
                errno,
                gs_strerror(errno));
            (void)CheckpointUtils::CloseFile(fd);
            break;
        }

        if (CheckpointUtils::FlushFile(fd)) {
            MOT_LOG_ERROR("CreatePendingRecoveryDataFile: Failed to flush map file");
            (void)CheckpointUtils::CloseFile(fd);
            break;
        }

        if (CheckpointUtils::CloseFile(fd)) {
            MOT_LOG_ERROR("CreatePendingRecoveryDataFile: Failed to close map file");
            break;
        }
        ret = true;
        MOT_LOG_INFO("CreatePendingRecoveryDataFile: Serialized %d entries", serializeStatus);
    } while (0);
    return ret;
}
bool CheckpointManager::CreateEndFile()
{
    int fd = -1;
    std::string fileName;
    bool ret = false;

    do {
        CheckpointUtils::MakeEndFilename(fileName, m_workingDir, m_inProgressId);
        if (!CheckpointUtils::OpenFileWrite(fileName, fd)) {
            MOT_LOG_ERROR(
                "CreateEndFile: Failed to create file '%s' - %d - %s", fileName.c_str(), errno, gs_strerror(errno));
            break;
        }

        if (CheckpointUtils::FlushFile(fd)) {
            MOT_LOG_ERROR("CreateEndFile: Failed to flush end file");
            (void)CheckpointUtils::CloseFile(fd);
            break;
        }

        if (CheckpointUtils::CloseFile(fd)) {
            MOT_LOG_ERROR("CreateEndFile: Failed to close end file");
            break;
        }
        ret = true;
    } while (0);

    return ret;
}
}  // namespace MOT
