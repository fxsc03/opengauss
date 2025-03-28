/* -------------------------------------------------------------------------
 *
 * aggregatecmds.cpp
 *
 *	  Routines for aggregate-manipulation commands
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/optimizer/commands/aggregatecmds.cpp
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/heapam.h"
#include "access/tableam.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "catalog/pg_proc_fn.h"

/*
 *	DefineAggregate
 *
 * "oldstyle" signals the old (pre-8.2) style where the aggregate input type
 * is specified by a BASETYPE element in the parameters.  Otherwise,
 * "args" defines the input type(s).
 */
ObjectAddress DefineAggregate(List* name, List* args, bool oldstyle, List* parameters)
{
    char* aggName = NULL;
    Oid aggNamespace;
    AclResult aclresult;
    List* transfuncName = NIL;
    List* finalfuncName = NIL;
    List* sortoperatorName = NIL;
    TypeName* baseType = NULL;
    TypeName* transType = NULL;
    char* initval = NULL;
#ifdef PGXC
    List* collectfuncName = NIL;
    char* initcollect = NULL;
#endif
    Oid* aggArgTypes = NULL;
    int numArgs;
    Oid transTypeId;
    ListCell* pl = NULL;

    /* attribute for ordered set aggregate */
    char aggKind = AGGKIND_NORMAL;

    /* Convert list of names to a name and namespace */
    aggNamespace = QualifiedNameGetCreationNamespace(name, &aggName);

    /* Check we have creation rights in target namespace */
    aclresult = pg_namespace_aclcheck(aggNamespace, GetUserId(), ACL_CREATE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(aggNamespace));
    if (u_sess->attr.attr_sql.enforce_a_behavior) {
        Oid proowner = InvalidOid;
        /*
         * isalter is true, change the owner of the objects as the owner of the
         * namespace, if the owner of the namespce has the same name as the namescpe
         */
        bool isalter = false;
        proowner = GetUserIdFromNspId(aggNamespace);

        if (!OidIsValid(proowner))
            proowner = GetUserId();
        else if (proowner != GetUserId())
            isalter = true;

        if (isalter) {
            aclresult = pg_namespace_aclcheck(aggNamespace, proowner, ACL_CREATE);
            if (aclresult != ACLCHECK_OK)
                aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(aggNamespace));
        }
    }
    foreach (pl, parameters) {
        DefElem* defel = (DefElem*)lfirst(pl);

        /*
         * sfunc1, stype1, and initcond1 are accepted as obsolete spellings
         * for sfunc, stype, initcond.
         */
        if (pg_strcasecmp(defel->defname, "sfunc") == 0)
            transfuncName = defGetQualifiedName(defel);
        else if (pg_strcasecmp(defel->defname, "sfunc1") == 0)
            transfuncName = defGetQualifiedName(defel);
        else if (pg_strcasecmp(defel->defname, "finalfunc") == 0)
            finalfuncName = defGetQualifiedName(defel);
        else if (pg_strcasecmp(defel->defname, "sortop") == 0)
            sortoperatorName = defGetQualifiedName(defel);
        else if (pg_strcasecmp(defel->defname, "basetype") == 0)
            baseType = defGetTypeName(defel);
        else if (pg_strcasecmp(defel->defname, "stype") == 0)
            transType = defGetTypeName(defel);
        else if (pg_strcasecmp(defel->defname, "stype1") == 0)
            transType = defGetTypeName(defel);
        else if (pg_strcasecmp(defel->defname, "initcond") == 0)
            initval = defGetString(defel);
        else if (pg_strcasecmp(defel->defname, "initcond1") == 0)
            initval = defGetString(defel);
        else if (pg_strcasecmp(defel->defname, "hypothetical") == 0)
            aggKind = AGGKIND_HYPOTHETICAL;
#ifdef PGXC
        else if (pg_strcasecmp(defel->defname, "cfunc") == 0)
            collectfuncName = defGetQualifiedName(defel);
        else if (pg_strcasecmp(defel->defname, "initcollect") == 0)
            initcollect = defGetString(defel);
#endif
        else
            ereport(WARNING,
                (errcode(ERRCODE_SYNTAX_ERROR), errmsg("aggregate attribute \"%s\" not recognized", defel->defname)));
    }

    /*
     * make sure we have our required definitions
     */
    if (transType == NULL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_FUNCTION_DEFINITION), errmsg("aggregate stype must be specified")));
    if (transfuncName == NIL)
        ereport(ERROR, (errcode(ERRCODE_INVALID_FUNCTION_DEFINITION), errmsg("aggregate sfunc must be specified")));

    /*
     * look up the aggregate's input datatype(s).
     */
    if (oldstyle) {
        /*
         * Old style: use basetype parameter.  This supports aggregates of
         * zero or one input, with input type ANY meaning zero inputs.
         *
         * Historically we allowed the command to look like basetype = 'ANY'
         * so we must do a case-insensitive comparison for the name ANY. Ugh.
         */
        if (baseType == NULL)
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_FUNCTION_DEFINITION), errmsg("aggregate input type must be specified")));

        if (pg_strcasecmp(TypeNameToString(baseType), "ANY") == 0) {
            numArgs = 0;
            aggArgTypes = NULL;
        } else {
            numArgs = 1;
            aggArgTypes = (Oid*)palloc(sizeof(Oid));
            aggArgTypes[0] = typenameTypeId(NULL, baseType);
        }
    } else {
        /*
         * New style: args is a list of TypeNames (possibly zero of 'em).
         */
        ListCell* lc = NULL;
        int i = 0;

        if (baseType != NULL)
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
                    errmsg("basetype is redundant with aggregate input type specification")));
        
        /* Given ordered set aggregate with no direct args, aggr_args variable is modified in gram.y.
           So the parse of aggr_args should be changed. See gram.y for detail. */
        numArgs = list_length((List*)linitial(args));
        aggArgTypes = (Oid*)palloc(sizeof(Oid) * numArgs);
        foreach (lc, (List*)linitial(args)) {
            TypeName* curTypeName = (TypeName*)lfirst(lc);

            aggArgTypes[i++] = typenameTypeId(NULL, curTypeName);
        }
        
        /* Set aggKind to AGGKIND_ORDERED_SET if second arg of aggr_args is 0. */
        if (intVal(lsecond(args)) == 0) {
            aggKind = AGGKIND_ORDERED_SET;
        }
    }

    /*
     * look up the aggregate's transtype.
     *
     * transtype can't be a pseudo-type, since we need to be able to store
     * values of the transtype.  However, we can allow polymorphic transtype
     * in some cases (AggregateCreate will check).	Also, we allow "internal"
     * for functions that want to pass pointers to private data structures;
     * but allow that only to superusers, since you could crash the system (or
     * worse) by connecting up incompatible internal-using functions in an
     * aggregate.
     */
    transTypeId = typenameTypeId(NULL, transType);
    if (get_typtype(transTypeId) == TYPTYPE_PSEUDO && !IsPolymorphicType(transTypeId)
        && (transTypeId != INTERNALOID || !superuser())) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
                    errmsg("aggregate transition data type cannot be %s", format_type_be(transTypeId))));
    }
    /*
     * Most of the argument-checking is done inside of AggregateCreate
     */
    return AggregateCreate(aggName, /* aggregate name */
        aggNamespace,        /* namespace */
        aggKind,             /* agg kind */
        aggArgTypes,         /* input data type(s) */
        numArgs,
        transfuncName, /* step function name */
#ifdef PGXC
        collectfuncName, /* collect function name */
#endif
        finalfuncName,    /* final function name */
        sortoperatorName, /* sort operator name */
        transTypeId,      /* transition data type */
#ifdef PGXC
        initval,      /* initial condition */
        initcollect); /* initial condition for collection function */
