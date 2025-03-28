/* -------------------------------------------------------------------------
 *
 * publicationcmds.c
 * 		publication manipulation
 *
 * Copyright (c) 2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 * 		publicationcmds.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"

#include "access/genam.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/xact.h"
#include "access/sysattr.h"

#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_inherits_fn.h"
#include "catalog/pg_type.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/dependency.h"
#include "parser/parser.h"

#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/publicationcmds.h"
#include "nodes/makefuncs.h"

#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/acl.h"


/* Same as MAXNUMMESSAGES in sinvaladt.c */
static const int MAX_RELCACHE_INVAL_MSGS = 4096;

static List *OpenTableList(List *tables);
static void CloseTableList(List *rels);
static void PublicationAddTables(Oid pubid, List *rels, bool if_not_exists, AlterPublicationStmt *stmt);
static void PublicationDropTables(Oid pubid, List *rels, bool missing_ok);

static void parse_publication_options(List *options,
                                      bool *publish_given,
                                      bool *publish_insert,
                                      bool *publish_update,
                                      bool *publish_delete,
                                      bool *publish_truncate,
                                      bool *publish_ddl_given,
                                      int64 *pubddl)
{
    ListCell *lc;

    *publish_given = false;
    *publish_ddl_given = false;

    /* Defaults are true */
    *publish_insert = true;
    *publish_update = true;
    *publish_delete = true;
    *publish_truncate = true;
    *pubddl = 0;

    /* Parse options */
    foreach (lc, options) {
        DefElem *defel = (DefElem *)lfirst(lc);

        if (strcmp(defel->defname, "publish") == 0) {
            char *publish;
            List *publish_list;
            ListCell *lc;

            if (*publish_given)
                ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("conflicting or redundant options")));

            /*
             * If publish option was given only the explicitly listed actions
             * should be published.
             */
            *publish_insert = false;
            *publish_update = false;
            *publish_delete = false;
            *publish_truncate = false;

            *publish_given = true;
            publish = defGetString(defel);
            if (!SplitIdentifierString(publish, ',', &publish_list))
                ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("invalid list syntax for \"publish\" option")));

            /* Process the option list. */
            foreach (lc, publish_list) {
                char *publish_opt = (char *)lfirst(lc);

                if (strcmp(publish_opt, "insert") == 0)
                    *publish_insert = true;
                else if (strcmp(publish_opt, "update") == 0)
                    *publish_update = true;
                else if (strcmp(publish_opt, "delete") == 0)
                    *publish_delete = true;
                else if (strcmp(publish_opt, "truncate") == 0)
                    *publish_truncate = true;
                else
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR), errmsg("unrecognized \"publish\" value: \"%s\"", publish_opt)));
            }
        } else if (pubddl && strcmp(defel->defname, "ddl") == 0) {
            char *ddl_types;
            List *ddl_type_list;
            ListCell *lc2;

            if (*publish_ddl_given)
                ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("conflicting or redundant options")));
            
            *publish_ddl_given = true;
            *pubddl = 0;

            /*
             * If ddl option was given only the explicitly listed ddl types
             * should be published.
             */
            ddl_types = defGetString(defel);

            if (!SplitIdentifierString(ddl_types, ',', &ddl_type_list))
                ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("invalid list syntax for \"ddl\" option")));

            /* Process the option list */
            foreach (lc2, ddl_type_list) {
                char *ddl_opt = (char *)lfirst(lc2);

                if (strcmp(ddl_opt, "table") == 0)
                    *pubddl |= PUBDDL_TABLE;
                else if (strcmp(ddl_opt, "none") == 0)
                    *pubddl = 0;
                else if (strcmp(ddl_opt, "all") == 0)
                    *pubddl = PUBDDL_ALL;
                else
                    ereport(ERROR,
                        (errcode(ERRCODE_SYNTAX_ERROR), errmsg("unrecognized \"ddl\" value: \"%s\"", ddl_opt)));
            }
        } else
            ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR), errmsg("unrecognized publication parameter: %s", defel->defname)));
    }
}

/*
 * Create event trigger which is used for DDL replication
 */
