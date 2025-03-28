/* -------------------------------------------------------------------------
 *
 * common.c
 *	Catalog routines used by pg_dump; long ago these were shared
 *	by another dump tool, but not anymore.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2021, openGauss Contributors
 *
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/common.c
 *
 * -------------------------------------------------------------------------
 */
#include "pg_backup_archiver.h"
#include "catalog/pg_class.h"
#include "dumpmem.h"
#include "pg_dump.h"

#ifdef GAUSS_SFT_TEST
#include "gauss_sft.h"
#endif
#define FirstNormalObjectId 16384

/*
 * Variables for mapping DumpId to DumpableObject
 */
static DumpableObject** dumpIdMap = NULL;
static int allocedDumpIds = 0;
static DumpId lastDumpId = 0;

/*
 * Variables for mapping CatalogId to DumpableObject
 */
static bool catalogIdMapValid = false;
static DumpableObject** catalogIdMap = NULL;
static int numCatalogIds = 0;

/*
 * These variables are static to avoid the notational cruft of having to pass
 * them into findTableByOid() and friends.	For each of these arrays, we
 * build a sorted-by-OID index array immediately after it's built, and then
 * we use binary search in findTableByOid() and friends.  (qsort'ing the base
 * arrays themselves would be simpler, but it doesn't work because pg_dump.c
 * may have already established pointers between items.)
 */
static TableInfo* tblinfo;
static TypeInfo* typinfo;
static FuncInfo* funinfo;
static PkgInfo* packageinfo;
static OprInfo* oprinfo;
static NamespaceInfo* nspinfo;
static int numTables;
static int numTypes;
static int numPackages;
static int numFuncs;
static int numOperators;
static int numCollations;
static int numNamespaces;
static DumpableObject** tblinfoindex;
static DumpableObject** typinfoindex;
static DumpableObject** packageinfoindex;
static DumpableObject** funinfoindex;
static DumpableObject** oprinfoindex;
static DumpableObject** collinfoindex;
static DumpableObject** nspinfoindex;

static void flagInhTables(TableInfo* tbinfo, int numTables, InhInfo* inhinfo, int numInherits);
static void flagInhAttrs(TableInfo* tblinfo, int numTables);
static DumpableObject** buildIndexArray(void* objArray, int numObjs, Size objSize);
static int DOCatalogIdCompare(const void* p1, const void* p2);
static void findParentsByOid(TableInfo* self, InhInfo* inhinfo, int numInherits);
static int strInArray(const char* pattern, const char** arr, int arr_size);

/*
 * getSchemaData
 *	  Collect information about all potentially dumpable objects
 */
