/* -------------------------------------------------------------------------
 *
 * portal.h
 *	  openGauss portal definitions.
 *
 * A portal is an abstraction which represents the execution state of
 * a running or runnable query.  Portals support both SQL-level CURSORs
 * and protocol-level portals.
 *
 * Scrolling (nonsequential access) and suspension of execution are allowed
 * only for portals that contain a single SELECT-type query.  We do not want
 * to let the client suspend an update-type query partway through!	Because
 * the query rewriter does not allow arbitrary ON SELECT rewrite rules,
 * only queries that were originally update-type could produce multiple
 * plan trees; so the restriction to a single query is not a problem
 * in practice.
 *
 * For SQL cursors, we support three kinds of scroll behavior:
 *
 * (1) Neither NO SCROLL nor SCROLL was specified: to remain backward
 *	   compatible, we allow backward fetches here, unless it would
 *	   impose additional runtime overhead to do so.
 *
 * (2) NO SCROLL was specified: don't allow any backward fetches.
 *
 * (3) SCROLL was specified: allow all kinds of backward fetches, even
 *	   if we need to take a performance hit to do so.  (The planner sticks
 *	   a Materialize node atop the query plan if needed.)
 *
 * Case #1 is converted to #2 or #3 by looking at the query itself and
 * determining if scrollability can be supported without additional
 * overhead.
 *
 * Protocol-level portals have no nonsequential-fetch API and so the
 * distinction doesn't matter for them.  They are always initialized
 * to look like NO SCROLL cursors.
 *
 *
 * Portions Copyright (c) 2021, openGauss Contributors
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/portal.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef PORTAL_H
#define PORTAL_H

#include "datatype/timestamp.h"
#include "executor/exec/execdesc.h"
#include "utils/resowner.h"

/*
 * We have several execution strategies for Portals, depending on what
 * query or queries are to be executed.  (Note: in all cases, a Portal
 * executes just a single source-SQL query, and thus produces just a
 * single result from the user's viewpoint.  However, the rule rewriter
 * may expand the single source query to zero or many actual queries.)
 *
 * PORTAL_ONE_SELECT: the portal contains one single SELECT query.	We run
 * the Executor incrementally as results are demanded.	This strategy also
 * supports holdable cursors (the Executor results can be dumped into a
 * tuplestore for access after transaction completion).
 *
 * PORTAL_ONE_RETURNING: the portal contains a single INSERT/UPDATE/DELETE
 * query with a RETURNING clause (plus possibly auxiliary queries added by
 * rule rewriting).  On first execution, we run the portal to completion
 * and dump the primary query's results into the portal tuplestore; the
 * results are then returned to the client as demanded.  (We can't support
 * suspension of the query partway through, because the AFTER TRIGGER code
 * can't cope, and also because we don't want to risk failing to execute
 * all the auxiliary queries.)
 *
 * PORTAL_ONE_MOD_WITH: the portal contains one single SELECT query, but
 * it has data-modifying CTEs.	This is currently treated the same as the
 * PORTAL_ONE_RETURNING case because of the possibility of needing to fire
 * triggers.  It may act more like PORTAL_ONE_SELECT in future.
 *
 * PORTAL_UTIL_SELECT: the portal contains a utility statement that returns
 * a SELECT-like result (for example, EXPLAIN or SHOW).  On first execution,
 * we run the statement and dump its results into the portal tuplestore;
 * the results are then returned to the client as demanded.
 *
 * PORTAL_MULTI_QUERY: all other cases.  Here, we do not support partial
 * execution: the portal's queries will be run to completion on first call.
 */

/* the four attributes are defined in params.h: CURSOR_ISOPEN, CURSOR_FOUND, CURSOR_NOTFOUND, CURSOR_ROWCOUNT */
#define CURSOR_ATTRIBUTE_NUMBER    4

typedef enum PortalStrategy {
    PORTAL_ONE_SELECT,
    PORTAL_ONE_RETURNING,
    PORTAL_ONE_MOD_WITH,
    PORTAL_UTIL_SELECT,
    PORTAL_MULTI_QUERY
} PortalStrategy;

/*
 * A portal is always in one of these states.  It is possible to transit
 * from ACTIVE back to READY if the query is not run to completion;
 * otherwise we never back up in status.
 */