static void
CreateDDLReplicaEventTrigger(char *eventname, List *commands, ObjectAddress pubaddress, Oid puboid)
{
    List *tags = NIL;
    ListCell *lc;
    Oid trigger_id;
    ObjectAddress referenced;
    CreateEventTrigStmt *ddl_trigger;
    char trigger_name[NAMEDATALEN];
    char trigger_func_name[NAMEDATALEN];
    errno_t rc;
    static const char *trigger_func_prefix = "publication_deparse_%s";

    ddl_trigger = makeNode(CreateEventTrigStmt);

    rc = snprintf_s(trigger_name, sizeof(trigger_name), sizeof(trigger_name) - 1, PUB_EVENT_TRIG_FORMAT, eventname, puboid);
    securec_check_ss(rc, "\0", "\0");
    rc = snprintf_s(trigger_func_name, sizeof(trigger_func_name), sizeof(trigger_func_name) - 1, trigger_func_prefix, eventname);
    securec_check_ss(rc, "\0", "\0");

    ddl_trigger->trigname = pstrdup(trigger_name);
    ddl_trigger->eventname = eventname;
    ddl_trigger->funcname = SystemFuncName(trigger_func_name);

    foreach(lc, commands) {
        Value *tag = (Value*)lfirst(lc);
        tags = lappend(tags, tag);
    }

    ddl_trigger->whenclause = list_make1(makeDefElem("tag", (Node*)tags));

    trigger_id = CreateEventTrigger(ddl_trigger);

    /*
     * Register the event triggers as internally dependent on the publication.
     */
    ObjectAddressSet(referenced, EventTriggerRelationId, trigger_id);
    recordDependencyOn(&referenced, &pubaddress, DEPENDENCY_INTERNAL);
}

static void AddAllDDLReplicaEventTriggers(List *end_commands)
{
    end_commands = lappend(end_commands, makeString("CREATE INDEX"));
    end_commands = lappend(end_commands, makeString("DROP INDEX"));
    end_commands = lappend(end_commands, makeString("ALTER INDEX"));
    end_commands = lappend(end_commands, makeString("CREATE SEQUENCE"));
    end_commands = lappend(end_commands, makeString("ALTER SEQUENCE"));
    end_commands = lappend(end_commands, makeString("DROP SEQUENCE"));
    end_commands = lappend(end_commands, makeString("CREATE SCHEMA"));
    end_commands = lappend(end_commands, makeString("ALTER SCHEMA"));
    end_commands = lappend(end_commands, makeString("DROP SCHEMA"));
    end_commands = lappend(end_commands, makeString("COMMENT"));
    end_commands = lappend(end_commands, makeString("CREATE VIEW"));
    end_commands = lappend(end_commands, makeString("ALTER VIEW"));
    end_commands = lappend(end_commands, makeString("DROP VIEW"));
    end_commands = lappend(end_commands, makeString("CREATE FUNCTION"));
    end_commands = lappend(end_commands, makeString("ALTER FUNCTION"));
    end_commands = lappend(end_commands, makeString("DROP FUNCTION"));
    end_commands = lappend(end_commands, makeString("CREATE TRIGGER"));
    end_commands = lappend(end_commands, makeString("ALTER TRIGGER"));
    end_commands = lappend(end_commands, makeString("DROP TRIGGER"));
    end_commands = lappend(end_commands, makeString("CREATE TYPE"));
    end_commands = lappend(end_commands, makeString("ALTER TYPE"));
    end_commands = lappend(end_commands, makeString("DROP TYPE"));
    if(DB_IS_CMPT(B_FORMAT)) {
        end_commands = lappend(end_commands, makeString("CREATE EVENT"));
        end_commands = lappend(end_commands, makeString("ALTER EVENT"));
        end_commands = lappend(end_commands, makeString("DROP EVENT"));
    }
}

/*
 * If DDL replication is enabled, create event triggers to capture and log any
 * relevant events.
 */
static void CreateDDLReplicaEventTriggers(ObjectAddress pubaddress, Oid puboid)
{
    List *start_commands = list_make1(makeString("DROP TABLE"));
    start_commands = lappend(start_commands, makeString("DROP INDEX"));
    start_commands = lappend(start_commands, makeString("DROP TYPE"));

    List *rewrite_commands = list_make1(makeString("ALTER TABLE"));

    List *end_commands = list_make1(makeString("CREATE TABLE"));
    end_commands = lappend(end_commands, makeString("ALTER TABLE"));
    end_commands = lappend(end_commands, makeString("DROP TABLE"));
    AddAllDDLReplicaEventTriggers(end_commands);

    /* Create the ddl_command_end event trigger */
    CreateDDLReplicaEventTrigger(PUB_TRIG_DDL_CMD_END, end_commands, pubaddress, puboid);

    /* Create the ddl_command_start event trigger */
    CreateDDLReplicaEventTrigger(PUB_TRIG_DDL_CMD_START, start_commands, pubaddress, puboid);

    /* Create the table_rewrite event trigger */
    CreateDDLReplicaEventTrigger(PUB_TRIG_TBL_REWRITE, rewrite_commands, pubaddress, puboid);
}