TableInfo* getSchemaData(Archive* fout, int* numTablesPtr)
{
    ExtensionInfo* extinfo = NULL;
    InhInfo* inhinfo = NULL;
    CollInfo* collinfo = NULL;
    int numExtensions;
    int numAggregates;
    int numInherits;
    int numRules;
    int numProcLangs;
    int numCasts;
    int numOpclasses;
    int numOpfamilies;
    int numConversions;
    int numTSParsers;
    int numTSTemplates;
    int numTSDicts;
    int numTSConfigs;
    int numForeignDataWrappers;
    int numForeignServers;
    int numDefaultACLs;
    int numEventTriggers;
    int numEvents;
    int numAccessMethods;
    if (g_verbose)
        write_msg(NULL, "reading schemas\n");
    nspinfo = getNamespaces(fout, &numNamespaces);
    nspinfoindex = buildIndexArray(nspinfo, numNamespaces, sizeof(NamespaceInfo));
    g_curStep++;

    /*
     * getTables should be done as soon as possible, so as to minimize the
     * window between starting our transaction and acquiring per-table locks.
     * However, we have to do getNamespaces first because the tables get
     * linked to their containing namespaces during getTables.
     */
    if (g_verbose)
        write_msg(NULL, "reading user-defined tables\n");
    tblinfo = getTables(fout, &numTables);
    tblinfoindex = buildIndexArray(tblinfo, numTables, sizeof(TableInfo));
    g_curStep++;

    /* Do this after we've built tblinfoindex */
    getOwnedSeqs(fout, tblinfo, numTables);

    if (g_verbose)
        write_msg(NULL, "reading extensions\n");
    extinfo = getExtensions(fout, &numExtensions);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined functions\n");
    funinfo = getFuncs(fout, &numFuncs);
    funinfoindex = buildIndexArray(funinfo, numFuncs, sizeof(FuncInfo));
    g_curStep++;

    /* this must be after getTables and getFuncs */
    if (g_verbose)
        write_msg(NULL, "reading user-defined types\n");
    typinfo = getTypes(fout, &numTypes);
    typinfoindex = buildIndexArray(typinfo, numTypes, sizeof(TypeInfo));
    g_curStep++;

    /* this must be after getFuncs, too */
    if (g_verbose)
        write_msg(NULL, "reading procedural languages\n");
    getProcLangs(fout, &numProcLangs);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined aggregate functions\n");
    getAggregates(fout, &numAggregates);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined operators\n");
    oprinfo = getOperators(fout, &numOperators);
    oprinfoindex = buildIndexArray(oprinfo, numOperators, sizeof(OprInfo));
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined operator classes\n");
    getOpclasses(fout, &numOpclasses);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined operator families\n");
    getOpfamilies(fout, &numOpfamilies);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined text search parsers\n");
    getTSParsers(fout, &numTSParsers);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined text search templates\n");
    getTSTemplates(fout, &numTSTemplates);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined text search dictionaries\n");
    getTSDictionaries(fout, &numTSDicts);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined text search configurations\n");
    getTSConfigurations(fout, &numTSConfigs);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined foreign-data wrappers\n");
    getForeignDataWrappers(fout, &numForeignDataWrappers);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined foreign servers\n");
    getForeignServers(fout, &numForeignServers);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading default privileges\n");
    getDefaultACLs(fout, &numDefaultACLs);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined collations\n");
    collinfo = getCollations(fout, &numCollations);
    collinfoindex = buildIndexArray(collinfo, numCollations, sizeof(CollInfo));
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined conversions\n");
    getConversions(fout, &numConversions);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading type casts\n");
    getCasts(fout, &numCasts);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading table inheritance information\n");
    inhinfo = getInherits(fout, &numInherits);
    g_curStep++;

    /*
     * Identify extension member objects and mark them as not to be dumped.
     * This must happen after reading all objects that can be direct members
     * of extensions, but before we begin to process table subsidiary objects.
     */
    if (g_verbose)
        write_msg(NULL, "finding extension members\n");
    getExtensionMembership(fout, extinfo, numExtensions);
    g_curStep++;

    /* Link tables to parents, mark parents of target tables interesting */
    if (g_verbose)
        write_msg(NULL, "finding inheritance relationships\n");
    flagInhTables(tblinfo, numTables, inhinfo, numInherits);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading column info for interesting tables\n");
    getTableAttrs(fout, tblinfo, numTables);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "flagging inherited columns in subtables\n");
    flagInhAttrs(tblinfo, numTables);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading indexes\n");
    getIndexes(fout, tblinfo, numTables);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading constraints\n");
    getConstraints(fout, tblinfo, numTables);
    g_curStep++;

    /*
     * @hdfs
     * Reading foreign table informational constraint info.
     */
    if (g_verbose)
        write_msg(NULL, "reading constraints about foregin table\n");
    getConstraintsOnForeignTable(fout, tblinfo, numTables);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading triggers\n");
    getTriggers(fout, tblinfo, numTables);
    g_curStep++;

    if (g_verbose) {
        write_msg(NULL, "reading events\n");
    }
    getEvents(fout, &numEvents);
    g_curStep++;

    /*Open-source-fix: Fix ordering of obj id for Rules and EventTriggers*/
    if (g_verbose)
        write_msg(NULL, "reading rewrite rules\n");
    getRules(fout, &numRules);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading row level security policies\n");
    getRlsPolicies(fout, tblinfo, numTables);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined packages\n");
    packageinfo = getPackages(fout, &numPackages);
    if (packageinfo!=NULL) {
        packageinfoindex = buildIndexArray(packageinfo, numPackages, sizeof(PkgInfo));
    }
    g_curStep++;

    if (g_verbose) {
        write_msg(NULL, "reading publications\n");
    }
    getPublications(fout);
    g_curStep++;

    if (g_verbose) {
        write_msg(NULL, "reading publication membership\n");
    }
    getPublicationTables(fout, tblinfo, numTables);
    g_curStep++;

    if (g_verbose) {
        write_msg(NULL, "reading subscriptions\n");
    }
    getSubscriptions(fout);
    g_curStep++;

    if (g_verbose) {
        write_msg(NULL, "reading event triggers\n");
    }
    getEventTriggers(fout, &numEventTriggers);
    g_curStep++;

    if (g_verbose)
        write_msg(NULL, "reading user-defined access methods\n");
    getAccessMethods(fout, &numAccessMethods);
    g_curStep++;

    *numTablesPtr = numTables;
    GS_FREE(inhinfo);
    return tblinfo;
}

