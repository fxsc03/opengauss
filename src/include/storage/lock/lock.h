/* -------------------------------------------------------------------------
 *
 * lock.h
 *	  openGauss low-level lock mechanism
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/lock/lock.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef LOCK_H_
#define LOCK_H_

#include "storage/backendid.h"
#include "storage/lock/lwlock.h"
#include "storage/lock/waitpolicy.h"
#include "storage/shmem.h"
#include "gs_thread.h"

typedef struct PROC_QUEUE {
    SHM_QUEUE links; /* head of list of PGPROC objects */
    int size;        /* number of entries in list */
} PROC_QUEUE;

/*
 * Top-level transactions are identified by VirtualTransactionIDs comprising
 * the BackendId of the backend running the xact, plus a locally-assigned
 * LocalTransactionId.	These are guaranteed unique over the short term,
 * but will be reused after a database restart; hence they should never
 * be stored on disk.
 *
 * Note that struct VirtualTransactionId can not be assumed to be atomically
 * assignable as a whole.  However, type LocalTransactionId is assumed to
 * be atomically assignable, and the backend ID doesn't change often enough
 * to be a problem, so we can fetch or assign the two fields separately.
 * We deliberately refrain from using the struct within PGPROC, to prevent
 * coding errors from trying to use struct assignment with it; instead use
 * GET_VXID_FROM_PGPROC().
 */
typedef struct VirtualTransactionId {
    BackendId backendId;                   /* determined at backend startup */
    LocalTransactionId localTransactionId; /* backend-local transaction
                                            * id */
} VirtualTransactionId;

#define InvalidLocalTransactionId 0
#define LocalTransactionIdIsValid(lxid) ((lxid) != InvalidLocalTransactionId)
#define VirtualTransactionIdIsValid(vxid) \
    (((vxid).backendId != InvalidBackendId) && LocalTransactionIdIsValid((vxid).localTransactionId))
#define VirtualTransactionIdEquals(vxid1, vxid2) \
    ((vxid1).backendId == (vxid2).backendId && (vxid1).localTransactionId == (vxid2).localTransactionId)
#define SetInvalidVirtualTransactionId(vxid) \
    ((vxid).backendId = InvalidBackendId, (vxid).localTransactionId = InvalidLocalTransactionId)
#define GET_VXID_FROM_PGPROC(vxid, proc) ((vxid).backendId = (proc).backendId, (vxid).localTransactionId = (proc).lxid)

/*
 * LOCKMODE is an integer (1..N) indicating a lock type.  LOCKMASK is a bit
 * mask indicating a set of held or requested lock types (the bit 1<<mode
 * corresponds to a particular lock mode).
 */
typedef int LOCKMASK;
typedef int LOCKMODE;

/* MAX_LOCKMODES cannot be larger than the # of bits in LOCKMASK */
#define MAX_LOCKMODES 10

#define LOCKBIT_ON(lockmode) (1U << (lockmode))
#define LOCKBIT_OFF(lockmode) (~(1U << (lockmode)))

/*
 * This data structure defines the locking semantics associated with a
 * "lock method".  The semantics specify the meaning of each lock mode
 * (by defining which lock modes it conflicts with).
 * All of this data is constant and is kept in const tables.
 *
 * numLockModes -- number of lock modes (READ,WRITE,etc) that
 *		are defined in this lock method.  Must be less than MAX_LOCKMODES.
 *
 * conflictTab -- this is an array of bitmasks showing lock
 *		mode conflicts.  conflictTab[i] is a mask with the j-th bit
 *		turned on if lock modes i and j conflict.  Lock modes are
 *		numbered 1..numLockModes; conflictTab[0] is unused.
 *
 * lockModeNames -- ID strings for debug printouts.
 *
 * trace_flag -- pointer to GUC trace flag for this lock method.  (The
 * GUC variable is not constant, but we use "const" here to denote that
 * it can't be changed through this reference.)
 */
typedef struct LockMethodData {
    int numLockModes;
    const LOCKMASK* conflictTab;
    const char* const* lockModeNames;
    const bool* trace_flag;
} LockMethodData;

typedef const LockMethodData* LockMethod;

/*
 * Lock methods are identified by LOCKMETHODID.  (Despite the declaration as
 * uint16, we are constrained to 256 lockmethods by the layout of LOCKTAG.)
 */
typedef uint16 LOCKMETHODID;

/*
 * These identify the known lock methods
 * Attention: adding new lock method should also modify MAX_LOCKMETHOD in knl_session.h
 */
#define DEFAULT_LOCKMETHOD 1
#define USER_LOCKMETHOD 2

/*
 * These are the valid values of type LOCKMODE for all the standard lock
 * methods (both DEFAULT and USER).
 */