/*
 * Helper function to drop an event trigger for DDL replication.
 */
static void DropDDLReplicaEventTrigger(char *eventname, Oid puboid)
{
    char trigger_name[NAMEDATALEN];
    Oid evtoid;
    ObjectAddress obj;
    errno_t rc;

    rc = snprintf_s(trigger_name, sizeof(trigger_name), sizeof(trigger_name) - 1, PUB_EVENT_TRIG_FORMAT, eventname, puboid);
    securec_check_ss(rc, "\0", "\0");

    evtoid = get_event_trigger_oid(trigger_name, true);
    if (!OidIsValid(evtoid))
        return;

    deleteDependencyRecordsForClass(EventTriggerRelationId, evtoid, PublicationRelationId, DEPENDENCY_INTERNAL);

    /*
     * Ensure that the dependency removal is visible, so that we can drop the
     * event trigger.
     */
    CommandCounterIncrement();

    ObjectAddressSet(obj, EventTriggerRelationId, evtoid);
    performDeletion(&obj, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
}

/*
 * Drop all the event triggers which are used for DDL replication.
 */
static void DropDDLReplicaEventTriggers(Oid puboid)
{
    DropDDLReplicaEventTrigger(PUB_TRIG_DDL_CMD_START, puboid);
    DropDDLReplicaEventTrigger(PUB_TRIG_DDL_CMD_END, puboid);
    DropDDLReplicaEventTrigger(PUB_TRIG_TBL_REWRITE, puboid);
}

/*
 * Create new publication.
 */
ObjectAddress CreatePublication(CreatePublicationStmt *stmt)
{
    if (t_thrd.proc->workingVersionNum < PUBLICATION_VERSION_NUM) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("Before GRAND VERSION NUM %u, we do not support publication.", PUBLICATION_VERSION_NUM)));
    }

    Relation rel;
    ObjectAddress myself;
    Oid puboid;
    bool nulls[Natts_pg_publication];
    Datum values[Natts_pg_publication];
    HeapTuple tup;
    bool publish_given;
    bool publish_insert;
    bool publish_update;
    bool publish_delete;
    bool publish_truncate;
    bool publish_ddl_given;
    int64 pubddl;
    AclResult aclresult;
    int rc;

    /* must have CREATE privilege on database */
    aclresult = pg_database_aclcheck(u_sess->proc_cxt.MyDatabaseId, GetUserId(), ACL_CREATE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, ACL_KIND_DATABASE, get_database_name(u_sess->proc_cxt.MyDatabaseId));

    /* FOR ALL TABLES requires superuser */
    if (stmt->for_all_tables && !superuser())
        ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
            (errmsg("must be superuser to create FOR ALL TABLES publication"))));

    rel = heap_open(PublicationRelationId, RowExclusiveLock);

    /* Check if name is used */
    puboid = GetSysCacheOid1(PUBLICATIONNAME, CStringGetDatum(stmt->pubname));
    if (OidIsValid(puboid)) {
        ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("publication \"%s\" already exists", stmt->pubname)));
    }

    /* Form a tuple. */
    rc = memset_s(values, sizeof(values), 0, sizeof(values));
    securec_check(rc, "", "");
    rc = memset_s(nulls, sizeof(nulls), false, sizeof(nulls));
    securec_check(rc, "", "");

    values[Anum_pg_publication_pubname - 1] = DirectFunctionCall1(namein, CStringGetDatum(stmt->pubname));
    values[Anum_pg_publication_pubowner - 1] = ObjectIdGetDatum(GetUserId());

    parse_publication_options(stmt->options, &publish_given, &publish_insert, &publish_update, &publish_delete,
                              &publish_truncate, &publish_ddl_given, &pubddl);

    values[Anum_pg_publication_puballtables - 1] = BoolGetDatum(stmt->for_all_tables);
    values[Anum_pg_publication_pubinsert - 1] = BoolGetDatum(publish_insert);
    values[Anum_pg_publication_pubupdate - 1] = BoolGetDatum(publish_update);
    values[Anum_pg_publication_pubdelete - 1] = BoolGetDatum(publish_delete);
    values[Anum_pg_publication_pubddl - 1] = Int64GetDatum(pubddl);
    values[Anum_pg_publication_pubtruncate - 1] = BoolGetDatum(publish_truncate);

    tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

    if (pubddl == PUBDDL_ALL && !stmt->for_all_tables) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("DDL=all replication is only supported in FOR ALL TABLES publications")));
    }

    /* Insert tuple into catalog. */
    puboid = simple_heap_insert(rel, tup);
    CatalogUpdateIndexes(rel, tup);
    heap_freetuple(tup);

    recordDependencyOnOwner(PublicationRelationId, puboid, GetUserId());

    myself.classId = PublicationRelationId;
    myself.objectId = puboid;
    myself.objectSubId = 0;

    /* Make the changes visible. */
    CommandCounterIncrement();

    if (stmt->tables) {
        List *rels;

        Assert(list_length(stmt->tables) > 0);

        rels = OpenTableList(stmt->tables);
        PublicationAddTables(puboid, rels, true, NULL);
        CloseTableList(rels);
    } else if (stmt->for_all_tables) {
        /* Invalidate relcache so that publication info is rebuilt. */
        CacheInvalidateRelcacheAll();
    }

    /*
     * Create an event trigger to allow logging of DDL statements.
     */
    if (pubddl != PUBDDL_NONE) {
        CreateDDLReplicaEventTriggers(myself, puboid);
    }

    heap_close(rel, RowExclusiveLock);

    InvokeObjectAccessHook(OAT_POST_CREATE, PublicationRelationId, puboid, 0, NULL);

    if (g_instance.attr.attr_storage.wal_level != WAL_LEVEL_LOGICAL) {
        ereport(WARNING, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("wal_level is insufficient to publish logical changes"),
            errhint("Set wal_level to logical before creating subscriptions.")));
    }

    return myself;
}