typedef enum PortalStatus {
    PORTAL_NEW,     /* freshly created */
    PORTAL_DEFINED, /* PortalDefineQuery done */
    PORTAL_READY,   /* PortalStart complete, can run it */
    PORTAL_ACTIVE,  /* portal is running (can't delete it) */
    PORTAL_DONE,    /* portal is finished (don't re-run it) */
    PORTAL_FAILED   /* portal got error (can't re-run it) */
} PortalStatus;
#ifndef ENABLE_MULTIPLE_NODES
typedef struct PortalStream {
    class StreamNodeGroup* streamGroup;
    MemoryContext stream_runtime_mem_cxt;
    MemoryContext data_exchange_mem_cxt;
    uint64 query_id;

    void Reset()
    {
        streamGroup = NULL;
        stream_runtime_mem_cxt = NULL;
        data_exchange_mem_cxt = NULL;
        query_id = 0;
    }

    void ResetEnv()
    {
        t_thrd.subrole = NO_SUBROLE;
        u_sess->stream_cxt.global_obj = NULL;
        u_sess->stream_cxt.stream_runtime_mem_cxt = NULL;
        u_sess->stream_cxt.data_exchange_mem_cxt = NULL;
        u_sess->debug_query_id = 0;
    }

    void ResetEnvForCursor()
    {
        Assert(u_sess->stream_cxt.global_obj != NULL);
        (void)MemoryContextSwitchTo(u_sess->top_portal_cxt);
        u_sess->stream_cxt.cursorNodeGroupList = lappend(u_sess->stream_cxt.cursorNodeGroupList,
            u_sess->stream_cxt.global_obj);
        ResetEnv();
    }

    void RecordSessionInfo()
    {
        streamGroup = u_sess->stream_cxt.global_obj;
        stream_runtime_mem_cxt = u_sess->stream_cxt.stream_runtime_mem_cxt;
        data_exchange_mem_cxt = u_sess->stream_cxt.data_exchange_mem_cxt;
        query_id = u_sess->debug_query_id;
    }

    void AttachToSession()
    {
        if (streamGroup != NULL || u_sess->stream_cxt.global_obj != NULL) {
            u_sess->stream_cxt.global_obj = streamGroup ?
                streamGroup : u_sess->stream_cxt.global_obj;
            u_sess->stream_cxt.stream_runtime_mem_cxt = stream_runtime_mem_cxt ?
                stream_runtime_mem_cxt : u_sess->stream_cxt.stream_runtime_mem_cxt;
            u_sess->stream_cxt.data_exchange_mem_cxt = data_exchange_mem_cxt ?
                data_exchange_mem_cxt : u_sess->stream_cxt.data_exchange_mem_cxt;
            u_sess->debug_query_id = query_id ?
                query_id : u_sess->debug_query_id;
        }
    }
} PortalStream;
#endif
typedef struct PortalData* Portal;