/* NoLock is not a lock mode, but a flag value meaning "don't get a lock" */
#define NoLock 0

#define AccessShareLock 1  /* SELECT */
#define RowShareLock 2     /* SELECT FOR UPDATE/FOR SHARE */
#define RowExclusiveLock 3 /* INSERT, UPDATE, DELETE */
#define ShareUpdateExclusiveLock                         \
    4               /* VACUUM (non-FULL),ANALYZE, CREATE \
                     * INDEX CONCURRENTLY */
#define ShareLock 5 /* CREATE INDEX (WITHOUT CONCURRENTLY) */
#define ShareRowExclusiveLock                \
    6 /* like EXCLUSIVE MODE, but allows ROW \
       * SHARE */
#define ExclusiveLock                  \
    7 /* blocks ROW SHARE/SELECT...FOR \
       * UPDATE */
#define AccessExclusiveLock              \
    8 /* ALTER TABLE, DROP TABLE, VACUUM \
       * FULL, and unqualified LOCK TABLE */

typedef enum PARTITION_LOCK_TYPE {
    PARTITION_LOCK,
    PARTITION_SEQUENCE_LOCK
} PARTITION_LOCK_TYPE;

/*
 * LOCKTAG is the key information needed to look up a LOCK item in the
 * lock hashtable.	A LOCKTAG value uniquely identifies a lockable object.
 *
 * The LockTagType enum defines the different kinds of objects we can lock.
 * We can handle up to 256 different LockTagTypes.
 */
typedef enum LockTagType {
    LOCKTAG_RELATION, /* whole relation */
    /* ID info for a relation is DB OID + REL OID; DB OID = 0 if shared */
    LOCKTAG_RELATION_EXTEND, /* the right to extend a relation */
    /* same ID info as RELATION */
    LOCKTAG_PARTITION,          /*partition*/
    LOCKTAG_PARTITION_SEQUENCE, /*partition sequence*/
    LOCKTAG_PAGE,               /* one page of a relation */
    /* ID info for a page is RELATION info + BlockNumber */
    LOCKTAG_TUPLE, /* one physical tuple */
    /* ID info for a tuple is PAGE info + OffsetNumber */
    LOCKTAG_TRANSACTION, /* transaction (for waiting for xact done) */
    /* ID info for a transaction is its TransactionId */
    LOCKTAG_VIRTUALTRANSACTION, /* virtual transaction (ditto) */
    /* ID info for a virtual transaction is its VirtualTransactionId */
    LOCKTAG_OBJECT, /* non-relation database object */
    /* ID info for an object is DB OID + CLASS OID + OBJECT OID + SUBID */
    LOCKTAG_CSTORE_FREESPACE, /* cstore free space */

    /*
     * Note: object ID has same representation as in pg_depend and
     * pg_description, but notice that we are constraining SUBID to 16 bits.
     * Also, we use DB OID = 0 for shared objects such as tablespaces.
     */
    LOCKTAG_USERLOCK, /* reserved for old contrib/userlock code */
    LOCKTAG_ADVISORY, /* advisory user locks */
    /* same ID info as spcoid, dboid, reloid */
    LOCKTAG_RELFILENODE, /* relfilenode */
    LOCKTAG_SUBTRANSACTION, /* subtransaction (for waiting for subxact done) */
    /* ID info for a transaction is its TransactionId + SubTransactionId */
    LOCKTAG_UID,
    LOCK_EVENT_NUM
} LockTagType;

#define LOCKTAG_LAST_TYPE (LOCK_EVENT_NUM - 1)
extern const char* const LockTagTypeNames[];

/*
 * The LOCKTAG struct is defined with malice aforethought to fit into 16
 * bytes with no padding.  Note that this would need adjustment if we were
 * to widen Oid, BlockNumber, or TransactionId to more than 32 bits.
 *
 * We include lockmethodid in the locktag so that a single hash table in
 * shared memory can store locks of different lockmethods.
 */
typedef struct LOCKTAG {
    uint32 locktag_field1;      /* a 32-bit ID field */
    uint32 locktag_field2;      /* a 32-bit ID field */
    uint32 locktag_field3;      /* a 32-bit ID field */
    uint32 locktag_field4;      /* a 32-bit ID field */
    uint16 locktag_field5;      /* a 16-bit ID field */
    uint8 locktag_type;         /* see enum LockTagType */
    uint8 locktag_lockmethodid; /* lockmethod indicator */
} LOCKTAG;

/*
 * These macros define how we map logical IDs of lockable objects into
 * the physical fields of LOCKTAG.	Use these to set up LOCKTAG values,
 * rather than accessing the fields directly.  Note multiple eval of target!
 */