/* flagInhTables -
 *	 Fill in parent link fields of every target table, and mark
 *	 parents of target tables as interesting
 *
 * Note that only direct ancestors of targets are marked interesting.
 * This is sufficient; we don't much care whether they inherited their
 * attributes or not.
 *
 * modifies tblinfo
 */
static void flagInhTables(TableInfo* ptblinfo, int inumTables, InhInfo* inhinfo, int numInherits)
{
    int i, j;
    int numParents;
    TableInfo** parents;

    for (i = 0; i < inumTables; i++) {
        /* Sequences, contqueries and views never have parents */
        if (RELKIND_IS_SEQUENCE(ptblinfo[i].relkind) || ptblinfo[i].relkind == RELKIND_VIEW || 
            ptblinfo[i].relkind == RELKIND_CONTQUERY)
            continue;

        /* Don't bother computing anything for non-target tables, either */
        if (!ptblinfo[i].dobj.dump)
            continue;

        /* Find all the immediate parent tables */
        findParentsByOid(&ptblinfo[i], inhinfo, numInherits);

        /* Mark the parents as interesting for getTableAttrs */
        numParents = ptblinfo[i].numParents;
        parents = ptblinfo[i].parents;
        for (j = 0; j < numParents; j++)
            parents[j]->interesting = true;
    }
}

/* flagInhAttrs -
 *	 for each dumpable table in tblinfo, flag its inherited attributes
 *
 * What we need to do here is detect child columns that inherit NOT NULL
 * bits from their parents (so that we needn't specify that again for the
 * child) and child columns that have DEFAULT NULL when their parents had
 * some non-null default.  In the latter case, we make up a dummy AttrDefInfo
 * object so that we'll correctly emit the necessary DEFAULT NULL clause;
 * otherwise the backend will apply an inherited default to the column.
 *
 * modifies tblinfo
 */