/*
 * Change options of a publication.
 */
static void AlterPublicationOptions(AlterPublicationStmt *stmt, Relation rel, HeapTuple tup)
{
    bool nulls[Natts_pg_publication];
    bool replaces[Natts_pg_publication];
    Datum values[Natts_pg_publication];
    bool publish_given;
    bool publish_insert;
    bool publish_update;
    bool publish_delete;
    bool publish_truncate;
    bool publish_ddl_given;
    int64 pubddl;
    bool pubddl_change = false;
    int rc;
    ObjectAddress obj;
    Form_pg_publication pubform;
    pubform = (Form_pg_publication)GETSTRUCT(tup);

    parse_publication_options(stmt->options, &publish_given, &publish_insert, &publish_update, &publish_delete,
                              &publish_truncate, &publish_ddl_given, &pubddl);

    /* Everything ok, form a new tuple. */
    rc = memset_s(values, sizeof(values), 0, sizeof(values));
    securec_check(rc, "", "");
    rc = memset_s(nulls, sizeof(nulls), false, sizeof(nulls));
    securec_check(rc, "", "");
    rc = memset_s(replaces, sizeof(replaces), false, sizeof(replaces));
    securec_check(rc, "", "");

    if (publish_given) {
        values[Anum_pg_publication_pubinsert - 1] = BoolGetDatum(publish_insert);
        replaces[Anum_pg_publication_pubinsert - 1] = true;

        values[Anum_pg_publication_pubupdate - 1] = BoolGetDatum(publish_update);
        replaces[Anum_pg_publication_pubupdate - 1] = true;

        values[Anum_pg_publication_pubdelete - 1] = BoolGetDatum(publish_delete);
        replaces[Anum_pg_publication_pubdelete - 1] = true;

        values[Anum_pg_publication_pubtruncate - 1] = BoolGetDatum(publish_truncate);
        replaces[Anum_pg_publication_pubtruncate - 1] = true;
    }

    if (publish_ddl_given) {
        if (pubform->pubddl != pubddl) {
            if (pubddl == PUBDDL_NONE || pubform->pubddl == PUBDDL_NONE) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("can not enable or disable DDL replication for an existing publication, cause there maybe some subscriptor with different compatibility.")));
            }
            
            pubddl_change = true;
        }

        values[Anum_pg_publication_pubddl - 1] = Int64GetDatum(pubddl);
        replaces[Anum_pg_publication_pubddl - 1] = true;
    } else {
        pubddl = pubform->pubddl;
    }

    tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls, replaces);

    /* Update the catalog. */
    simple_heap_update(rel, &tup->t_self, tup);
    CatalogUpdateIndexes(rel, tup);

    CommandCounterIncrement();

    /* Invalidate the relcache. */
    if (((Form_pg_publication)GETSTRUCT(tup))->puballtables) {
        CacheInvalidateRelcacheAll();
    } else {
        if (pubddl == PUBDDL_ALL) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("DDL=all replication is only supported in FOR ALL TABLES publications")));
        }

        List *relids = GetPublicationRelations(HeapTupleGetOid(tup));

        /*
         * We don't want to send too many individual messages, at some point
         * it's cheaper to just reset whole relcache.
         */
        if (list_length(relids) < MAX_RELCACHE_INVAL_MSGS) {
            ListCell *lc;

            foreach (lc, relids) {
                Oid relid = lfirst_oid(lc);

                CacheInvalidateRelcacheByRelid(relid);
            }
        } else {
            CacheInvalidateRelcacheAll();
        }
    }
    ObjectAddressSet(obj, PublicationRelationId,  HeapTupleGetOid(tup));
    if (pubddl_change) {
        DropDDLReplicaEventTriggers(HeapTupleGetOid(tup));
        if (pubddl != PUBDDL_NONE) {
            CreateDDLReplicaEventTriggers(obj, HeapTupleGetOid(tup));
        }
    }
    EventTriggerCollectSimpleCommand(obj, InvalidObjectAddress, (Node *) stmt);
}