#else
        initval); /* initial condition */
#endif
}

void RenameAggregate(List* name, List* args, const char* newname)
{
    Oid procOid;
    Oid namespaceOid;
    HeapTuple tup;
    Form_pg_proc procForm;
    Relation rel;
    AclResult aclresult;
    bool isNull = false;
    rel = heap_open(ProcedureRelationId, RowExclusiveLock);

    /* Look up function and make sure it's an aggregate */
    procOid = LookupAggNameTypeNames(name, args, false);

    tup = SearchSysCacheCopy1(PROCOID, ObjectIdGetDatum(procOid));
    if (!HeapTupleIsValid(tup)) /* should not happen */
        ereport(ERROR, (errcode(ERRCODE_CACHE_LOOKUP_FAILED), errmsg("cache lookup failed for function %u", procOid)));
    procForm = (Form_pg_proc)GETSTRUCT(tup);

    namespaceOid = procForm->pronamespace;

    oidvector* proargs = ProcedureGetArgTypes(tup);
    Datum packageoid = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_packageid, &isNull);
    if (isNull) {
        packageoid = DatumGetObjectId(InvalidOid);
    }

#ifndef ENABLE_MULTIPLE_NODES
    Datum allargtypes = ProcedureGetAllArgTypes(tup, &isNull);
    Datum argmodes = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_proargmodes, &isNull);
    /* make sure the new name doesn't exist */
    if (SearchSysCacheForProcAllArgs(
            CStringGetDatum(newname),
            allargtypes,
            ObjectIdGetDatum(namespaceOid),
            ObjectIdGetDatum(packageoid),
            argmodes))
        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_FUNCTION),
                errmsg("function %s already exists in schema \"%s\"",
                    funcname_signature_string(newname, procForm->pronargs, NIL, proargs->values),
                    get_namespace_name(namespaceOid))));
#else
    if (SearchSysCacheExists3(PROCNAMEARGSNSP,
            CStringGetDatum(newname),
            PointerGetDatum(&procForm->proargtypes),
            ObjectIdGetDatum(namespaceOid)))
        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_FUNCTION),
                errmsg("function %s already exists in schema \"%s\"",
                    funcname_signature_string(newname, procForm->pronargs, NIL, proargs->values),
                    get_namespace_name(namespaceOid))));
#endif
    /* must be owner */
    if (!pg_proc_ownercheck(procOid, GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_PROC, NameListToString(name));

    /* must have CREATE privilege on namespace */
    aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
    if (aclresult != ACLCHECK_OK)
        aclcheck_error(aclresult, ACL_KIND_NAMESPACE, get_namespace_name(namespaceOid));

    /* rename */
    (void)namestrcpy(&(((Form_pg_proc)GETSTRUCT(tup))->proname), newname);
    simple_heap_update(rel, &tup->t_self, tup);
    CatalogUpdateIndexes(rel, tup);

    heap_close(rel, NoLock);
    tableam_tops_free_tuple(tup);
}

/*
 * Change aggregate owner
 */
ObjectAddress AlterAggregateOwner(List* name, List* args, Oid newOwnerId)
{
    Oid procOid;
    ObjectAddress address;

    /* Look up function and make sure it's an aggregate */
    procOid = LookupAggNameTypeNames(name, args, false);

    /* The rest is just like a function */
    AlterFunctionOwner_oid(procOid, newOwnerId);
    ObjectAddressSet(address, ProcedureRelationId, procOid);
    return address;
}