static void flagInhAttrs(TableInfo* ptblinfo, int inumTables)
{
    int i, j, k;

    for (i = 0; i < inumTables; i++) {
        TableInfo* tbinfo = &(ptblinfo[i]);
        int numParents;
        TableInfo** parents;

        /* Sequences, contqueries and views never have parents */
        if (RELKIND_IS_SEQUENCE(tbinfo->relkind) || tbinfo->relkind == RELKIND_VIEW || 
            tbinfo->relkind == RELKIND_MATVIEW || tbinfo->relkind == RELKIND_CONTQUERY)
            continue;

        /* Don't bother computing anything for non-target tables, either */
        if (!tbinfo->dobj.dump)
            continue;

        numParents = tbinfo->numParents;
        parents = tbinfo->parents;

        if (numParents == 0)
            continue; /* nothing to see here, move along */

        /* For each column, search for matching column names in parent(s) */
        for (j = 0; j < tbinfo->numatts; j++) {
            bool foundNotNull = false; /* Attr was NOT NULL in a parent */
            bool foundDefault = false; /* Found a default in a parent */

            /* no point in examining dropped columns */
            if (tbinfo->attisdropped[j]) {
                continue;
            }

            foundNotNull = false;
            foundDefault = false;
            for (k = 0; k < numParents; k++) {
                TableInfo* parent = parents[k];
                int inhAttrInd;

                inhAttrInd = strInArray(tbinfo->attnames[j], (const char**)(parent->attnames), parent->numatts);
                if (inhAttrInd >= 0) {
                    foundNotNull = (bool)((uint8)(foundNotNull) | (uint8)(parent->notnull[inhAttrInd]));
                    foundDefault = (bool)((uint8)(foundDefault) | (uint8)(parent->attrdefs[inhAttrInd] != NULL));
                }
            }

            /* Remember if we found inherited NOT NULL */
            tbinfo->inhNotNull[j] = foundNotNull;

            /* Manufacture a DEFAULT NULL clause if necessary */
            if (foundDefault && tbinfo->attrdefs[j] == NULL) {
                AttrDefInfo* attrDef = NULL;

                attrDef = (AttrDefInfo*)pg_malloc(sizeof(AttrDefInfo));
                attrDef->dobj.objType = DO_ATTRDEF;
                attrDef->dobj.catId.tableoid = 0;
                attrDef->dobj.catId.oid = 0;
                AssignDumpId(&attrDef->dobj);
                attrDef->dobj.name = gs_strdup(tbinfo->dobj.name);
                attrDef->dobj.nmspace = tbinfo->dobj.nmspace;
                attrDef->dobj.dump = tbinfo->dobj.dump;

                attrDef->adtable = tbinfo;
                attrDef->adnum = j + 1;
                attrDef->adef_expr = gs_strdup("NULL");

                /* Will column be dumped explicitly? */
                if (shouldPrintColumn(tbinfo, j)) {
                    attrDef->separate = false;
                    /* No dependency needed: NULL cannot have dependencies */
                } else {
                    /* column will be suppressed, print default separately */
                    attrDef->separate = true;
                    /* ensure it comes out after the table */
                    addObjectDependency(&attrDef->dobj, tbinfo->dobj.dumpId);
                }

                tbinfo->attrdefs[j] = attrDef;
            }
        }
    }
}

/*
 * AssignDumpId
 *		Given a newly-created dumpable object, assign a dump ID,
 *		and enter the object into the lookup table.
 *
 * The caller is expected to have filled in objType and catId,
 * but not any of the other standard fields of a DumpableObject.
 */
void AssignDumpId(DumpableObject* dobj)
{
    errno_t rc = 0;
    dobj->dumpId = ++lastDumpId;
    dobj->name = NULL;        /* must be set later */
    dobj->nmspace = NULL;     /* may be set later */
    dobj->dump = true;        /* default assumption */
    dobj->ext_member = false; /* default assumption */
    dobj->dependencies = NULL;
    dobj->nDeps = 0;
    dobj->allocDeps = 0;

    while (dobj->dumpId >= allocedDumpIds) {
        int newAlloc;

        if (allocedDumpIds <= 0) {
            newAlloc = 256;
            dumpIdMap = (DumpableObject**)pg_malloc(newAlloc * sizeof(DumpableObject*));
        } else {
            newAlloc = allocedDumpIds * 2;
            dumpIdMap = (DumpableObject**)pg_realloc(dumpIdMap, newAlloc * sizeof(DumpableObject*));
        }
        rc = memset_s(dumpIdMap + allocedDumpIds,
            (newAlloc - allocedDumpIds) * sizeof(DumpableObject*),
            0,
            (newAlloc - allocedDumpIds) * sizeof(DumpableObject*));
        securec_check_c(rc, "\0", "\0");
        allocedDumpIds = newAlloc;
    }
    dumpIdMap[dobj->dumpId] = dobj;

    /* mark catalogIdMap invalid, but don't rebuild it yet */
    catalogIdMapValid = false;
}

/*
 * Assign a DumpId that's not tied to a DumpableObject.
 *
 * This is used when creating a "fixed" ArchiveEntry that doesn't need to
 * participate in the sorting logic.
 */
DumpId createDumpId(void)
{
    return ++lastDumpId;
}

/*
 * Return the largest DumpId so far assigned
 */