/*
 * Add or remove table to/from publication.
 */
static void AlterPublicationTables(AlterPublicationStmt *stmt, Relation rel, HeapTuple tup)
{
    Oid pubid = HeapTupleGetOid(tup);
    List *rels = NIL;
    Form_pg_publication pubform = (Form_pg_publication)GETSTRUCT(tup);
    /* Check that user is allowed to manipulate the publication tables. */
    if (pubform->puballtables)
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("publication \"%s\" is defined as FOR ALL TABLES", NameStr(pubform->pubname)),
            errdetail("Tables cannot be added to or dropped from FOR ALL TABLES publications.")));

    Assert(list_length(stmt->tables) > 0);

    rels = OpenTableList(stmt->tables);

    if (stmt->tableAction == DEFELEM_ADD)
        PublicationAddTables(pubid, rels, false, stmt);
    else if (stmt->tableAction == DEFELEM_DROP)
        PublicationDropTables(pubid, rels, false);
    else { /* DEFELEM_SET */
        List *oldrelids = GetPublicationRelations(pubid);
        List *delrels = NIL;
        ListCell *oldlc;

        /* Calculate which relations to drop. */
        foreach (oldlc, oldrelids) {
            Oid oldrelid = lfirst_oid(oldlc);
            ListCell *newlc;
            bool found = false;

            foreach (newlc, rels) {
                Relation newrel = (Relation)lfirst(newlc);
                if (RelationGetRelid(newrel) == oldrelid) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                Relation oldrel = heap_open(oldrelid, ShareUpdateExclusiveLock);
                delrels = lappend(delrels, oldrel);
            }
        }

        /* And drop them. */
        PublicationDropTables(pubid, delrels, true);

        /*
         * Don't bother calculating the difference for adding, we'll catch
         * and skip existing ones when doing catalog update.
         */
        PublicationAddTables(pubid, rels, true, stmt);

        CloseTableList(delrels);
    }

    CloseTableList(rels);
}

/*
 * Alter the existing publication.
 *
 * This is dispatcher function for AlterPublicationOptions and
 * AlterPublicationTables.
 */