typedef struct PortalData {
    /* Bookkeeping data */
    const char* name;               /* portal's name */
    const char* prepStmtName;       /* source prepared statement (NULL if none) */
    MemoryContext heap;             /* subsidiary memory for portal */
    ResourceOwner resowner;         /* resources owned by portal */
    void (*cleanup)(Portal portal); /* cleanup hook */

    /*
     * State data for remembering which subtransaction(s) the portal was
     * created or used in.  If the portal is held over from a previous
     * transaction, both subxids are InvalidSubTransactionId.  Otherwise,
     * createSubid is the creating subxact and activeSubid is the last subxact
     * in which we ran the portal.
     */
    SubTransactionId createSubid; /* the creating subxact */
    SubTransactionId activeSubid; /* the last subxact with activity */

    /* The query or queries the portal will execute */
    const char* sourceText; /* text of query (as of 8.4, never NULL) */
    const char* commandTag; /* command tag for original query */
    List* stmts;            /* PlannedStmts and/or utility statements */
    CachedPlan* cplan;      /* CachedPlan, if stmts are from one */

    ParamListInfo portalParams; /* params to pass to query */

    /* Features/options */
    PortalStrategy strategy; /* see above */
    int cursorOptions;       /* DECLARE CURSOR option bits */
    Oid cursorHoldUserId;    /* Record UserId when cursor with hold */
    int cursorHoldSecRestrictionCxt;
    
    /* Status data */
    PortalStatus status; /* see above */
    bool portalPinned;   /* a pinned portal can't be dropped */
	bool autoHeld;		/* was automatically converted from pinned to
								 * held (see HoldPinnedPortals()) */

    /* If not NULL, Executor is active; call ExecutorEnd eventually: */
    QueryDesc* queryDesc; /* info needed for executor invocation */

    TableScanDesc scanDesc;  // info needed for reducing memory allocation
                            // when reusing the portal for too many times
                            //(e.g., FETCH/MOVE cursor in a loop)

    /* If portal returns tuples, this is their tupdesc: */
    TupleDesc tupDesc; /* descriptor for result tuples */
    /* and these are the format codes to use for the columns: */
    int16* formats; /* a format code for each column */

    /*
     * Where we store tuples for a held cursor or a PORTAL_ONE_RETURNING or
     * PORTAL_UTIL_SELECT query.  (A cursor held past the end of its
     * transaction no longer has any active executor state.)
     */
    Tuplestorestate* holdStore; /* store for holdable cursors */
    MemoryContext holdContext;  /* memory containing holdStore */

    /*
     * atStart, atEnd and portalPos indicate the current cursor position.
     * portalPos is zero before the first row, N after fetching N'th row of
     * query.  After we run off the end, portalPos = # of rows in query, and
     * atEnd is true.  If portalPos overflows, set posOverflow (this causes us
     * to stop relying on its value for navigation).  Note that atStart
     * implies portalPos == 0, but not the reverse (portalPos could have
     * overflowed).
     */
    bool atStart;
    bool atEnd;
    bool posOverflow;
    long portalPos;
    long commitPortalPos;
    bool hasStreamForPlpgsql; /* true if plpgsql's portal has stream may cause hang in for-loop */

    /* Presentation data, primarily used by the pg_cursors system view */
    TimestampTz creation_time; /* time at which this portal was defined */
    bool visible;              /* include this portal in pg_cursors? */
    /* Data used by workload manager. */
    int stmtMemCost; /* Estimate of memory usage from the plan    */
    void* cursorAttribute[CURSOR_ATTRIBUTE_NUMBER];
    Oid funcOid; /* function oid */
    int funcUseCount;
    MemoryContext copyCxt;             /*  memory for gpc copy plan */
    bool is_from_spi;
    /*
     * have_rollback_transaction parameter has dirty read and rollback is available.
     * This parameter is used to report an error when the cursor is rolled back.
     */
    bool have_rollback_transaction;
#ifndef ENABLE_MULTIPLE_NODES
    PortalStream streamInfo;
    bool isAutoOutParam;  /* is autonomous transaction procedure out param? */
    bool isPkgCur; /* cursor variable is a package variable? */
#endif
    int nextval_default_expr_type; /* nextval does not support lightproxy and sqlbypass */
    List *specialDataList;
    /*
     * A specific data link list hung under the portal.
     * The special data mounted in the portal can be used during cleaning,such as utl_tcp.
     */
} PortalData;

/*
 * PortalIsValid
 *		True iff portal is valid.
 */
#define PortalIsValid(p) PointerIsValid(p)

/*
 * Access macros for Portal ... use these in preference to field access.
 */
#define PortalGetQueryDesc(portal) ((portal)->queryDesc)
#define PortalGetHeapMemory(portal) ((portal)->heap)
#define PortalGetPrimaryStmt(portal) PortalListGetPrimaryStmt((portal)->stmts)

/* Prototypes for functions in utils/mmgr/portalmem.c */
extern void EnablePortalManager(void);
extern bool PreCommit_Portals(bool isPrepare = true, bool STP_commit = false);
extern void AtAbort_Portals(bool STP_rollback = false);
extern void AtCleanup_Portals(void);
extern void PortalErrorCleanup(void);
extern void AtSubCommit_Portals(SubTransactionId mySubid, SubTransactionId parentSubid, ResourceOwner parentXactOwner);
extern void AtSubAbort_Portals(SubTransactionId mySubid, SubTransactionId parentSubid,
    ResourceOwner myXactOwner, ResourceOwner parentXactOwner, bool inSTP);
extern void AtSubCleanup_Portals(SubTransactionId mySubid);
extern Portal CreatePortal(const char* name, bool allowDup, bool dupSilent, bool is_from_spi = false, bool is_from_pbe = false);
extern Portal CreateNewPortal(bool is_from_spi = false);
extern void PinPortal(Portal portal);
extern void UnpinPortal(Portal portal);
extern void MarkPortalActive(Portal portal);
extern void MarkPortalDone(Portal portal);
extern void MarkPortalFailed(Portal portal);
extern void PortalDrop(Portal portal, bool isTopCommit);
extern Portal GetPortalByName(const char* name);
extern void PortalDefineQuery(Portal portal, const char* prepStmtName, const char* sourceText, const char* commandTag,
    List* stmts, CachedPlan* cplan);
extern Node* PortalListGetPrimaryStmt(List* stmts);
extern void PortalCreateHoldStore(Portal portal);
extern void PortalHashTableDeleteAll(void);
extern bool ThereAreNoReadyPortals(void);
extern void ResetPortalCursor(SubTransactionId mySubid, Oid funOid, int funUseCount, bool reset = true);
extern void HoldPinnedPortals(bool is_rollback = false);
extern void HoldPortal(Portal portal, bool is_rollback = false);
#endif /* PORTAL_H */