DumpId getMaxDumpId(void)
{
    return lastDumpId;
}

/*
 * Find a DumpableObject by dump ID
 *
 * Returns NULL for invalid ID
 */
DumpableObject* findObjectByDumpId(DumpId dumpId)
{
    if (dumpId <= 0 || dumpId >= allocedDumpIds)
        return NULL; /* out of range? */
    return dumpIdMap[dumpId];
}

/*
 * Find a DumpableObject by catalog ID
 *
 * Returns NULL for unknown ID
 *
 * We use binary search in a sorted list that is built on first call.
 * If AssignDumpId() and findObjectByCatalogId() calls were freely intermixed,
 * the code would work, but possibly be very slow.	In the current usage
 * pattern that does not happen, indeed we build the list at most twice.
 */
DumpableObject* findObjectByCatalogId(CatalogId catalogId)
{
    DumpableObject** low;
    DumpableObject** high;

    if (!catalogIdMapValid) {
        if (catalogIdMap != NULL) {
            free(catalogIdMap);
            catalogIdMap = NULL;
        }
        getDumpableObjects(&catalogIdMap, &numCatalogIds);
        if (numCatalogIds > 1)
            qsort((void*)catalogIdMap, numCatalogIds, sizeof(DumpableObject*), DOCatalogIdCompare);
        catalogIdMapValid = true;
    }

    /*
     * We could use bsearch() here, but the notational cruft of calling
     * bsearch is nearly as bad as doing it ourselves; and the generalized
     * bsearch function is noticeably slower as well.
     */
    if (numCatalogIds <= 0) {
        return NULL;
    }
    low = catalogIdMap;
    high = catalogIdMap + (numCatalogIds - 1);
    while (low <= high) {
        DumpableObject** middle;
        int difference;

        middle = low + (high - low) / 2;
        /* comparison must match DOCatalogIdCompare, below */
        difference = oidcmp((*middle)->catId.oid, catalogId.oid);
        if (difference == 0)
            difference = oidcmp((*middle)->catId.tableoid, catalogId.tableoid);
        if (difference == 0)
            return *middle;
        else if (difference < 0)
            low = middle + 1;
        else
            high = middle - 1;
    }
    return NULL;
}

/*
 * Find a DumpableObject by OID, in a pre-sorted array of one type of object
 *
 * Returns NULL for unknown OID
 */
static DumpableObject* findObjectByOid(Oid oid, DumpableObject** indexArray, int numObjs)
{
    DumpableObject** low;
    DumpableObject** high;

    /*
     * This is the same as findObjectByCatalogId except we assume we need not
     * look at table OID because the objects are all the same type.
     *
     * We could use bsearch() here, but the notational cruft of calling
     * bsearch is nearly as bad as doing it ourselves; and the generalized
     * bsearch function is noticeably slower as well.
     */
    if (numObjs <= 0) {
        return NULL;
    }
    low = indexArray;
    high = indexArray + (numObjs - 1);
    while (low <= high) {
        DumpableObject** middle;
        int difference;

        middle = low + (high - low) / 2;
        difference = oidcmp((*middle)->catId.oid, oid);
        if (difference == 0)
            return *middle;
        else if (difference < 0)
            low = middle + 1;
        else
            high = middle - 1;
    }
    return NULL;
}

/*
 * Build an index array of DumpableObject pointers, sorted by OID
 */
static DumpableObject** buildIndexArray(void* objArray, int numObjs, Size objSize)
{
    DumpableObject** ptrs;
    int i;

    ptrs = (DumpableObject**)pg_malloc(numObjs * sizeof(DumpableObject*));
    for (i = 0; i < numObjs; i++)
        ptrs[i] = (DumpableObject*)((char*)objArray + i * objSize);

    /* We can use DOCatalogIdCompare to sort since its first key is OID */
    if (numObjs > 1)
        qsort((void*)ptrs, numObjs, sizeof(DumpableObject*), DOCatalogIdCompare);

    return ptrs;
}

/*
 * qsort comparator for pointers to DumpableObjects
 */