#define SET_LOCKTAG_RELATION(locktag, dboid, reloid) \
    ((locktag).locktag_field1 = (dboid),             \
        (locktag).locktag_field2 = (reloid),         \
        (locktag).locktag_field3 = 0,                \
        (locktag).locktag_field4 = 0,                \
        (locktag).locktag_field5 = 0,                \
        (locktag).locktag_type = LOCKTAG_RELATION,   \
        (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_RELFILENODE(locktag, spcoid, dboid, reloid) \
    ((locktag).locktag_field1 = (spcoid),                       \
        (locktag).locktag_field2 = (dboid),                     \
        (locktag).locktag_field3 = (reloid),                    \
        (locktag).locktag_field4 = 0,                           \
        (locktag).locktag_field5 = 0,                           \
        (locktag).locktag_type = LOCKTAG_RELFILENODE,           \
        (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_RELATION_EXTEND(locktag, dboid, reloid, bucketid) \
    ((locktag).locktag_field1 = (dboid),                    \
        (locktag).locktag_field2 = (reloid),                \
        (locktag).locktag_field3 = 0,                       \
        (locktag).locktag_field4 = 0,                       \
        (locktag).locktag_field5 = (bucketid),              \
        (locktag).locktag_type = LOCKTAG_RELATION_EXTEND,   \
        (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_PAGE(locktag, dboid, reloid, bucketid, blocknum) \
    ((locktag).locktag_field1 = (dboid),                   \
        (locktag).locktag_field2 = (reloid),               \
        (locktag).locktag_field3 = (blocknum),             \
        (locktag).locktag_field4 = 0,                       \
        (locktag).locktag_field5 = (bucketid),             \
        (locktag).locktag_type = LOCKTAG_PAGE,             \
        (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_TUPLE(locktag, dboid, reloid, bucketid, blocknum, offnum) \
    ((locktag).locktag_field1 = (dboid),                            \
        (locktag).locktag_field2 = (reloid),                        \
        (locktag).locktag_field3 = (blocknum),                      \
        (locktag).locktag_field4 = (offnum),                        \
        (locktag).locktag_field5 = (bucketid),                      \
        (locktag).locktag_type = LOCKTAG_TUPLE,                     \
        (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_UID(locktag, dboid, reloid, uidHighBits, uidLowBits) \
    ((locktag).locktag_field1 = (dboid),                    \
        (locktag).locktag_field2 = (reloid),                \
        (locktag).locktag_field3 = (uidHighBits),                   \
        (locktag).locktag_field4 = (uidLowBits),                       \
        (locktag).locktag_field5 = 0,                       \
        (locktag).locktag_type = LOCKTAG_UID,               \
        (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_TRANSACTION(locktag, xid)               \
    ((locktag).locktag_field1 = (uint32)((xid)&0xFFFFFFFF), \
        (locktag).locktag_field2 = (uint32)((xid) >> 32),   \
        (locktag).locktag_field3 = 0,                       \
        (locktag).locktag_field4 = 0,                       \
        (locktag).locktag_field5 = 0,                       \
        (locktag).locktag_type = LOCKTAG_TRANSACTION,       \
        (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_VIRTUALTRANSACTION(locktag, vxid)                                \
    ((locktag).locktag_field1 = (vxid).backendId,                                    \
        (locktag).locktag_field2 = (uint32)((vxid).localTransactionId & 0xFFFFFFFF),\
        (locktag).locktag_field3 = (uint32)((vxid).localTransactionId >> 32),        \
        (locktag).locktag_field4 = 0,                                                \
        (locktag).locktag_field5 = 0,                                                \
        (locktag).locktag_type = LOCKTAG_VIRTUALTRANSACTION,                         \
        (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_SUBTRANSACTION(locktag, xid, subxid) \
            ((locktag).locktag_field1 = (uint32)((xid)&0xFFFFFFFF), \
             (locktag).locktag_field2 = (uint32)((xid) >> 32), \
             (locktag).locktag_field3 = subxid, \
             (locktag).locktag_field4 = 0, \
             (locktag).locktag_field5 = 0, \
             (locktag).locktag_type = LOCKTAG_SUBTRANSACTION, \
             (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_OBJECT(locktag, dboid, classoid, objoid, objsubid) \
    ((locktag).locktag_field1 = (dboid),                               \
        (locktag).locktag_field2 = (classoid),                         \
        (locktag).locktag_field3 = (objoid),                           \
        (locktag).locktag_field4 = (objsubid),                         \
        (locktag).locktag_field5 = 0,                                  \
        (locktag).locktag_type = LOCKTAG_OBJECT,                       \
        (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_ADVISORY(locktag, id1, id2, id3, id4) \
    ((locktag).locktag_field1 = (id1),                    \
        (locktag).locktag_field2 = (id2),                 \
        (locktag).locktag_field3 = (id3),                 \
        (locktag).locktag_field4 = (id4),                 \
        (locktag).locktag_field5 = 0,                     \
        (locktag).locktag_type = LOCKTAG_ADVISORY,        \
        (locktag).locktag_lockmethodid = USER_LOCKMETHOD)

#define SET_LOCKTAG_PARTITION(locktag, id1, id2, id3) \
    ((locktag).locktag_field1 = (id1),                \
        (locktag).locktag_field2 = (id2),             \
        (locktag).locktag_field3 = (id3),             \
        (locktag).locktag_field4 = 0,                 \
        (locktag).locktag_field5 = 0,                 \
        (locktag).locktag_type = LOCKTAG_PARTITION,   \
        (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_PARTITION_SEQUENCE(locktag, id1, id2, id3) \
    ((locktag).locktag_field1 = (id1),                         \
        (locktag).locktag_field2 = (id2),                      \
        (locktag).locktag_field3 = (id3),                      \
        (locktag).locktag_field4 = 0,                          \
        (locktag).locktag_field5 = 0,                          \
        (locktag).locktag_type = LOCKTAG_PARTITION_SEQUENCE,   \
        (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define SET_LOCKTAG_CSTORE_FREESPACE(locktag, id1, id2)    \
    ((locktag).locktag_field1 = (id1),                     \
        (locktag).locktag_field2 = (id2),                  \
        (locktag).locktag_field3 = 0,                      \
        (locktag).locktag_field4 = 0,                      \
        (locktag).locktag_field5 = 0,                      \
        (locktag).locktag_type = LOCKTAG_CSTORE_FREESPACE, \
        (locktag).locktag_lockmethodid = DEFAULT_LOCKMETHOD)

#define PackageRelationId  7815
#define SET_LOCKTAG_PKG_OBJECT(locktag, dboid, objoid)      \
    ((locktag).locktag_field1 = (dboid),                    \
        (locktag).locktag_field2 = (PackageRelationId),     \
        (locktag).locktag_field3 = (objoid),                \
        (locktag).locktag_field4 = 0,                       \
        (locktag).locktag_field5 = 0,                       \
        (locktag).locktag_type = LOCKTAG_OBJECT,            \
        (locktag).locktag_lockmethodid = USER_LOCKMETHOD)

#define ProcedureRelationId  1255
#define SET_LOCKTAG_PROC_OBJECT(locktag, dboid, objoid)    \
    ((locktag).locktag_field1 = (dboid),                   \
        (locktag).locktag_field2 = (ProcedureRelationId),  \
        (locktag).locktag_field3 = objoid,                 \
        (locktag).locktag_field4 = 0,                      \
        (locktag).locktag_field5 = 0,                      \
        (locktag).locktag_type = LOCKTAG_OBJECT,           \
        (locktag).locktag_lockmethodid = USER_LOCKMETHOD)

/*
 * Per-locked-object lock information:
 *
 * tag -- uniquely identifies the object being locked
 * grantMask -- bitmask for all lock types currently granted on this object.
 * waitMask -- bitmask for all lock types currently awaited on this object.
 * procLocks -- list of PROCLOCK objects for this lock.
 * waitProcs -- queue of processes waiting for this lock.
 * requested -- count of each lock type currently requested on the lock
 *		(includes requests already granted!!).
 * nRequested -- total requested locks of all types.
 * granted -- count of each lock type currently granted on the lock.
 * nGranted -- total granted locks of all types.
 *
 * Note: these counts count 1 for each backend.  Internally to a backend,
 * there may be multiple grabs on a particular lock, but this is not reflected
 * into shared memory.
 */
typedef struct LOCK {
    /* hash key */
    LOCKTAG tag; /* unique identifier of lockable object */

    /* data */
    LOCKMASK grantMask;           /* bitmask for lock types already granted */
    LOCKMASK waitMask;            /* bitmask for lock types awaited */
    SHM_QUEUE procLocks;          /* list of PROCLOCK objects assoc. with lock */
    PROC_QUEUE waitProcs;         /* list of PGPROC objects waiting on lock */
    int requested[MAX_LOCKMODES]; /* counts of requested locks */
    int nRequested;               /* total of requested[] array */
    int granted[MAX_LOCKMODES];   /* counts of granted locks */
    int nGranted;                 /* total of granted[] array */
} LOCK;

#define LOCK_LOCKMETHOD(lock) ((LOCKMETHODID)(lock).tag.locktag_lockmethodid)
#define SSLOCK_LOCKMETHOD(locallock) ((LOCKMETHODID)(locallock).tag.lock.locktag_lockmethodid)

/*
 * We may have several different backends holding or awaiting locks
 * on the same lockable object.  We need to store some per-holder/waiter
 * information for each such holder (or would-be holder).  This is kept in
 * a PROCLOCK struct.
 *
 * PROCLOCKTAG is the key information needed to look up a PROCLOCK item in the
 * proclock hashtable.	A PROCLOCKTAG value uniquely identifies the combination
 * of a lockable object and a holder/waiter for that object.  (We can use
 * pointers here because the PROCLOCKTAG need only be unique for the lifespan
 * of the PROCLOCK, and it will never outlive the lock or the proc.)
 *
 * Internally to a backend, it is possible for the same lock to be held
 * for different purposes: the backend tracks transaction locks separately
 * from session locks.	However, this is not reflected in the shared-memory
 * state: we only track which backend(s) hold the lock.  This is OK since a
 * backend can never block itself.
 *
 * The holdMask field shows the already-granted locks represented by this
 * proclock.  Note that there will be a proclock object, possibly with
 * zero holdMask, for any lock that the process is currently waiting on.
 * Otherwise, proclock objects whose holdMasks are zero are recycled
 * as soon as convenient.
 *
 * releaseMask is workspace for LockReleaseAll(): it shows the locks due
 * to be released during the current call.	This must only be examined or
 * set by the backend owning the PROCLOCK.
 *
 * Each PROCLOCK object is linked into lists for both the associated LOCK
 * object and the owning PGPROC object.  Note that the PROCLOCK is entered
 * into these lists as soon as it is created, even if no lock has yet been
 * granted.  A PGPROC that is waiting for a lock to be granted will also be
 * linked into the lock's waitProcs queue.
 */
typedef struct PROCLOCKTAG {
    /* NB: we assume this struct contains no padding! */
    LOCK* myLock;   /* link to per-lockable-object information */
    PGPROC* myProc; /* link to PGPROC of owning backend */
} PROCLOCKTAG;

typedef struct PROCLOCK {
    /* tag */
    PROCLOCKTAG tag; /* unique identifier of proclock object */

    /* data */
    PGPROC  *groupLeader; /* group leader, or NULL if no lock group */	
    LOCKMASK holdMask;    /* bitmask for lock types currently held */
    LOCKMASK releaseMask; /* bitmask for lock types to be released */
    SHM_QUEUE lockLink;   /* list link in LOCK's list of proclocks */
    SHM_QUEUE procLink;   /* list link in PGPROC's list of proclocks */
} PROCLOCK;

#define PROCLOCK_LOCKMETHOD(proclock) LOCK_LOCKMETHOD(*((proclock).tag.myLock))

/*
 * Each backend also maintains a local hash table with information about each
 * lock it is currently interested in.	In particular the local table counts
 * the number of times that lock has been acquired.  This allows multiple
 * requests for the same lock to be executed without additional accesses to
 * shared memory.  We also track the number of lock acquisitions per
 * ResourceOwner, so that we can release just those locks belonging to a
 * particular ResourceOwner.
 *
 * When holding a lock taken "normally", the lock and proclock fields always
 * point to the associated objects in shared memory.  However, if we acquired
 * the lock via the fast-path mechanism, the lock and proclock fields are set
 * to NULL, since there probably aren't any such objects in shared memory.
 * (If the lock later gets promoted to normal representation, we may eventually
 * update our locallock's lock/proclock fields after finding the shared
 * objects.)
 *
 * Caution: a locallock object can be left over from a failed lock acquisition
 * attempt.  In this case its lock/proclock fields are untrustworthy, since
 * the shared lock object is neither held nor awaited, and hence is available
 * to be reclaimed.  If nLocks > 0 then these pointers must either be valid or
 * NULL, but when nLocks == 0 they should be considered garbage.
 */
typedef struct LOCALLOCKTAG {
    LOCKTAG lock;  /* identifies the lockable object */
    LOCKMODE mode; /* lock mode for this table entry */
} LOCALLOCKTAG;

typedef struct LOCALLOCKOWNER {
    /*
     * Note: if owner is NULL then the lock is held on behalf of the session;
     * otherwise it is held on behalf of my current transaction.
     *
     * Must use a forward struct reference to avoid circularity.
     */
    struct ResourceOwnerData* owner;
    int64 nLocks; /* # of times held by this owner */
} LOCALLOCKOWNER;

typedef struct LOCALLOCK {
    /* tag */
    LOCALLOCKTAG tag; /* unique identifier of locallock entry */

    /* data */
    LOCK* lock;                 /* associated LOCK object, if any */
    PROCLOCK* proclock;         /* associated PROCLOCK object, if any */
    uint32 hashcode;            /* copy of LOCKTAG's hash value */
    int64 nLocks;               /* total number of times lock is held */
    int numLockOwners;          /* # of relevant ResourceOwners */
    int maxLockOwners;          /* allocated size of array */
    bool holdsStrongLockCount;  /* bumped FastPathStrongRelatonLocks */
    bool ssLock;                /* distribute lock in shared storage mode */
    LOCALLOCKOWNER* lockOwners; /* dynamically resizable array */
} LOCALLOCK;

#define LOCALLOCK_LOCKMETHOD(llock) ((llock).tag.lock.locktag_lockmethodid)

typedef struct GlobalSessionId {
    uint64 sessionId;  /* Increasing sequence num */
    uint32 nodeId;     /* the number of the send node */
    /* Used to identify the latest global sessionid during pooler reuse */
    uint64 seq;
} GlobalSessionId;
/*
 * These structures hold information passed from lmgr internals to the lock
 * listing user-level functions (in lockfuncs.c).
 */

typedef struct LockInstanceData {
    LOCKTAG locktag;         /* locked object */
    LOCKMASK holdMask;       /* locks held by this PGPROC */
    LOCKMODE waitLockMode;   /* lock awaited by this PGPROC, if any */
    BackendId backend;       /* backend ID of this PGPROC */
    LocalTransactionId lxid; /* local transaction ID of this PGPROC */
    ThreadId pid;            /* pid of this PGPROC */
    uint64 sessionid;        /* session id of this PGPROC */
    GlobalSessionId globalSessionId; /* global session id of this PGPROC */
    bool fastpath;           /* taken via fastpath? */
} LockInstanceData;

typedef struct LockData {
    int nelements; /* The length of the array */
    LockInstanceData* locks;
} LockData;

/* Result codes for LockAcquire() */
typedef enum {
    LOCKACQUIRE_NOT_AVAIL,   /* lock not available, and dontWait=true */
    LOCKACQUIRE_OK,          /* lock successfully acquired */
    LOCKACQUIRE_ALREADY_HELD /* incremented count for lock already held */
} LockAcquireResult;

/* Deadlock states identified by DeadLockCheck() */
typedef enum {
    DS_NOT_YET_CHECKED,          /* no deadlock check has run yet */
    DS_LOCK_TIMEOUT,             /* lock acuqire timeout */
    DS_NO_DEADLOCK,              /* no deadlock detected */
    DS_SOFT_DEADLOCK,            /* deadlock avoided by queue rearrangement */
    DS_HARD_DEADLOCK,            /* deadlock, no way out but ERROR */
    DS_BLOCKED_BY_AUTOVACUUM,    /* no deadlock; queue blocked by autovacuum worker */
    DS_BLOCKED_BY_REDISTRIBUTION /* no deadlock; queue blocked by data redistribution */
} DeadLockState;

/*
 * The lockmgr's shared hash tables are partitioned to reduce contention.
 * To determine which partition a given locktag belongs to, compute the tag's
 * hash code with LockTagHashCode(), then apply one of these macros.
 * NB: NUM_LOCK_PARTITIONS must be a power of 2!
 */
#define LockHashPartition(hashcode) ((hashcode) % NUM_LOCK_PARTITIONS)
#define LockHashPartitionLock(hashcode) \
	(&t_thrd.shemem_ptr_cxt.mainLWLockArray[FirstLockMgrLock + LockHashPartition(hashcode)].lock)

/*
 * The deadlock detector needs to be able to access lockGroupLeader and
 * related fields in the PGPROC, so we arrange for those fields to be protected
 * by one of the lock hash partition locks.  Since the deadlock detector
 * acquires all such locks anyway, this makes it safe for it to access these
 * fields without doing anything extra.  To avoid contention as much as
 * possible, we map different PGPROCs to different partition locks.  The lock
 * used for a given lock group is determined by the group leader's pgprocno.
 */
#define LockHashPartitionLockByProc(leader_pgproc) \
    LockHashPartitionLock((leader_pgproc)->pgprocno)

/* Macros for manipulating proc->fpLockBits */
#define FAST_PATH_BITS_PER_SLOT 3
#define FAST_PATH_LOCKNUMBER_OFFSET 1
#define FAST_PATH_MASK ((1 << FAST_PATH_BITS_PER_SLOT) - 1)
#define FAST_PATH_GET_BITS(proc, n) (((proc)->fpLockBits[n / FP_LOCK_SLOTS_PER_LOCKBIT] \
    >> (FAST_PATH_BITS_PER_SLOT * (n % FP_LOCK_SLOTS_PER_LOCKBIT))) & FAST_PATH_MASK)
#define FAST_PATH_BIT_POSITION(n, l)                                              \
    (AssertMacro((l) >= FAST_PATH_LOCKNUMBER_OFFSET),                             \
     AssertMacro((l) < FAST_PATH_BITS_PER_SLOT + FAST_PATH_LOCKNUMBER_OFFSET), \
     AssertMacro((n) < FP_LOCK_SLOTS_PER_LOCKBIT),                             \
     ((l) - FAST_PATH_LOCKNUMBER_OFFSET + FAST_PATH_BITS_PER_SLOT * (n)))

#define FAST_PATH_SET_LOCKMODE(proc, n, l) \
    (proc)->fpLockBits[n / FP_LOCK_SLOTS_PER_LOCKBIT] |= \
    UINT64CONST(UINT64CONST(1) << FAST_PATH_BIT_POSITION((n % FP_LOCK_SLOTS_PER_LOCKBIT), l))
#define FAST_PATH_CLEAR_LOCKMODE(proc, n, l) \
    (proc)->fpLockBits[n / FP_LOCK_SLOTS_PER_LOCKBIT] &= \
    ~(UINT64CONST(UINT64CONST(1) << FAST_PATH_BIT_POSITION((n % FP_LOCK_SLOTS_PER_LOCKBIT), l)))
#define FAST_PATH_CHECK_LOCKMODE(proc, n, l) \
    ((proc)->fpLockBits[n / FP_LOCK_SLOTS_PER_LOCKBIT] & \
    (UINT64CONST(UINT64CONST(1) << FAST_PATH_BIT_POSITION((n % FP_LOCK_SLOTS_PER_LOCKBIT), l))))

#define PRINT_WAIT_LENTH (8 + 1)
#define CHECK_LOCKMETHODID(lockMethodId)                                             \
    do {                                                 \
        if (unlikely((lockMethodId) == 0 || (lockMethodId) >= lengthof(LockMethods))) {  \
            ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),              \
                            errmsg("unrecognized lock method: %hu", (lockMethodId))));               \
        }                                                    \
    } while (0)
#define CHECK_LOCKMODE(lockMode, lockMethodTable)                                    \
    do {                                                 \
        if (unlikely((lockMode) <= 0 || (lockMode) > (lockMethodTable)->numLockModes)) { \
            ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),                          \
                            errmsg("unrecognized lock mode: %d", (lockMode))));                      \
        }                                                    \
    } while (0)

/*
 * The fast-path lock mechanism is concerned only with relation locks on
 * unshared relations by backends bound to a database.	The fast-path
 * mechanism exists mostly to accelerate acquisition and release of locks
 * that rarely conflict.  Because ShareUpdateExclusiveLock is
 * self-conflicting, it can't use the fast-path mechanism; but it also does
 * not conflict with any of the locks that do, so we can ignore it completely.
 */
#define EligibleForRelationFastPath(locktag, mode)                                                    \
    ((locktag)->locktag_lockmethodid == DEFAULT_LOCKMETHOD &&                                         \
     ((locktag)->locktag_type == LOCKTAG_RELATION || (locktag)->locktag_type == LOCKTAG_PARTITION) && \
     (mode) < ShareUpdateExclusiveLock)
#define ConflictsWithRelationFastPath(locktag, mode)                                                  \
    ((locktag)->locktag_lockmethodid == DEFAULT_LOCKMETHOD &&                                         \
     ((locktag)->locktag_type == LOCKTAG_RELATION || (locktag)->locktag_type == LOCKTAG_PARTITION) && \
     (mode) > ShareUpdateExclusiveLock)

/*
 * To make the fast-path lock mechanism work, we must have some way of
 * preventing the use of the fast-path when a conflicting lock might be
 * present.  We partition* the locktag space into FAST_PATH_HASH_BUCKETS
 * partitions, and maintain an integer count of the number of "strong" lockers
 * in each partition.  When any "strong" lockers are present (which is
 * hopefully not very often), the fast-path mechanism can't be used, and we
 * must fall back to the slower method of pushing matching locks directly
 * into the main lock tables.
 *
 * The deadlock detector does not know anything about the fast path mechanism,
 * so any locks that might be involved in a deadlock must be transferred from
 * the fast-path queues to the main lock table.
 */
#define FAST_PATH_STRONG_LOCK_HASH_BITS 10
#define FAST_PATH_STRONG_LOCK_HASH_PARTITIONS (1 << FAST_PATH_STRONG_LOCK_HASH_BITS)
#define FastPathStrongLockHashPartition(hashcode) ((hashcode) % FAST_PATH_STRONG_LOCK_HASH_PARTITIONS)

typedef struct FastPathStrongRelationLockData {
    slock_t mutex;
    uint32 count[FAST_PATH_STRONG_LOCK_HASH_PARTITIONS];
} FastPathStrongRelationLockData;

/* Record that's written to 2PC state file when a lock is persisted */
typedef struct TwoPhaseLockRecord {
    LOCKTAG locktag;
    LOCKMODE lockmode;
} TwoPhaseLockRecord;

/*
 * function prototypes
 */
extern void InitLocks(void);
extern LockMethod GetLocksMethodTable(const LOCK *lock);
extern uint32 LockTagHashCode(const LOCKTAG *locktag);
extern bool DoLockModesConflict(LOCKMODE mode1, LOCKMODE mode2);
extern LockAcquireResult LockAcquire(const LOCKTAG *locktag, LOCKMODE lockmode, bool sessionLock, bool dontWait,
                                     bool allow_con_update = false, int waitSec = 0);
extern bool LockIncrementIfExists(const LOCKTAG *locktag, LOCKMODE lockmode, bool sessionLock);
extern LockAcquireResult LockAcquireExtended(const LOCKTAG *locktag, LOCKMODE lockmode, bool sessionLock, bool dontWait,
                                             bool report_memory_error, bool allow_con_update = false, int waitSec = 0);
extern void AbortStrongLockAcquire(void);
extern bool LockRelease(const LOCKTAG* locktag, LOCKMODE lockmode, bool sessionLock);
extern void ReleaseLockIfHeld(const LOCKTAG *locktag, LOCKMODE lockmode, bool sessionLock);
extern void LockReleaseAll(LOCKMETHODID lockmethodid, bool allLocks);
extern void Check_FastpathBit();

extern void LockReleaseSession(LOCKMETHODID lockmethodid);
extern void LockReleaseCurrentOwner(void);
extern void LockReassignCurrentOwner(void);
extern bool LockHasWaiters(const LOCKTAG* locktag, LOCKMODE lockmode, bool sessionLock);
extern VirtualTransactionId* GetLockConflicts(const LOCKTAG* locktag, LOCKMODE lockmode);
extern void AtPrepare_Locks(void);
extern void PostPrepare_Locks(TransactionId xid);
extern int LockCheckConflicts(LockMethod lockMethodTable, LOCKMODE lockmode, LOCK *lock, PROCLOCK *proclock,
                              PGPROC *proc);
extern void GrantLock(LOCK *lock, PROCLOCK *proclock, LOCKMODE lockmode);
extern void GrantAwaitedLock(void);
extern void RemoveFromWaitQueue(PGPROC* proc, uint32 hashcode);
extern Size LockShmemSize(void);
extern LockData* GetLockStatusData(void);

extern void ReportLockTableError(bool report);

typedef struct xl_standby_lock {
    TransactionId xid; /* xid of holder of AccessExclusiveLock */
    Oid dbOid;
    Oid relOid;
} xl_standby_lock;

typedef struct XlStandbyLockNew {
    TransactionId xid; /* xid of holder of ACCESS_EXCLUSIVE_LOCK */
    Oid dbOid;
    Oid relOid;
    uint32 seq;
} XlStandbyLockNew;

extern xl_standby_lock* GetRunningTransactionLocks(int* nlocks);
extern const char* GetLockmodeName(LOCKMETHODID lockmethodid, LOCKMODE mode);

extern void lock_twophase_recover(TransactionId xid, uint16 info, void* recdata, uint32 len);
extern void lock_twophase_postcommit(TransactionId xid, uint16 info, void* recdata, uint32 len);
extern void lock_twophase_postabort(TransactionId xid, uint16 info, void* recdata, uint32 len);

extern DeadLockState DeadLockCheck(PGPROC* proc);
extern PGPROC* GetBlockingAutoVacuumPgproc(void);
extern PGPROC* GetBlockingRedistributionPgproc(void);
extern void DeadLockReport(void);
extern void RememberSimpleDeadLock(PGPROC* proc1, LOCKMODE lockmode, LOCK* lock, PGPROC* proc2);
extern void InitDeadLockChecking(void);

extern int LockWaiterCount(const LOCKTAG* locktag);
extern bool IsOtherProcRedistribution(PGPROC* otherProc);
#if defined(LOCK_DEBUG) || defined(USE_ASSERT_CHECKING)
extern void DumpLocks(PGPROC* proc);
extern void DumpAllLocks(void);
#endif

/* Lock a VXID (used to wait for a transaction to finish) */
extern void VirtualXactLockTableInsert(const VirtualTransactionId& vxid);
extern void VirtualXactLockTableCleanup(void);
extern bool VirtualXactLock(const VirtualTransactionId& vxid, bool wait);
extern char* LocktagToString(LOCKTAG locktag);

#endif /* LOCK_H */