void AlterPublication(AlterPublicationStmt *stmt)
{
    if (t_thrd.proc->workingVersionNum < PUBLICATION_VERSION_NUM) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("Before GRAND VERSION NUM %u, we do not support publication.", PUBLICATION_VERSION_NUM)));
    }

    Relation rel;
    HeapTuple tup;

    rel = heap_open(PublicationRelationId, RowExclusiveLock);

    tup = SearchSysCacheCopy1(PUBLICATIONNAME, CStringGetDatum(stmt->pubname));
    if (!HeapTupleIsValid(tup))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("publication \"%s\" does not exist", stmt->pubname)));

    /* must be owner */
    if (!pg_publication_ownercheck(HeapTupleGetOid(tup), GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PUBLICATION, stmt->pubname);

    if (stmt->options)
        AlterPublicationOptions(stmt, rel, tup);
    else
        AlterPublicationTables(stmt, rel, tup);

    /* Cleanup. */
    heap_freetuple(tup);
    heap_close(rel, RowExclusiveLock);
}

/*
 * Remove the publication by mapping OID.
 */
void RemovePublicationById(Oid pubid)
{
    Relation rel;
    HeapTuple tup;
    Form_pg_publication pubform;

    rel = heap_open(PublicationRelationId, RowExclusiveLock);

    tup = SearchSysCache1(PUBLICATIONOID, ObjectIdGetDatum(pubid));
    if (!HeapTupleIsValid(tup))
        elog(ERROR, "cache lookup failed for publication %u", pubid);

    pubform = (Form_pg_publication)GETSTRUCT(tup);
    /* Invalidate relcache so that publication info is rebuilt. */
    if (pubform->puballtables) {
        CacheInvalidateRelcacheAll();
    }

    simple_heap_delete(rel, &tup->t_self);
    ReleaseSysCache(tup);
    heap_close(rel, RowExclusiveLock);
}

/*
 * Remove relation from publication by mapping OID.
 */
void RemovePublicationRelById(Oid proid)
{
    Relation rel;
    HeapTuple tup;
    Form_pg_publication_rel pubrel;
    ScanKeyData scanKey[1];
    ScanKeyInit(&scanKey[0], ObjectIdAttributeNumber, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(proid));

    rel = heap_open(PublicationRelRelationId, RowExclusiveLock);
    SysScanDesc scanDesc = systable_beginscan(rel, PublicationRelObjectIndexId, true, NULL, 1, scanKey);
    tup = systable_getnext(scanDesc);
    if (!HeapTupleIsValid(tup)) {
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("could not find tuple for publication %u", proid)));
    }

    pubrel = (Form_pg_publication_rel)GETSTRUCT(tup);
    /* Invalidate relcache so that publication info is rebuilt. */
    CacheInvalidateRelcacheByRelid(pubrel->prrelid);

    simple_heap_delete(rel, &tup->t_self);

    systable_endscan(scanDesc);
    heap_close(rel, RowExclusiveLock);
}

/*
 * Open relations specified by a RangeVar list.
 * The returned tables are locked in ShareUpdateExclusiveLock mode.
 */
static List *OpenTableList(List *tables)
{
    List *relids = NIL;
    List *rels = NIL;
    ListCell *lc;

    /*
     * Open, share-lock, and check all the explicitly-specified relations
     */
    foreach (lc, tables) {
        RangeVar *rv = lfirst_node(RangeVar, lc);
        Relation rel;
        Oid myrelid;

        CHECK_FOR_INTERRUPTS();

        rel = heap_openrv(rv, ShareUpdateExclusiveLock);
        myrelid = RelationGetRelid(rel);
        /*
         * filter out duplicates when user specifies "foo, foo"
         * Note that this algorithm is known to not be very efficient (O(N^2))
         * but given that it only works on list of tables given to us by user
         * it's deemed acceptable.
         */
        if (list_member_oid(relids, myrelid)) {
            heap_close(rel, ShareUpdateExclusiveLock);
            continue;
        }
        rels = lappend(rels, rel);
        relids = lappend_oid(relids, myrelid);
    }

    list_free(relids);

    return rels;
}

/*
 * Close all relations in the list.
 */
static void CloseTableList(List *rels)
{
    ListCell *lc;

    foreach (lc, rels) {
        Relation rel = (Relation)lfirst(lc);

        heap_close(rel, NoLock);
    }
}

/*
 * Add listed tables to the publication.
 */