static int DOCatalogIdCompare(const void* p1, const void* p2)
{
    const DumpableObject* obj1 = *(DumpableObject* const*)p1;
    const DumpableObject* obj2 = *(DumpableObject* const*)p2;
    int cmpval;

    /*
     * Compare OID first since it's usually unique, whereas there will only be
     * a few distinct values of tableoid.
     */
    cmpval = oidcmp(obj1->catId.oid, obj2->catId.oid);
    if (cmpval == 0)
        cmpval = oidcmp(obj1->catId.tableoid, obj2->catId.tableoid);
    return cmpval;
}

/*
 * Build an array of pointers to all known dumpable objects
 *
 * This simply creates a modifiable copy of the internal map.
 */
void getDumpableObjects(DumpableObject*** objs, int* numObjs)
{
    int i, j;

    *objs = (DumpableObject**)pg_malloc(allocedDumpIds * sizeof(DumpableObject*));
    j = 0;
    for (i = 1; i < allocedDumpIds; i++) {
        if (dumpIdMap[i] != NULL)
            (*objs)[j++] = dumpIdMap[i];
    }
    *numObjs = j;
}

/*
 * Add a dependency link to a DumpableObject
 *
 * Note: duplicate dependencies are currently not eliminated
 */
void addObjectDependency(DumpableObject* dobj, DumpId refId)
{
    if (dobj->nDeps >= dobj->allocDeps) {
        if (dobj->allocDeps <= 0) {
            dobj->allocDeps = 16;
            dobj->dependencies = (DumpId*)pg_malloc(dobj->allocDeps * sizeof(DumpId));
        } else {
            dobj->allocDeps *= 2;
            dobj->dependencies = (DumpId*)pg_realloc(dobj->dependencies, dobj->allocDeps * sizeof(DumpId));
        }
    }
    dobj->dependencies[dobj->nDeps++] = refId;
}

/*
 * Remove a dependency link from a DumpableObject
 *
 * If there are multiple links, all are removed
 */
void removeObjectDependency(DumpableObject* dobj, DumpId refId)
{
    int i;
    int j = 0;

    for (i = 0; i < dobj->nDeps; i++) {
        if (dobj->dependencies[i] != refId)
            dobj->dependencies[j++] = dobj->dependencies[i];
    }
    dobj->nDeps = j;
}


bool repairDependencyPkgLoops(DumpableObject** loop, int nLoop)
{
    int i = 0;
    int j = 0;

    for (i = 0; i < nLoop; i++) {
        if (loop[i]->objType == DO_FUNC && ((FuncInfo*)loop[i])->propackageid > 0) {
            for (j = 0; j < loop[i]->nDeps; j++) {
                DumpId dumpId = loop[i]->dependencies[j];
                DumpableObject* funcObj = dumpIdMap[dumpId];
                if (funcObj->catId.oid > FirstNormalObjectId &&
                    (funcObj->objType == DO_TABLE || funcObj->objType == DO_DUMMY_TYPE)) {
                    return false;
                }
            }
        }
    }
    for (i = 0; i < nLoop; i++) {
        if (loop[i]->objType == DO_FUNC && ((FuncInfo*)loop[i])->propackageid > 0) {
            for (j = 0; j < nLoop; j++) {
                if (loop[j]->objType == DO_PACKAGE &&
                    ((FuncInfo*)loop[i])->propackageid == loop[j]->catId.oid &&
                    loop[j+1]->objType == DO_PRE_DATA_BOUNDARY) {
                    removeObjectDependency(loop[j], loop[j+1]->dumpId);
                    return true;
                }
            }
        }
    }

    return false;
}

/*
 * findTableByOid
 *	  finds the entry (in tblinfo) of the table with the given oid
 *	  returns NULL if not found
 */
TableInfo* findTableByOid(Oid oid)
{
    return (TableInfo*)findObjectByOid(oid, tblinfoindex, numTables);
}

/*
 * findTypeByOid
 *	  finds the entry (in typinfo) of the type with the given oid
 *	  returns NULL if not found
 */
TypeInfo* findTypeByOid(Oid oid)
{
    return (TypeInfo*)findObjectByOid(oid, typinfoindex, numTypes);
}

