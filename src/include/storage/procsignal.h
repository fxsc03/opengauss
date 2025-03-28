/* -------------------------------------------------------------------------
 *
 * procsignal.h
 *	  Routines for interprocess signalling
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/procsignal.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef PROCSIGNAL_H
#define PROCSIGNAL_H

#include "storage/backendid.h"

/*
 * Reasons for signalling a Postgres child process (a backend or an auxiliary
 * process, like checkpointer).  We can cope with concurrent signals for different
 * reasons.  However, if the same reason is signaled multiple times in quick
 * succession, the process is likely to observe only one notification of it.
 * This is okay for the present uses.
 *
 * Also, because of race conditions, it's important that all the signals be
 * defined so that no harm is done if a process mistakenly receives one.
 */
#ifdef PGXC
/*
 * In the case of Postgres-XC, it may be possible that this backend is
 * signaled during a pool manager reload process. In this case it means that
 * remote node connection has been changed inside pooler, so backend has to
 * abort its current transaction, reconnect to pooler and update its session
 * information regarding remote node handles.
 */
#endif
typedef enum {
    PROCSIG_CATCHUP_INTERRUPT,    /* sinval catchup interrupt */
    PROCSIG_NOTIFY_INTERRUPT,     /* listen/notify interrupt */
    PROCSIG_DEFAULTXACT_READONLY, /* default transaction read only */
#ifdef PGXC
    PROCSIG_PGXCPOOL_RELOAD,      /* abort current transaction and reconnect to pooler */
    PROCSIG_MEMORYCONTEXT_DUMP,   /* dump memory context on all backends */
    PROCSIG_UPDATE_WORKLOAD_DATA, /* update workload data */
    PROCSIG_SPACE_LIMIT,          /* space limitation */
    PROCSIG_STREAM_STOP_CHECK,    /* check local connections close or not */
#endif

    /* Recovery conflict reasons */
    PROCSIG_RECOVERY_CONFLICT_DATABASE,
    PROCSIG_RECOVERY_CONFLICT_TABLESPACE,
    PROCSIG_RECOVERY_CONFLICT_LOCK,
    PROCSIG_RECOVERY_CONFLICT_SNAPSHOT,
    PROCSIG_RECOVERY_CONFLICT_BUFFERPIN,
    PROCSIG_RECOVERY_CONFLICT_STARTUP_DEADLOCK,
    PROCSIG_EXECUTOR_FLAG,

    /* close active session socket */
    PROCSIG_COMM_CLOSE_ACTIVE_SESSION_SOCKET,

    NUM_PROCSIGNALS /* Must be last! */
} ProcSignalReason;

/*
 * prototypes for functions in procsignal.c
 */
extern Size ProcSignalShmemSize(void);
extern void ProcSignalShmemInit(void);

extern void ProcSignalInit(int pss_idx);
extern int SendProcSignal(ThreadId pid, ProcSignalReason reason, BackendId backendId);

extern void procsignal_sigusr1_handler(SIGNAL_ARGS);
extern int SendProcSignalForLibcomm(ThreadId pid, ProcSignalReason reason, BackendId backendId);

#endif /* PROCSIGNAL_H */