static void PublicationAddTables(Oid pubid, List *rels, bool if_not_exists, AlterPublicationStmt *stmt)
{
    ListCell *lc;

    Assert(!stmt || !stmt->for_all_tables);

    foreach (lc, rels) {
        Relation rel = (Relation)lfirst(lc);
        ObjectAddress obj;

        /* Must be owner of the table or superuser. */
        if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
            aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS, RelationGetRelationName(rel));

        obj = publication_add_relation(pubid, rel, if_not_exists);
        if (stmt) {
            InvokeObjectAccessHook(OAT_POST_CREATE, PublicationRelRelationId, obj.objectId, 0, NULL);
        }
    }
}

/*
 * Remove listed tables from the publication.
 */
static void PublicationDropTables(Oid pubid, List *rels, bool missing_ok)
{
    ObjectAddress obj;
    ListCell *lc;
    Oid prid;

    foreach (lc, rels) {
        Relation rel = (Relation)lfirst(lc);
        Oid relid = RelationGetRelid(rel);

        prid = GetSysCacheOid2(PUBLICATIONRELMAP, ObjectIdGetDatum(relid), ObjectIdGetDatum(pubid));
        if (!OidIsValid(prid)) {
            if (missing_ok)
                continue;

            ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                errmsg("relation \"%s\" is not part of the publication", RelationGetRelationName(rel))));
        }

        obj.classId = PublicationRelRelationId;
        obj.objectId = prid;
        obj.objectSubId = 0;
        performDeletion(&obj, DROP_CASCADE, 0);
    }
}

/*
 * Internal workhorse for changing a publication owner
 */
static void AlterPublicationOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
    Form_pg_publication form;

    form = (Form_pg_publication)GETSTRUCT(tup);
    if (form->pubowner == newOwnerId) {
        return;
    }

    if (!superuser()) {
        AclResult aclresult;

        /* Must be owner */
        if (!pg_publication_ownercheck(HeapTupleGetOid(tup), GetUserId()))
            aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PUBLICATION, NameStr(form->pubname));

        /* Must be able to become new owner */
        check_is_member_of_role(GetUserId(), newOwnerId);

        /* New owner must have CREATE privilege on database */
        aclresult = pg_database_aclcheck(u_sess->proc_cxt.MyDatabaseId, newOwnerId, ACL_CREATE);
        if (aclresult != ACLCHECK_OK)
            aclcheck_error(aclresult, ACL_KIND_DATABASE, get_database_name(u_sess->proc_cxt.MyDatabaseId));

        if (form->puballtables && !superuser_arg(newOwnerId))
            ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                errmsg("permission denied to change owner of publication \"%s\"", NameStr(form->pubname)),
                errhint("The owner of a FOR ALL TABLES publication must be a superuser.")));
    }

    form->pubowner = newOwnerId;
    simple_heap_update(rel, &tup->t_self, tup);
    CatalogUpdateIndexes(rel, tup);

    /* Update owner dependency reference */
    changeDependencyOnOwner(PublicationRelationId, HeapTupleGetOid(tup), newOwnerId);
}

/*
 * Change publication owner -- by name
 */
ObjectAddress AlterPublicationOwner(const char *name, Oid newOwnerId)
{
    Oid subid;
    HeapTuple tup;
    Relation rel;
    ObjectAddress address;

    rel = heap_open(PublicationRelationId, RowExclusiveLock);

    tup = SearchSysCacheCopy1(PUBLICATIONNAME, CStringGetDatum(name));
    if (!HeapTupleIsValid(tup))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("publication \"%s\" does not exist", name)));

    subid = HeapTupleGetOid(tup);

    AlterPublicationOwner_internal(rel, tup, newOwnerId);
    address.classId = PublicationRelationId;
    address.objectId = subid;
    address.objectSubId = 0;

    heap_freetuple(tup);
    heap_close(rel, RowExclusiveLock);

    return address;
}

/*
 * Change publication owner -- by OID
 */
void AlterPublicationOwner_oid(Oid subid, Oid newOwnerId)
{
    HeapTuple tup;
    Relation rel;

    rel = heap_open(PublicationRelationId, RowExclusiveLock);
    tup = SearchSysCacheCopy1(PUBLICATIONOID, ObjectIdGetDatum(subid));
    if (!HeapTupleIsValid(tup))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("publication with OID %u does not exist", subid)));

    AlterPublicationOwner_internal(rel, tup, newOwnerId);

    heap_freetuple(tup);

    heap_close(rel, RowExclusiveLock);
}