/*
 * findFuncByOid
 *	  finds the entry (in funinfo) of the function with the given oid
 *	  returns NULL if not found
 */
FuncInfo* findFuncByOid(Oid oid)
{
    return (FuncInfo*)findObjectByOid(oid, funinfoindex, numFuncs);
}

/*
 * findOprByOid
 *	  finds the entry (in oprinfo) of the operator with the given oid
 *	  returns NULL if not found
 */
OprInfo* findOprByOid(Oid oid)
{
    return (OprInfo*)findObjectByOid(oid, oprinfoindex, numOperators);
}

/*
 * findCollationByOid
 *	  finds the entry (in collinfo) of the collation with the given oid
 *	  returns NULL if not found
 */
CollInfo* findCollationByOid(Oid oid)
{
    return (CollInfo*)findObjectByOid(oid, collinfoindex, numCollations);
}

/*
 * findNamespaceByOid
 *	  finds the entry (in nspinfo) of the namespace with the given oid
 *	  returns NULL if not found
 */
NamespaceInfo* findNamespaceByOid(Oid oid)
{
    return (NamespaceInfo*)findObjectByOid(oid, nspinfoindex, numNamespaces);
}

/*
 * findParentsByOid
 *	  find a table's parents in tblinfo[]
 */
static void findParentsByOid(TableInfo* self, InhInfo* inhinfo, int numInherits)
{
    Oid oid = self->dobj.catId.oid;
    int i = 0;
    int j = 0;

    int numParents = 0;

    numParents = 0;
    for (i = 0; i < numInherits; i++) {
        if (inhinfo[i].inhrelid == oid)
            numParents++;
    }

    self->numParents = numParents;

    if (numParents > 0) {
        self->parents = (TableInfo**)pg_malloc(sizeof(TableInfo*) * numParents);
        j = 0;
        for (i = 0; i < numInherits; i++) {
            if (inhinfo[i].inhrelid == oid) {
                TableInfo* parent = NULL;

                parent = findTableByOid(inhinfo[i].inhparent);
                if (parent == NULL) {
                    write_msg(NULL,
                        "failed sanity check, parent OID %u of table \"%s\" (OID %u) not found\n",
                        inhinfo[i].inhparent,
                        self->dobj.name,
                        oid);
                    exit_nicely(1);
                }
                self->parents[j++] = parent;
            }
        }
    } else {
        self->parents = NULL;
    }
}

/*
 * parseOidArray
 *	  parse a string of numbers delimited by spaces into a character array
 *
 * Note: actually this is used for both Oids and potentially-signed
 * attribute numbers.  This should cause no trouble, but we could split
 * the function into two functions with different argument types if it does.
 */
void parseOidArray(const char* str, Oid* array, int arraysize)
{
    int j, argNum;
    char temp[100];
    char s;

    argNum = 0;
    j = 0;
    for (;;) {
        s = *str++;
        if (s == ' ' || s == '\0') {
            if (j > 0) {
                if (argNum >= arraysize) {
                    write_msg(NULL, "could not parse numeric array \"%s\": too many numbers\n", str);
                    exit_nicely(1);
                }
                temp[j] = '\0';
                array[argNum++] = atooid(temp);
                j = 0;
            }
            if (s == '\0') {
                break;
            }
        } else {
            if (!(isdigit((unsigned char)s) || s == '-') || (unsigned int)(j) >= sizeof(temp) - 1) {
                write_msg(NULL, "could not parse numeric array \"%s\": invalid character in number\n", str);
                exit_nicely(1);
            }
            temp[j++] = s;
        }
    }

    while (argNum < arraysize)
        array[argNum++] = InvalidOid;
}

/*
 * strInArray:
 *	  takes in a string and a string array and the number of elements in the
 * string array.
 *	  returns the index if the string is somewhere in the array, -1 otherwise
 */
static int strInArray(const char* pattern, const char** arr, int arr_size)
{
    int i;

    for (i = 0; i < arr_size; i++) {
        if (strcmp(pattern, arr[i]) == 0)
            return i;
    }
    return -1;
}
