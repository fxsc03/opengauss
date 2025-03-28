/* -------------------------------------------------------------------------
 *
 * rangetypes.c
 *	  I/O functions, operators, and support functions for range types.
 *
 * The stored (serialized) format of a range value is:
 *
 *	4 bytes: varlena header
 *	4 bytes: range type's OID
 *	Lower boundary value, if any, aligned according to subtype's typalign
 *	Upper boundary value, if any, aligned according to subtype's typalign
 *	1 byte for flags
 *
 * This representation is chosen to avoid needing any padding before the
 * lower boundary value, even when it requires double alignment.  We can
 * expect that the varlena header is presented to us on a suitably aligned
 * boundary (possibly after detoasting), and then the lower boundary is too.
 * Note that this means we can't work with a packed (short varlena header)
 * value; we must detoast it first.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/rangetypes.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "access/hash.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/rangetypes.h"
#include "utils/timestamp.h"

#define RANGE_EMPTY_LITERAL "empty"

/* fn_extra cache entry for one of the range I/O functions */
typedef struct RangeIOData {
    TypeCacheEntry* typcache; /* range type's typcache entry */
    Oid typiofunc;            /* element type's I/O function */
    Oid typioparam;           /* element type's I/O parameter */
    FmgrInfo proc;            /* lookup result for typiofunc */
} RangeIOData;

static RangeIOData* get_range_io_data(FunctionCallInfo fcinfo, Oid rngtypid, IOFuncSelector func);
static char range_parse_flags(const char* flags_str);
static void range_parse(const char* input_str, bool can_ignore, char* flags, char** lbound_str,
                        char** ubound_str, bool* should_reset_base);
static const char* range_parse_bound(const char* string, const char* ptr, char** bound_str, bool* infinite);
static char* range_deparse(char flags, const char* lbound_str, const char* ubound_str);
static char* range_bound_escape(const char* value);
static Size datum_compute_size(Size sz, Datum datum, bool typbyval, char typalign, int16 typlen, char typstorage);
static Pointer datum_write(Pointer ptr, Datum datum, bool typbyval, char typalign, int16 typlen, char typstorage);

void CheckRangeTypeMatch(RangeType* r1, RangeType* r2);

static Datum GetRangeTypeBaseValue(Oid rangeoid, Oid typmod, char* string);

/*
 * ----------------------------------------------------------
 * I/O FUNCTIONS
 * ----------------------------------------------------------
 */

Datum range_in(PG_FUNCTION_ARGS)
{
    char* input_str = PG_GETARG_CSTRING(0);
    Oid rngtypoid = PG_GETARG_OID(1);
    Oid typmod = PG_GETARG_INT32(2);
    RangeType* range = NULL;
    RangeIOData* cache = NULL;
    char flags;
    char* lbound_str = NULL;
    char* ubound_str = NULL;
    RangeBound lower;
    RangeBound upper;
    bool should_reset_base = false;

    cache = get_range_io_data(fcinfo, rngtypoid, IOFunc_input);

    /* parse */
    range_parse(input_str, fcinfo->can_ignore, &flags, &lbound_str, &ubound_str, &should_reset_base);

    if (should_reset_base) {
        return GetRangeTypeBaseValue(rngtypoid, typmod, input_str);
    }

    /* call element type's input function */
    if (RANGE_HAS_LBOUND((unsigned char)flags))
        lower.val = InputFunctionCall(&cache->proc, lbound_str, cache->typioparam, typmod);
    if (RANGE_HAS_UBOUND((unsigned char)flags))
        upper.val = InputFunctionCall(&cache->proc, ubound_str, cache->typioparam, typmod);

    lower.infinite = ((unsigned char)flags & RANGE_LB_INF) != 0;
    lower.inclusive = ((unsigned char)flags & RANGE_LB_INC) != 0;
    lower.lower = true;
    upper.infinite = ((unsigned char)flags & RANGE_UB_INF) != 0;
    upper.inclusive = ((unsigned char)flags & RANGE_UB_INC) != 0;
    upper.lower = false;

    /* serialize and canonicalize */
    range = make_range(cache->typcache, &lower, &upper, flags & RANGE_EMPTY);

    PG_RETURN_RANGE(range);
}

Datum range_out(PG_FUNCTION_ARGS)
{
    RangeType* range = PG_GETARG_RANGE(0);
    char* output_str = NULL;
    RangeIOData* cache = NULL;
    char flags;
    char* lbound_str = NULL;
    char* ubound_str = NULL;
    RangeBound lower;
    RangeBound upper;
    bool empty = false;

    cache = get_range_io_data(fcinfo, RangeTypeGetOid(range), IOFunc_output);

    /* deserialize */
    range_deserialize(cache->typcache, range, &lower, &upper, &empty);
    flags = range_get_flags(range);

    /* call element type's output function */
    if (RANGE_HAS_LBOUND((unsigned char)flags))
        lbound_str = OutputFunctionCall(&cache->proc, lower.val);
    if (RANGE_HAS_UBOUND((unsigned char)flags))
        ubound_str = OutputFunctionCall(&cache->proc, upper.val);

    /* construct result string */
    output_str = range_deparse(flags, lbound_str, ubound_str);

    /* free memory if allocated by the toaster */
    PG_FREE_IF_COPY(range, 0);

    PG_RETURN_CSTRING(output_str);
}

/*
 * Binary representation: The first byte is the flags, then the lower bound
 * (if present), then the upper bound (if present).  Each bound is represented
 * by a 4-byte length header and the binary representation of that bound (as
 * returned by a call to the send function for the subtype).
 */

Datum range_recv(PG_FUNCTION_ARGS)
{
    StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
    Oid rngtypoid = PG_GETARG_OID(1);
    int32 typmod = PG_GETARG_INT32(2);
    RangeType* range = NULL;
    RangeIOData* cache = NULL;
    char flags;
    RangeBound lower;
    RangeBound upper;

    cache = get_range_io_data(fcinfo, rngtypoid, IOFunc_receive);

    /* receive the flags... */
    flags = (unsigned char)pq_getmsgbyte(buf);

    /*
     * Mask out any unsupported flags, particularly RANGE_xB_NULL which would
     * confuse following tests.  Note that range_serialize will take care of
     * cleaning up any inconsistencies in the remaining flags.
     */
    flags &= (RANGE_EMPTY | RANGE_LB_INC | RANGE_LB_INF | RANGE_UB_INC | RANGE_UB_INF);

    /* receive the bounds ... */
    if (RANGE_HAS_LBOUND(flags)) {
        uint32 bound_len = pq_getmsgint(buf, 4);
        const char* bound_data = pq_getmsgbytes(buf, bound_len);
        StringInfoData bound_buf;

        initStringInfo(&bound_buf);
        appendBinaryStringInfo(&bound_buf, bound_data, bound_len);

        lower.val = ReceiveFunctionCall(&cache->proc, &bound_buf, cache->typioparam, typmod);
        pfree_ext(bound_buf.data);
    } else
        lower.val = (Datum)0;

    if (RANGE_HAS_UBOUND(flags)) {
        uint32 bound_len = pq_getmsgint(buf, 4);
        const char* bound_data = pq_getmsgbytes(buf, bound_len);
        StringInfoData bound_buf;

        initStringInfo(&bound_buf);
        appendBinaryStringInfo(&bound_buf, bound_data, bound_len);

        upper.val = ReceiveFunctionCall(&cache->proc, &bound_buf, cache->typioparam, typmod);
        pfree_ext(bound_buf.data);
    } else
        upper.val = (Datum)0;

    pq_getmsgend(buf);

    /* finish constructing RangeBound representation */
    lower.infinite = ((unsigned char)flags & RANGE_LB_INF) != 0;
    lower.inclusive = ((unsigned char)flags & RANGE_LB_INC) != 0;
    lower.lower = true;
    upper.infinite = ((unsigned char)flags & RANGE_UB_INF) != 0;
    upper.inclusive = ((unsigned char)flags & RANGE_UB_INC) != 0;
    upper.lower = false;

    /* serialize and canonicalize */
    range = make_range(cache->typcache, &lower, &upper, flags & RANGE_EMPTY);

    PG_RETURN_RANGE(range);
}

Datum range_send(PG_FUNCTION_ARGS)
{
    RangeType* range = PG_GETARG_RANGE(0);
    StringInfo buf = makeStringInfo();
    RangeIOData* cache = NULL;
    char flags;
    RangeBound lower;
    RangeBound upper;
    bool empty = false;

    cache = get_range_io_data(fcinfo, RangeTypeGetOid(range), IOFunc_send);

    /* deserialize */
    range_deserialize(cache->typcache, range, &lower, &upper, &empty);
    flags = range_get_flags(range);

    /* construct output */
    pq_begintypsend(buf);

    pq_sendbyte(buf, flags);

    if (RANGE_HAS_LBOUND((unsigned char)flags)) {
        Datum bound = PointerGetDatum(SendFunctionCall(&cache->proc, lower.val));
        uint32 bound_len = VARSIZE(bound) - VARHDRSZ;
        char* bound_data = VARDATA(bound);

        pq_sendint32(buf, bound_len);
        pq_sendbytes(buf, bound_data, bound_len);
    }

    if (RANGE_HAS_UBOUND((unsigned char)flags)) {
        Datum bound = PointerGetDatum(SendFunctionCall(&cache->proc, upper.val));
        uint32 bound_len = VARSIZE(bound) - VARHDRSZ;
        char* bound_data = VARDATA(bound);

        pq_sendint32(buf, bound_len);
        pq_sendbytes(buf, bound_data, bound_len);
    }

    PG_RETURN_BYTEA_P(pq_endtypsend(buf));
}

/*
 * get_range_io_data: get cached information needed for range type I/O
 *
 * The range I/O functions need a bit more cached info than other range
 * functions, so they store a RangeIOData struct in fn_extra, not just a
 * pointer to a type cache entry.
 */
static RangeIOData* get_range_io_data(FunctionCallInfo fcinfo, Oid rngtypid, IOFuncSelector func)
{
    RangeIOData* cache = (RangeIOData*)fcinfo->flinfo->fn_extra;

    if (cache == NULL || cache->typcache->type_id != rngtypid) {
        int16 typlen;
        bool typbyval = false;
        char typalign;
        char typdelim;

        cache = (RangeIOData*)MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, sizeof(RangeIOData));
        cache->typcache = lookup_type_cache(rngtypid, TYPECACHE_RANGE_INFO);
        if (cache->typcache->rngelemtype == NULL)
            ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("type %u is not a range type", rngtypid)));

        /* get_type_io_data does more than we need, but is convenient */
        get_type_io_data(cache->typcache->rngelemtype->type_id,
            func,
            &typlen,
            &typbyval,
            &typalign,
            &typdelim,
            &cache->typioparam,
            &cache->typiofunc);

        if (!OidIsValid(cache->typiofunc)) {
            /* this could only happen for receive or send */
            if (func == IOFunc_receive)
                ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_FUNCTION),
                        errmsg("no binary input function available for type %s",
                            format_type_be(cache->typcache->rngelemtype->type_id))));
            else
                ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_FUNCTION),
                        errmsg("no binary output function available for type %s",
                            format_type_be(cache->typcache->rngelemtype->type_id))));
        }
        fmgr_info_cxt(cache->typiofunc, &cache->proc, fcinfo->flinfo->fn_mcxt);

        fcinfo->flinfo->fn_extra = (void*)cache;
    }

    return cache;
}

/*
 * ----------------------------------------------------------
 * GENERIC FUNCTIONS
 * ----------------------------------------------------------
 */

/* Construct standard-form range value from two arguments */
Datum range_constructor2(PG_FUNCTION_ARGS)
{
    Datum arg1 = PG_GETARG_DATUM(0);
    Datum arg2 = PG_GETARG_DATUM(1);
    Oid rngtypid = get_fn_expr_rettype(fcinfo->flinfo);
    RangeType* range = NULL;
    TypeCacheEntry* typcache = NULL;
    RangeBound lower;
    RangeBound upper;

    typcache = range_get_typcache(fcinfo, rngtypid);

    lower.val = PG_ARGISNULL(0) ? (Datum)0 : arg1;
    lower.infinite = PG_ARGISNULL(0);
    lower.inclusive = true;
    lower.lower = true;

    upper.val = PG_ARGISNULL(1) ? (Datum)0 : arg2;
    upper.infinite = PG_ARGISNULL(1);
    upper.inclusive = false;
    upper.lower = false;

    range = make_range(typcache, &lower, &upper, false);

    PG_RETURN_RANGE(range);
}

/* Construct general range value from three arguments */
Datum range_constructor3(PG_FUNCTION_ARGS)
{
    Datum arg1 = PG_GETARG_DATUM(0);
    Datum arg2 = PG_GETARG_DATUM(1);
    Oid rngtypid = get_fn_expr_rettype(fcinfo->flinfo);
    RangeType* range = NULL;
    TypeCacheEntry* typcache = NULL;
    RangeBound lower;
    RangeBound upper;
    char flags;

    typcache = range_get_typcache(fcinfo, rngtypid);

    if (PG_ARGISNULL(2))
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("range constructor flags argument must not be NULL")));

    flags = range_parse_flags(text_to_cstring(PG_GETARG_TEXT_P(2)));

    lower.val = PG_ARGISNULL(0) ? (Datum)0 : arg1;
    lower.infinite = PG_ARGISNULL(0);
    lower.inclusive = ((unsigned char)flags & RANGE_LB_INC) != 0;
    lower.lower = true;

    upper.val = PG_ARGISNULL(1) ? (Datum)0 : arg2;
    upper.infinite = PG_ARGISNULL(1);
    upper.inclusive = ((unsigned char)flags & RANGE_UB_INC) != 0;
    upper.lower = false;

    range = make_range(typcache, &lower, &upper, false);

    PG_RETURN_RANGE(range);
}

/* range -> subtype functions */

/* extract lower bound value */
Datum range_lower(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    TypeCacheEntry* typcache = NULL;
    RangeBound lower;
    RangeBound upper;
    bool empty = false;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    range_deserialize(typcache, r1, &lower, &upper, &empty);

    /* Return NULL if there's no finite lower bound */
    if (empty || lower.infinite)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(lower.val);
}

/* extract upper bound value */
Datum range_upper(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    TypeCacheEntry* typcache = NULL;
    RangeBound lower;
    RangeBound upper;
    bool empty = false;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    range_deserialize(typcache, r1, &lower, &upper, &empty);

    /* Return NULL if there's no finite upper bound */
    if (empty || upper.infinite)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(upper.val);
}

/* range -> bool functions */

/* is range empty? */
Datum range_empty(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    char flags = range_get_flags(r1);

    PG_RETURN_BOOL((unsigned char)flags & RANGE_EMPTY);
}

/* is lower bound inclusive? */
Datum range_lower_inc(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    char flags = range_get_flags(r1);

    PG_RETURN_BOOL((unsigned char)flags & RANGE_LB_INC);
}

/* is upper bound inclusive? */
Datum range_upper_inc(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    char flags = range_get_flags(r1);

    PG_RETURN_BOOL((unsigned char)flags & RANGE_UB_INC);
}

/* is lower bound infinite? */
Datum range_lower_inf(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    char flags = range_get_flags(r1);

    PG_RETURN_BOOL((unsigned char)flags & RANGE_LB_INF);
}

/* is upper bound infinite? */
Datum range_upper_inf(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    char flags = range_get_flags(r1);

    PG_RETURN_BOOL((unsigned char)flags & RANGE_UB_INF);
}

/* range, element -> bool functions */

/* contains? */
Datum range_contains_elem(PG_FUNCTION_ARGS)
{
    RangeType* r = PG_GETARG_RANGE(0);
    Datum val = PG_GETARG_DATUM(1);
    TypeCacheEntry* typcache = NULL;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

    PG_RETURN_BOOL(range_contains_elem_internal(typcache, r, val));
}

/* contained by? */
Datum elem_contained_by_range(PG_FUNCTION_ARGS)
{
    Datum val = PG_GETARG_DATUM(0);
    RangeType* r = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

    PG_RETURN_BOOL(range_contains_elem_internal(typcache, r, val));
}

/* range, range -> bool functions */

/* equality (internal version) */
bool range_eq_internal(TypeCacheEntry* typcache, RangeType* r1, RangeType* r2)
{
    RangeBound lower1, lower2;
    RangeBound upper1, upper2;
    bool empty1 = false;
    bool empty2 = false;

    /* Different types should be prevented by ANYRANGE matching rules */
    CheckRangeTypeMatch(r1, r2);

    range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
    range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

    if (empty1 && empty2)
        return true;
    if (empty1 != empty2)
        return false;

    if (range_cmp_bounds(typcache, &lower1, &lower2) != 0)
        return false;

    if (range_cmp_bounds(typcache, &upper1, &upper2) != 0)
        return false;

    return true;
}

/* equality */
Datum range_eq(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    PG_RETURN_BOOL(range_eq_internal(typcache, r1, r2));
}

/* inequality (internal version) */
bool range_ne_internal(TypeCacheEntry* typcache, RangeType* r1, RangeType* r2)
{
    return (!range_eq_internal(typcache, r1, r2));
}

/* inequality */
Datum range_ne(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    PG_RETURN_BOOL(range_ne_internal(typcache, r1, r2));
}

/* contains? */
Datum range_contains(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    PG_RETURN_BOOL(range_contains_internal(typcache, r1, r2));
}

/* contained by? */
Datum range_contained_by(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    PG_RETURN_BOOL(range_contained_by_internal(typcache, r1, r2));
}

/* strictly left of? (internal version) */
bool range_before_internal(TypeCacheEntry* typcache, RangeType* r1, RangeType* r2)
{
    RangeBound lower1, lower2;
    RangeBound upper1, upper2;
    bool empty1 = false;
    bool empty2 = false;

    /* Different types should be prevented by ANYRANGE matching rules */
    CheckRangeTypeMatch(r1, r2);

    range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
    range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

    /* An empty range is neither before nor after any other range */
    if (empty1 || empty2)
        return false;

    return (range_cmp_bounds(typcache, &upper1, &lower2) < 0);
}

/* strictly left of? */
Datum range_before(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    PG_RETURN_BOOL(range_before_internal(typcache, r1, r2));
}

/* strictly right of? (internal version) */
bool range_after_internal(TypeCacheEntry* typcache, RangeType* r1, RangeType* r2)
{
    RangeBound lower1, lower2;
    RangeBound upper1, upper2;
    bool empty1 = false;
    bool empty2 = false;

    /* Different types should be prevented by ANYRANGE matching rules */
    CheckRangeTypeMatch(r1, r2);

    range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
    range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

    /* An empty range is neither before nor after any other range */
    if (empty1 || empty2)
        return false;

    return (range_cmp_bounds(typcache, &lower1, &upper2) > 0);
}

/* strictly right of? */
Datum range_after(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    PG_RETURN_BOOL(range_after_internal(typcache, r1, r2));
}

/* adjacent to (but not overlapping)? (internal version) */
bool range_adjacent_internal(TypeCacheEntry* typcache, RangeType* r1, RangeType* r2)
{
    RangeBound lower1, lower2;
    RangeBound upper1, upper2;
    bool empty1 = false;
    bool empty2 = false;
    RangeType* r3 = NULL;
    int cmp;

    /* Different types should be prevented by ANYRANGE matching rules */
    CheckRangeTypeMatch(r1, r2);

    range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
    range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

    /* An empty range is not adjacent to any other range */
    if (empty1 || empty2)
        return false;

    /*
     * Given two ranges A..B and C..D, where B < C, the ranges are adjacent if
     * and only if the range B..C is empty, where inclusivity of these two
     * bounds is inverted compared to the original bounds.	For discrete
     * ranges, we have to rely on the canonicalization function to normalize
     * B..C to empty if it contains no elements of the subtype.  (If there is
     * no canonicalization function, it's impossible for such a range to
     * normalize to empty, so we needn't bother to try.)
     *
     * If B == C, the ranges are adjacent only if these bounds have different
     * inclusive flags (i.e., exactly one of the ranges includes the common
     * boundary point).
     *
     * And if B > C then the ranges cannot be adjacent in this order, but we
     * must consider the other order (i.e., check D <= A).
     */
    cmp = range_cmp_bound_values(typcache, &upper1, &lower2);
    if (cmp < 0) {
        /* in a continuous subtype, there are assumed to be points between */
        if (!OidIsValid(typcache->rng_canonical_finfo.fn_oid))
            return (false);
        /* flip the inclusion flags */
        upper1.inclusive = !upper1.inclusive;
        lower2.inclusive = !lower2.inclusive;
        /* change upper/lower labels to avoid Assert failures */
        upper1.lower = true;
        lower2.lower = false;
        r3 = make_range(typcache, &upper1, &lower2, false);
        return RangeIsEmpty(r3);
    }
    if (cmp == 0) {
        return (upper1.inclusive != lower2.inclusive);
    }

    cmp = range_cmp_bound_values(typcache, &upper2, &lower1);
    if (cmp < 0) {
        /* in a continuous subtype, there are assumed to be points between */
        if (!OidIsValid(typcache->rng_canonical_finfo.fn_oid))
            return (false);
        /* flip the inclusion flags */
        upper2.inclusive = !upper2.inclusive;
        lower1.inclusive = !lower1.inclusive;
        /* change upper/lower labels to avoid Assert failures */
        upper2.lower = true;
        lower1.lower = false;
        r3 = make_range(typcache, &upper2, &lower1, false);
        return RangeIsEmpty(r3);
    }
    if (cmp == 0) {
        return (upper2.inclusive != lower1.inclusive);
    }

    return false;
}

/* adjacent to (but not overlapping)? */
Datum range_adjacent(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    PG_RETURN_BOOL(range_adjacent_internal(typcache, r1, r2));
}

/* overlaps? (internal version) */
bool range_overlaps_internal(TypeCacheEntry* typcache, RangeType* r1, RangeType* r2)
{
    RangeBound lower1, lower2;
    RangeBound upper1, upper2;
    bool empty1 = false;
    bool empty2 = false;

    /* Different types should be prevented by ANYRANGE matching rules */
    CheckRangeTypeMatch(r1, r2);

    range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
    range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

    /* An empty range does not overlap any other range */
    if (empty1 || empty2)
        return false;

    if (range_cmp_bounds(typcache, &lower1, &lower2) >= 0 && range_cmp_bounds(typcache, &lower1, &upper2) <= 0)
        return true;

    if (range_cmp_bounds(typcache, &lower2, &lower1) >= 0 && range_cmp_bounds(typcache, &lower2, &upper1) <= 0)
        return true;

    return false;
}

/* overlaps? */
Datum range_overlaps(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    PG_RETURN_BOOL(range_overlaps_internal(typcache, r1, r2));
}

/* does not extend to right of? (internal version) */
bool range_overleft_internal(TypeCacheEntry* typcache, RangeType* r1, RangeType* r2)
{
    RangeBound lower1, lower2;
    RangeBound upper1, upper2;
    bool empty1 = false;
    bool empty2 = false;

    /* Different types should be prevented by ANYRANGE matching rules */
    CheckRangeTypeMatch(r1, r2);

    range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
    range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

    /* An empty range is neither before nor after any other range */
    if (empty1 || empty2)
        return false;

    if (range_cmp_bounds(typcache, &upper1, &upper2) <= 0)
        return true;

    return false;
}

/* does not extend to right of? */
Datum range_overleft(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    PG_RETURN_BOOL(range_overleft_internal(typcache, r1, r2));
}

/* does not extend to left of? (internal version) */
bool range_overright_internal(TypeCacheEntry* typcache, RangeType* r1, RangeType* r2)
{
    RangeBound lower1, lower2;
    RangeBound upper1, upper2;
    bool empty1 = false;
    bool empty2 = false;

    /* Different types should be prevented by ANYRANGE matching rules */
    CheckRangeTypeMatch(r1, r2);

    range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
    range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

    /* An empty range is neither before nor after any other range */
    if (empty1 || empty2)
        PG_RETURN_BOOL(false);

    if (range_cmp_bounds(typcache, &lower1, &lower2) >= 0)
        PG_RETURN_BOOL(true);

    PG_RETURN_BOOL(false);
}

void CheckRangeTypeMatch(RangeType* r1, RangeType* r2)
{
    if (RangeTypeGetOid(r1) != RangeTypeGetOid(r2))
        ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("range types do not match")));
}

/* does not extend to left of? */
Datum range_overright(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    PG_RETURN_BOOL(range_overright_internal(typcache, r1, r2));
}

/* range, range -> range functions */

/* set difference */
Datum range_minus(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;
    RangeBound lower1, lower2;
    RangeBound upper1, upper2;
    bool empty1 = false;
    bool empty2 = false;
    int cmp_l1l2, cmp_l1u2, cmp_u1l2, cmp_u1u2;

    /* Different types should be prevented by ANYRANGE matching rules */
    CheckRangeTypeMatch(r1, r2);

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
    range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

    /* if either is empty, r1 is the correct answer */
    if (empty1 || empty2)
        PG_RETURN_RANGE(r1);

    cmp_l1l2 = range_cmp_bounds(typcache, &lower1, &lower2);
    cmp_l1u2 = range_cmp_bounds(typcache, &lower1, &upper2);
    cmp_u1l2 = range_cmp_bounds(typcache, &upper1, &lower2);
    cmp_u1u2 = range_cmp_bounds(typcache, &upper1, &upper2);

    if (cmp_l1l2 < 0 && cmp_u1u2 > 0)
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("result of range difference would not be contiguous")));

    if (cmp_l1u2 > 0 || cmp_u1l2 < 0)
        PG_RETURN_RANGE(r1);

    if (cmp_l1l2 >= 0 && cmp_u1u2 <= 0)
        PG_RETURN_RANGE(make_empty_range(typcache));

    if (cmp_l1l2 <= 0 && cmp_u1l2 >= 0 && cmp_u1u2 <= 0) {
        lower2.inclusive = !lower2.inclusive;
        lower2.lower = false; /* it will become the upper bound */
        PG_RETURN_RANGE(make_range(typcache, &lower1, &lower2, false));
    }

    if (cmp_l1l2 >= 0 && cmp_u1u2 >= 0 && cmp_l1u2 <= 0) {
        upper2.inclusive = !upper2.inclusive;
        upper2.lower = true; /* it will become the lower bound */
        PG_RETURN_RANGE(make_range(typcache, &upper2, &upper1, false));
    }

    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("unexpected case in range_minus")));
    PG_RETURN_NULL();
}

/* set union */
Datum range_union(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;
    RangeBound lower1, lower2;
    RangeBound upper1, upper2;
    bool empty1 = false;
    bool empty2 = false;
    RangeBound* result_lower = NULL;
    RangeBound* result_upper = NULL;

    /* Different types should be prevented by ANYRANGE matching rules */
    CheckRangeTypeMatch(r1, r2);

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
    range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

    /* if either is empty, the other is the correct answer */
    if (empty1)
        PG_RETURN_RANGE(r2);
    if (empty2)
        PG_RETURN_RANGE(r1);

    if (!DatumGetBool(range_overlaps(fcinfo)) && !DatumGetBool(range_adjacent(fcinfo)))
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("result of range union would not be contiguous")));

    if (range_cmp_bounds(typcache, &lower1, &lower2) < 0)
        result_lower = &lower1;
    else
        result_lower = &lower2;

    if (range_cmp_bounds(typcache, &upper1, &upper2) > 0)
        result_upper = &upper1;
    else
        result_upper = &upper2;

    PG_RETURN_RANGE(make_range(typcache, result_lower, result_upper, false));
}

/* set intersection */
Datum range_intersect(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;
    RangeBound lower1, lower2;
    RangeBound upper1, upper2;
    bool empty1 = false;
    bool empty2 = false;
    RangeBound* result_lower = NULL;
    RangeBound* result_upper = NULL;

    /* Different types should be prevented by ANYRANGE matching rules */
    CheckRangeTypeMatch(r1, r2);

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
    range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

    if (empty1 || empty2 || !DatumGetBool(range_overlaps(fcinfo)))
        PG_RETURN_RANGE(make_empty_range(typcache));

    if (range_cmp_bounds(typcache, &lower1, &lower2) >= 0)
        result_lower = &lower1;
    else
        result_lower = &lower2;

    if (range_cmp_bounds(typcache, &upper1, &upper2) <= 0)
        result_upper = &upper1;
    else
        result_upper = &upper2;

    PG_RETURN_RANGE(make_range(typcache, result_lower, result_upper, false));
}

/* Btree support */

/* btree comparator */
Datum range_cmp(PG_FUNCTION_ARGS)
{
    RangeType* r1 = PG_GETARG_RANGE(0);
    RangeType* r2 = PG_GETARG_RANGE(1);
    TypeCacheEntry* typcache = NULL;
    RangeBound lower1, lower2;
    RangeBound upper1, upper2;
    bool empty1 = false;
    bool empty2 = false;
    int cmp;

    /* Different types should be prevented by ANYRANGE matching rules */
    CheckRangeTypeMatch(r1, r2);

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r1));

    range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
    range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

    /* For b-tree use, empty ranges sort before all else */
    if (empty1 && empty2)
        cmp = 0;
    else if (empty1)
        cmp = -1;
    else if (empty2)
        cmp = 1;
    else {
        cmp = range_cmp_bounds(typcache, &lower1, &lower2);
        if (cmp == 0)
            cmp = range_cmp_bounds(typcache, &upper1, &upper2);
    }

    PG_FREE_IF_COPY(r1, 0);
    PG_FREE_IF_COPY(r2, 1);

    PG_RETURN_INT32(cmp);
}

/* inequality operators using the range_cmp function */
Datum range_lt(PG_FUNCTION_ARGS)
{
    int cmp = range_cmp(fcinfo);

    PG_RETURN_BOOL(cmp < 0);
}

Datum range_le(PG_FUNCTION_ARGS)
{
    int cmp = range_cmp(fcinfo);

    PG_RETURN_BOOL(cmp <= 0);
}

Datum range_ge(PG_FUNCTION_ARGS)
{
    int cmp = range_cmp(fcinfo);

    PG_RETURN_BOOL(cmp >= 0);
}

Datum range_gt(PG_FUNCTION_ARGS)
{
    int cmp = range_cmp(fcinfo);

    PG_RETURN_BOOL(cmp > 0);
}

/* Hash support */

/* hash a range value */
Datum hash_range(PG_FUNCTION_ARGS)
{
    RangeType* r = PG_GETARG_RANGE(0);
    uint32 result;
    TypeCacheEntry* typcache = NULL;
    TypeCacheEntry* scache = NULL;
    RangeBound lower;
    RangeBound upper;
    bool empty = false;
    char flags;
    uint32 lower_hash;
    uint32 upper_hash;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

    /* deserialize */
    range_deserialize(typcache, r, &lower, &upper, &empty);
    flags = range_get_flags(r);

    /*
     * Look up the element type's hash function, if not done already.
     */
    scache = typcache->rngelemtype;
    if (!OidIsValid(scache->hash_proc_finfo.fn_oid)) {
        scache = lookup_type_cache(scache->type_id, TYPECACHE_HASH_PROC_FINFO);
        if (!OidIsValid(scache->hash_proc_finfo.fn_oid))
            ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_FUNCTION),
                    errmsg("could not identify a hash function for type %s", format_type_be(scache->type_id))));
    }

    /*
     * Apply the hash function to each bound.
     */
    if (RANGE_HAS_LBOUND((unsigned char)flags))
        lower_hash = DatumGetUInt32(FunctionCall1Coll(&scache->hash_proc_finfo, typcache->rng_collation, lower.val));
    else
        lower_hash = 0;

    if (RANGE_HAS_UBOUND((unsigned char)flags))
        upper_hash = DatumGetUInt32(FunctionCall1Coll(&scache->hash_proc_finfo, typcache->rng_collation, upper.val));
    else
        upper_hash = 0;

    /* Merge hashes of flags and bounds */
    result = hash_uint32((uint32)flags);
    result ^= lower_hash;
    result = (result << 1) | (result >> 31);
    result ^= upper_hash;

    PG_RETURN_INT32(result);
}

/* ANALYZE support */

/* typanalyze function for range datatypes */
Datum range_typanalyze(PG_FUNCTION_ARGS)
{
    /*
     * For the moment, just punt and don't analyze range columns.  If we get
     * close to release without having a better answer, we could consider
     * letting std_typanalyze do what it can ... but those stats are probably
     * next door to useless for most activity with range columns, so it's not
     * clear it's worth gathering them.
     */
    PG_RETURN_BOOL(false);
}

/*
 * ----------------------------------------------------------
 * CANONICAL FUNCTIONS
 *
 *	 Functions for specific built-in range types.
 * ----------------------------------------------------------
 */

Datum int4range_canonical(PG_FUNCTION_ARGS)
{
    RangeType* r = PG_GETARG_RANGE(0);
    TypeCacheEntry* typcache = NULL;
    RangeBound lower;
    RangeBound upper;
    bool empty = false;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

    range_deserialize(typcache, r, &lower, &upper, &empty);

    if (empty)
        PG_RETURN_RANGE(r);

    if (!lower.infinite && !lower.inclusive) {
        lower.val = DirectFunctionCall2(int4pl, lower.val, Int32GetDatum(1));
        lower.inclusive = true;
    }

    if (!upper.infinite && upper.inclusive) {
        upper.val = DirectFunctionCall2(int4pl, upper.val, Int32GetDatum(1));
        upper.inclusive = false;
    }

    PG_RETURN_RANGE(range_serialize(typcache, &lower, &upper, false));
}

Datum int8range_canonical(PG_FUNCTION_ARGS)
{
    RangeType* r = PG_GETARG_RANGE(0);
    TypeCacheEntry* typcache = NULL;
    RangeBound lower;
    RangeBound upper;
    bool empty = false;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

    range_deserialize(typcache, r, &lower, &upper, &empty);

    if (empty)
        PG_RETURN_RANGE(r);

    if (!lower.infinite && !lower.inclusive) {
        lower.val = DirectFunctionCall2(int8pl, lower.val, Int64GetDatum(1));
        lower.inclusive = true;
    }

    if (!upper.infinite && upper.inclusive) {
        upper.val = DirectFunctionCall2(int8pl, upper.val, Int64GetDatum(1));
        upper.inclusive = false;
    }

    PG_RETURN_RANGE(range_serialize(typcache, &lower, &upper, false));
}

Datum daterange_canonical(PG_FUNCTION_ARGS)
{
    RangeType* r = PG_GETARG_RANGE(0);
    TypeCacheEntry* typcache = NULL;
    RangeBound lower;
    RangeBound upper;
    bool empty = false;

    typcache = range_get_typcache(fcinfo, RangeTypeGetOid(r));

    range_deserialize(typcache, r, &lower, &upper, &empty);

    if (empty)
        PG_RETURN_RANGE(r);

    if (!lower.infinite && !lower.inclusive) {
        lower.val = DirectFunctionCall2(date_pli, lower.val, Int32GetDatum(1));
        lower.inclusive = true;
    }

    if (!upper.infinite && upper.inclusive) {
        upper.val = DirectFunctionCall2(date_pli, upper.val, Int32GetDatum(1));
        upper.inclusive = false;
    }

    PG_RETURN_RANGE(range_serialize(typcache, &lower, &upper, false));
}

/*
 * ----------------------------------------------------------
 * SUBTYPE_DIFF FUNCTIONS
 *
 * Functions for specific built-in range types.
 *
 * Note that subtype_diff does return the difference, not the absolute value
 * of the difference, and it must take care to avoid overflow.
 * (numrange_subdiff is at some risk there ...)
 * ----------------------------------------------------------
 */

Datum int4range_subdiff(PG_FUNCTION_ARGS)
{
    int32 v1 = PG_GETARG_INT32(0);
    int32 v2 = PG_GETARG_INT32(1);

    PG_RETURN_FLOAT8((float8)v1 - (float8)v2);
}

Datum int8range_subdiff(PG_FUNCTION_ARGS)
{
    int64 v1 = PG_GETARG_INT64(0);
    int64 v2 = PG_GETARG_INT64(1);

    PG_RETURN_FLOAT8((float8)v1 - (float8)v2);
}

Datum numrange_subdiff(PG_FUNCTION_ARGS)
{
    Datum v1 = PG_GETARG_DATUM(0);
    Datum v2 = PG_GETARG_DATUM(1);
    Datum numresult;
    float8 floatresult;

    numresult = DirectFunctionCall2(numeric_sub, v1, v2);

    floatresult = DatumGetFloat8(DirectFunctionCall1(numeric_float8, numresult));

    PG_RETURN_FLOAT8(floatresult);
}

Datum daterange_subdiff(PG_FUNCTION_ARGS)
{
    int32 v1 = PG_GETARG_INT32(0);
    int32 v2 = PG_GETARG_INT32(1);

    PG_RETURN_FLOAT8((float8)v1 - (float8)v2);
}

Datum tsrange_subdiff(PG_FUNCTION_ARGS)
{
    Timestamp v1 = PG_GETARG_TIMESTAMP(0);
    Timestamp v2 = PG_GETARG_TIMESTAMP(1);
    float8 result;

#ifdef HAVE_INT64_TIMESTAMP
    result = ((float8)v1 - (float8)v2) / USECS_PER_SEC;
#else
    result = v1 - v2;
#endif

    PG_RETURN_FLOAT8(result);
}

Datum tstzrange_subdiff(PG_FUNCTION_ARGS)
{
    Timestamp v1 = PG_GETARG_TIMESTAMP(0);
    Timestamp v2 = PG_GETARG_TIMESTAMP(1);
    float8 result;

#ifdef HAVE_INT64_TIMESTAMP
    result = ((float8)v1 - (float8)v2) / USECS_PER_SEC;
#else
    result = v1 - v2;
#endif

    PG_RETURN_FLOAT8(result);
}

/*
 * ----------------------------------------------------------
 * SUPPORT FUNCTIONS
 *
 *	 These functions aren't in pg_proc, but are useful for
 *	 defining new generic range functions in C.
 * ----------------------------------------------------------
 */

/*
 * range_get_typcache: get cached information about a range type
 *
 * This is for use by range-related functions that follow the convention
 * of using the fn_extra field as a pointer to the type cache entry for
 * the range type.	Functions that need to cache more information than
 * that must fend for themselves.
 */
TypeCacheEntry* range_get_typcache(FunctionCallInfo fcinfo, Oid rngtypid)
{
    TypeCacheEntry* typcache = (TypeCacheEntry*)fcinfo->flinfo->fn_extra;

    if (typcache == NULL || typcache->type_id != rngtypid) {
        typcache = lookup_type_cache(rngtypid, TYPECACHE_RANGE_INFO);
        if (typcache->rngelemtype == NULL)
            ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("type %u is not a range type", rngtypid)));
        fcinfo->flinfo->fn_extra = (void*)typcache;
    }

    return typcache;
}

/*
 * range_serialize: construct a range value from bounds and empty-flag
 *
 * This does not force canonicalization of the range value.  In most cases,
 * external callers should only be canonicalization functions.	Note that
 * we perform some datatype-independent canonicalization checks anyway.
 */
RangeType* range_serialize(TypeCacheEntry* typcache, RangeBound* lower, RangeBound* upper, bool empty)
{
    RangeType* range = NULL;
    int cmp;
    Size msize;
    Pointer ptr;
    int16 typlen;
    bool typbyval = false;
    char typalign;
    char typstorage;
    char flags = 0;

    /*
     * Verify range is not invalid on its face, and construct flags value,
     * preventing any non-canonical combinations such as infinite+inclusive.
     */
    Assert(lower->lower);
    Assert(!upper->lower);

    if (empty)
        flags |= RANGE_EMPTY;
    else {
        cmp = range_cmp_bound_values(typcache, lower, upper);

        /* error check: if lower bound value is above upper, it's wrong */
        if (cmp > 0)
            ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                    errmsg("range lower bound must be less than or equal to range upper bound")));

        /* if bounds are equal, and not both inclusive, range is empty */
        if (cmp == 0 && !(lower->inclusive && upper->inclusive))
            flags |= RANGE_EMPTY;
        else {
            /* infinite boundaries are never inclusive */
            if (lower->infinite)
                flags |= RANGE_LB_INF;
            else if (lower->inclusive)
                flags |= RANGE_LB_INC;
            if (upper->infinite)
                flags |= RANGE_UB_INF;
            else if (upper->inclusive)
                flags |= RANGE_UB_INC;
        }
    }

    /* Fetch information about range's element type */
    typlen = typcache->rngelemtype->typlen;
    typbyval = typcache->rngelemtype->typbyval;
    typalign = typcache->rngelemtype->typalign;
    typstorage = typcache->rngelemtype->typstorage;

    /* Count space for varlena header and range type's OID */
    msize = sizeof(RangeType);
    Assert(msize == (uint)MAXALIGN(msize));

    /* Count space for bounds */
    if (RANGE_HAS_LBOUND((unsigned char)flags)) {
        /*
         * Make sure item to be inserted is not toasted.  It is essential that
         * we not insert an out-of-line toast value pointer into a range
         * object, for the same reasons that arrays and records can't contain
         * them.  It would work to store a compressed-in-line value, but we
         * prefer to decompress and then let compression be applied to the
         * whole range object if necessary.  But, unlike arrays, we do allow
         * short-header varlena objects to stay as-is.
         */
        if (typlen == -1)
            lower->val = PointerGetDatum(PG_DETOAST_DATUM_PACKED(lower->val));

        msize = datum_compute_size(msize, lower->val, typbyval, typalign, typlen, typstorage);
    }

    if (RANGE_HAS_UBOUND((unsigned char)flags)) {
        /* Make sure item to be inserted is not toasted */
        if (typlen == -1)
            upper->val = PointerGetDatum(PG_DETOAST_DATUM_PACKED(upper->val));

        msize = datum_compute_size(msize, upper->val, typbyval, typalign, typlen, typstorage);
    }

    /* Add space for flag byte */
    msize += sizeof(char);

    /* Note: zero-fill is required here, just as in heap tuples */
    range = (RangeType*)palloc0(msize);
    SET_VARSIZE(range, msize);

    /* Now fill in the datum */
    range->rangetypid = typcache->type_id;

    ptr = (char*)(range + 1);

    if (RANGE_HAS_LBOUND((unsigned char)flags)) {
        Assert(lower->lower);
        ptr = datum_write(ptr, lower->val, typbyval, typalign, typlen, typstorage);
    }

    if (RANGE_HAS_UBOUND((unsigned char)flags)) {
        Assert(!upper->lower);
        ptr = datum_write(ptr, upper->val, typbyval, typalign, typlen, typstorage);
    }

    *((char*)ptr) = flags;

    return range;
}

/*
 * range_deserialize: deconstruct a range value
 *
 * NB: the given range object must be fully detoasted; it cannot have a
 * short varlena header.
 *
 * Note that if the element type is pass-by-reference, the datums in the
 * RangeBound structs will be pointers into the given range object.
 */
void range_deserialize(TypeCacheEntry* typcache, RangeType* range, RangeBound* lower, RangeBound* upper, bool* empty)
{
    char flags;
    int16 typlen;
    bool typbyval = false;
    char typalign;
    Pointer ptr;
    Datum lbound;
    Datum ubound;

    /* assert caller passed the right typcache entry */
    if (RangeTypeGetOid(range)!= typcache->type_id) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("caller passed the error typcache entry")));
    }

    /* fetch the flag byte from datum's last byte */
    flags = *((char*)range + VARSIZE(range) - 1);

    /* fetch information about range's element type */
    typlen = typcache->rngelemtype->typlen;
    typbyval = typcache->rngelemtype->typbyval;
    typalign = typcache->rngelemtype->typalign;

    /* initialize data pointer just after the range OID */
    ptr = (Pointer)(range + 1);

    /* fetch lower bound, if any */
    if (RANGE_HAS_LBOUND((unsigned char)flags)) {
        /* att_align_pointer cannot be necessary here */
        lbound = fetch_att(ptr, typbyval, typlen);
        ptr = (Pointer)att_addlength_pointer(ptr, typlen, ptr);
    } else
        lbound = (Datum)0;

    /* fetch upper bound, if any */
    if (RANGE_HAS_UBOUND((unsigned char)flags)) {
        ptr = (Pointer)att_align_pointer(ptr, typalign, typlen, ptr);
        ubound = fetch_att(ptr, typbyval, typlen);
        /* no need for att_addlength_pointer */
    } else
        ubound = (Datum)0;

    /* emit results */

    *empty = ((unsigned char)flags & RANGE_EMPTY) != 0;

    lower->val = lbound;
    lower->infinite = ((unsigned char)flags & RANGE_LB_INF) != 0;
    lower->inclusive = ((unsigned char)flags & RANGE_LB_INC) != 0;
    lower->lower = true;

    upper->val = ubound;
    upper->infinite = ((unsigned char)flags & RANGE_UB_INF) != 0;
    upper->inclusive = ((unsigned char)flags & RANGE_UB_INC) != 0;
    upper->lower = false;
}

/*
 * range_get_flags: just get the flags from a RangeType value.
 *
 * This is frequently useful in places that only need the flags and not
 * the full results of range_deserialize.
 */
char range_get_flags(RangeType* range)
{
    /* fetch the flag byte from datum's last byte */
    return *((char*)range + VARSIZE(range) - 1);
}

/*
 * range_set_contain_empty: set the RANGE_CONTAIN_EMPTY bit in the value.
 *
 * This is only needed in GiST operations, so we don't include a provision
 * for setting it in range_serialize; rather, this function must be applied
 * afterwards.
 */
void range_set_contain_empty(RangeType* range)
{
    char* flagsp = NULL;

    /* flag byte is datum's last byte */
    flagsp = (char*)range + VARSIZE(range) - 1;
    *flagsp = ((unsigned int)(*flagsp) | RANGE_CONTAIN_EMPTY);
}

/*
 * This both serializes and canonicalizes (if applicable) the range.
 * This should be used by most callers.
 */
RangeType* make_range(TypeCacheEntry* typcache, RangeBound* lower, RangeBound* upper, bool empty)
{
    RangeType* range = NULL;

    range = range_serialize(typcache, lower, upper, empty);

    /* no need to call canonical on empty ranges ... */
    if (OidIsValid(typcache->rng_canonical_finfo.fn_oid) && !RangeIsEmpty(range))
        range = DatumGetRangeType(FunctionCall1(&typcache->rng_canonical_finfo, RangeTypeGetDatum(range)));

    return range;
}

/*
 * Compare two range boundary points, returning <0, 0, or >0 according to
 * whether b1 is less than, equal to, or greater than b2.
 *
 * The boundaries can be any combination of upper and lower; so it's useful
 * for a variety of operators.
 *
 * The simple case is when b1 and b2 are both finite and inclusive, in which
 * case the result is just a comparison of the values held in b1 and b2.
 *
 * If a bound is exclusive, then we need to know whether it's a lower bound,
 * in which case we treat the boundary point as "just greater than" the held
 * value; or an upper bound, in which case we treat the boundary point as
 * "just less than" the held value.
 *
 * If a bound is infinite, it represents minus infinity (less than every other
 * point) if it's a lower bound; or plus infinity (greater than every other
 * point) if it's an upper bound.
 *
 * There is only one case where two boundaries compare equal but are not
 * identical: when both bounds are inclusive and hold the same finite value,
 * but one is an upper bound and the other a lower bound.
 */
int range_cmp_bounds(TypeCacheEntry* typcache, RangeBound* b1, RangeBound* b2)
{
    int32 result;

    /*
     * First, handle cases involving infinity, which don't require invoking
     * the comparison proc.
     */
    if (b1->infinite && b2->infinite) {
        /*
         * Both are infinity, so they are equal unless one is lower and the
         * other not.
         */
        if (b1->lower == b2->lower)
            return 0;
        else
            return b1->lower ? -1 : 1;
    } else if (b1->infinite)
        return b1->lower ? -1 : 1;
    else if (b2->infinite)
        return b2->lower ? 1 : -1;

    /*
     * Both boundaries are finite, so compare the held values.
     */
    result = DatumGetInt32(FunctionCall2Coll(&typcache->rng_cmp_proc_finfo, typcache->rng_collation, b1->val, b2->val));

    /*
     * If the comparison is anything other than equal, we're done. If they
     * compare equal though, we still have to consider whether the boundaries
     * are inclusive or exclusive.
     */
    if (result == 0) {
        if (!b1->inclusive && !b2->inclusive) {
            /* both are exclusive */
            if (b1->lower == b2->lower)
                return 0;
            else
                return b1->lower ? 1 : -1;
        } else if (!b1->inclusive)
            return b1->lower ? 1 : -1;
        else if (!b2->inclusive)
            return b2->lower ? -1 : 1;
        else {
            /*
             * Both are inclusive and the values held are equal, so they are
             * equal regardless of whether they are upper or lower boundaries,
             * or a mix.
             */
            return 0;
        }
    }

    return result;
}

/*
 * Compare two range boundary point values, returning <0, 0, or >0 according
 * to whether b1 is less than, equal to, or greater than b2.
 *
 * This is similar to but simpler than range_cmp_bounds().	We just compare
 * the values held in b1 and b2, ignoring inclusive/exclusive flags.  The
 * lower/upper flags only matter for infinities, where they tell us if the
 * infinity is plus or minus.
 */
int range_cmp_bound_values(TypeCacheEntry* typcache, RangeBound* b1, RangeBound* b2)
{
    /*
     * First, handle cases involving infinity, which don't require invoking
     * the comparison proc.
     */
    if (b1->infinite && b2->infinite) {
        /*
         * Both are infinity, so they are equal unless one is lower and the
         * other not.
         */
        if (b1->lower == b2->lower)
            return 0;
        else
            return b1->lower ? -1 : 1;
    } else if (b1->infinite)
        return b1->lower ? -1 : 1;
    else if (b2->infinite)
        return b2->lower ? 1 : -1;

    /*
     * Both boundaries are finite, so compare the held values.
     */
    return DatumGetInt32(FunctionCall2Coll(&typcache->rng_cmp_proc_finfo, typcache->rng_collation, b1->val, b2->val));
}

/*
 * Build an empty range value of the type indicated by the typcache entry.
 */
RangeType* make_empty_range(TypeCacheEntry* typcache)
{
    RangeBound lower;
    RangeBound upper;

    lower.val = (Datum)0;
    lower.infinite = false;
    lower.inclusive = false;
    lower.lower = true;

    upper.val = (Datum)0;
    upper.infinite = false;
    upper.inclusive = false;
    upper.lower = false;

    return make_range(typcache, &lower, &upper, true);
}

/*
 * ----------------------------------------------------------
 * STATIC FUNCTIONS
 * ----------------------------------------------------------
 */

/*
 * Given a string representing the flags for the range type, return the flags
 * represented as a char.
 */
static char range_parse_flags(const char* flags_str)
{
    char flags = 0;

    if (flags_str[0] == '\0' || flags_str[1] == '\0' || flags_str[2] != '\0')
        ereport(ERROR,
            (errcode(ERRCODE_SYNTAX_ERROR),
                errmsg("invalid range bound flags"),
                errhint("Valid values are \"[]\", \"[)\", \"(]\", and \"()\".")));

    switch (flags_str[0]) {
        case '[':
            flags = (unsigned char)flags | RANGE_LB_INC;
            break;
        case '(':
            break;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg("invalid range bound flags"),
                    errhint("Valid values are \"[]\", \"[)\", \"(]\", and \"()\".")));
    }

    switch (flags_str[1]) {
        case ']':
            flags = (unsigned char)flags | RANGE_UB_INC;
            break;
        case ')':
            break;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_SYNTAX_ERROR),
                    errmsg("invalid range bound flags"),
                    errhint("Valid values are \"[]\", \"[)\", \"(]\", and \"()\".")));
    }

    return flags;
}

/*
 * Parse range input.
 *
 * Input parameters:
 *	string: input string to be parsed
 *	can_ignore: whether ignore errors
 * Output parameters:
 *	*flags: receives flags bitmask
 *	*lbound_str: receives palloc'd lower bound string, or NULL if none
 *	*ubound_str: receives palloc'd upper bound string, or NULL if none
 *	*should_reset_base: whether have to reset to base value by type
 *
 * This is modeled somewhat after record_in in rowtypes.c.
 * The input syntax is:
 *	<range>   := EMPTY
 *			   | <lb-inc> <string>, <string> <ub-inc>
 *	<lb-inc>  := '[' | '('
 *	<ub-inc>  := ']' | ')'
 *
 * Whitespace before or after <range> is ignored.  Whitespace within a <string>
 * is taken literally and becomes part of the input string for that bound.
 *
 * A <string> of length zero is taken as "infinite" (i.e. no bound), unless it
 * is surrounded by double-quotes, in which case it is the literal empty
 * string.
 *
 * Within a <string>, special characters (such as comma, parenthesis, or
 * brackets) can be enclosed in double-quotes or escaped with backslash. Within
 * double-quotes, a double-quote can be escaped with double-quote or backslash.
 */
static void range_parse(const char* string, bool can_ignore, char* flags, char** lbound_str,
                        char** ubound_str, bool* should_reset_base)
{
    const char* ptr = string;
    bool infinite = false;
    int level = can_ignore ? WARNING : ERROR;

    *flags = 0;

    /* consume whitespace */
    while (*ptr != '\0' && isspace((unsigned char)*ptr))
        ptr++;

    /* check for empty range */
    if (pg_strncasecmp(ptr, RANGE_EMPTY_LITERAL, strlen(RANGE_EMPTY_LITERAL)) == 0) {
        *flags = RANGE_EMPTY;
        *lbound_str = NULL;
        *ubound_str = NULL;

        ptr += strlen(RANGE_EMPTY_LITERAL);

        /* the rest should be whitespace */
        while (*ptr != '\0' && isspace((unsigned char)*ptr))
            ptr++;

        /* should have consumed everything */
        if (*ptr != '\0') {
            ereport(level,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("malformed range literal: \"%s\"", string),
                    errdetail("Junk after \"empty\" keyword.")));
            *should_reset_base = true;
            return;
        }

        return;
    }

    if (*ptr == '[') {
        *flags = (unsigned char)(*flags) | RANGE_LB_INC;
        ptr++;
    } else if (*ptr == '(') {
        ptr++;
    } else {
        ereport(level,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                errmsg("malformed range literal: \"%s\"", string),
                errdetail("Missing left parenthesis or bracket.")));
        *should_reset_base = true;
        return;
    }

    ptr = range_parse_bound(string, ptr, lbound_str, &infinite);
    if (infinite)
        *flags = (unsigned char)(*flags) | RANGE_LB_INF;

    if (*ptr == ',') {
        ptr++;
    } else {
        ereport(level,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                errmsg("malformed range literal: \"%s\"", string),
                errdetail("Missing comma after lower bound.")));
        *should_reset_base = true;
        return;
    }

    ptr = range_parse_bound(string, ptr, ubound_str, &infinite);
    if (infinite)
        *flags |= RANGE_UB_INF;

    if (*ptr == ']') {
        *flags = (unsigned int)*flags | RANGE_UB_INC;
        ptr++;
    } else if (*ptr == ')') {
        ptr++;
    } else { /* must be a comma */
        ereport(level,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                errmsg("malformed range literal: \"%s\"", string),
                errdetail("Too many commas.")));
        *should_reset_base = true;
        return;
    }

    /* consume whitespace */
    while (*ptr != '\0' && isspace((unsigned char)*ptr))
        ptr++;

    if (*ptr != '\0') {
        ereport(level,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                errmsg("malformed range literal: \"%s\"", string),
                errdetail("Junk after right parenthesis or bracket.")));
        *should_reset_base = true;
        return;
    }
}

static Datum GetRangeTypeBaseValue(Oid rangeoid, Oid typmod, char* string)
{
    Datum datum;
    switch (rangeoid) {
        case NUMRANGEOID:
        case INT8RANGEOID:
        case INT4RANGEOID: {
            Type targetType = typeidType(rangeoid);
            datum = stringTypeDatum(targetType, "(0,0)", NULL, NULL, typmod, true);
            ReleaseSysCache(targetType);
            break;
        }
        case TSRANGEOID:
        case TSTZRANGEOID: {
            Type targetType = typeidType(rangeoid);
            datum = stringTypeDatum(targetType, "(1970-01-01 00:00:00,1970-01-01 00:00:00)", NULL, NULL, typmod, true);
            ReleaseSysCache(targetType);
            break;
        }
        case DATERANGEOID: {
            Type targetType = typeidType(rangeoid);
            datum = stringTypeDatum(targetType, "(1970-01-01,1970-01-01)", NULL, NULL, typmod, true);
            ReleaseSysCache(targetType);
            break;
        }
        default: {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("malformed range literal: \"%s\"", string),
                    errdetail("the feature \"ignore_error\" not supports \"%u\" type yet.", rangeoid)));
            break;
        }
    }

    return datum;
}

/*
 * Helper for range_parse: parse and de-quote one bound string.
 *
 * We scan until finding comma, right parenthesis, or right bracket.
 *
 * Input parameters:
 *	string: entire input string (used only for error reports)
 *	ptr: where to start parsing bound
 * Output parameters:
 *	*bound_str: receives palloc'd bound string, or NULL if none
 *	*infinite: set true if no bound, else false
 *
 * The return value is the scan ptr, advanced past the bound string.
 */
static const char* range_parse_bound(const char* string, const char* ptr, char** bound_str, bool* infinite)
{
    StringInfoData buf;

    /* Check for null: completely empty input means null */
    if (*ptr == ',' || *ptr == ')' || *ptr == ']') {
        *bound_str = NULL;
        *infinite = true;
    } else {
        /* Extract string for this bound */
        bool inquote = false;

        initStringInfo(&buf);
        while (inquote || !(*ptr == ',' || *ptr == ')' || *ptr == ']')) {
            char ch = *ptr++;

            if (ch == '\0')
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                        errmsg("malformed range literal: \"%s\"", string),
                        errdetail("Unexpected end of input.")));
            if (ch == '\\') {
                if (*ptr == '\0')
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                            errmsg("malformed range literal: \"%s\"", string),
                            errdetail("Unexpected end of input.")));
                appendStringInfoChar(&buf, *ptr++);
            } else if (ch == '\"') {
                if (!inquote)
                    inquote = true;
                else if (*ptr == '\"') {
                    /* doubled quote within quote sequence */
                    appendStringInfoChar(&buf, *ptr++);
                } else
                    inquote = false;
            } else
                appendStringInfoChar(&buf, ch);
        }

        *bound_str = buf.data;
        *infinite = false;
    }

    return ptr;
}

/*
 * Convert a deserialized range value to text form
 *
 * Inputs are the flags byte, and the two bound values already converted to
 * text (but not yet quoted).  If no bound value, pass NULL.
 *
 * Result is a palloc'd string
 */
static char* range_deparse(char flags, const char* lbound_str, const char* ubound_str)
{
    StringInfoData buf;

    if (flags & RANGE_EMPTY)
        return pstrdup(RANGE_EMPTY_LITERAL);

    initStringInfo(&buf);

    appendStringInfoChar(&buf, (flags & RANGE_LB_INC) ? '[' : '(');

    if (RANGE_HAS_LBOUND((unsigned char)flags))
        appendStringInfoString(&buf, range_bound_escape(lbound_str));

    appendStringInfoChar(&buf, ',');

    if (RANGE_HAS_UBOUND((unsigned char)flags))
        appendStringInfoString(&buf, range_bound_escape(ubound_str));

    appendStringInfoChar(&buf, ((unsigned char)flags & RANGE_UB_INC) ? ']' : ')');

    return buf.data;
}

/*
 * Helper for range_deparse: quote a bound value as needed
 *
 * Result is a palloc'd string
 */
static char* range_bound_escape(const char* value)
{
    bool nq = false;
    const char* ptr = NULL;
    StringInfoData buf;

    initStringInfo(&buf);

    /* Detect whether we need double quotes for this value */
    nq = (value[0] == '\0'); /* force quotes for empty string */
    for (ptr = value; *ptr; ptr++) {
        char ch = *ptr;

        if (ch == '"' || ch == '\\' || ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == ',' ||
            isspace((unsigned char)ch)) {
            nq = true;
            break;
        }
    }

    /* And emit the string */
    if (nq)
        appendStringInfoChar(&buf, '"');
    for (ptr = value; *ptr; ptr++) {
        char ch = *ptr;

        if (ch == '"' || ch == '\\')
            appendStringInfoChar(&buf, ch);
        appendStringInfoChar(&buf, ch);
    }
    if (nq)
        appendStringInfoChar(&buf, '"');

    return buf.data;
}

/*
 * Test whether range r1 contains range r2.
 *
 * Caller has already checked that they are the same range type, and looked up
 * the necessary typcache entry.
 */
bool range_contains_internal(TypeCacheEntry* typcache, RangeType* r1, RangeType* r2)
{
    RangeBound lower1;
    RangeBound upper1;
    bool empty1 = false;
    RangeBound lower2;
    RangeBound upper2;
    bool empty2 = false;

    /* Different types should be prevented by ANYRANGE matching rules */
    CheckRangeTypeMatch(r1, r2);

    range_deserialize(typcache, r1, &lower1, &upper1, &empty1);
    range_deserialize(typcache, r2, &lower2, &upper2, &empty2);

    /* If either range is empty, the answer is easy */
    if (empty2)
        return true;
    else if (empty1)
        return false;

    /* Else we must have lower1 <= lower2 and upper1 >= upper2 */
    if (range_cmp_bounds(typcache, &lower1, &lower2) > 0)
        return false;
    if (range_cmp_bounds(typcache, &upper1, &upper2) < 0)
        return false;

    return true;
}

bool range_contained_by_internal(TypeCacheEntry* typcache, RangeType* r1, RangeType* r2)
{
    return range_contains_internal(typcache, r2, r1);
}

/*
 * Test whether range r contains a specific element value.
 */
bool range_contains_elem_internal(TypeCacheEntry* typcache, RangeType* r, Datum val)
{
    RangeBound lower;
    RangeBound upper;
    bool empty = false;
    int32 cmp;

    range_deserialize(typcache, r, &lower, &upper, &empty);

    if (empty)
        return false;

    if (!lower.infinite) {
        cmp = DatumGetInt32(FunctionCall2Coll(&typcache->rng_cmp_proc_finfo, typcache->rng_collation, lower.val, val));
        if (cmp > 0)
            return false;
        if (cmp == 0 && !lower.inclusive)
            return false;
    }

    if (!upper.infinite) {
        cmp = DatumGetInt32(FunctionCall2Coll(&typcache->rng_cmp_proc_finfo, typcache->rng_collation, upper.val, val));
        if (cmp < 0)
            return false;
        if (cmp == 0 && !upper.inclusive)
            return false;
    }

    return true;
}

/*
 * datum_compute_size() and datum_write() are used to insert the bound
 * values into a range object.	They are modeled after heaptuple.c's
 * heap_compute_data_size() and heap_fill_tuple(), but we need not handle
 * null values here.  TYPE_IS_PACKABLE must test the same conditions as
 * heaptuple.c's ATT_IS_PACKABLE macro.
 */

/* Does datatype allow packing into the 1-byte-header varlena format? */
#define TYPE_IS_PACKABLE(typlen, typstorage) ((typlen) == -1 && (typstorage) != 'p')

/*
 * Increment data_length by the space needed by the datum, including any
 * preceding alignment padding.
 */
static Size datum_compute_size(Size data_length, Datum val, bool typbyval, char typalign, int16 typlen, char typstorage)
{
    if (TYPE_IS_PACKABLE(typlen, typstorage) && VARATT_CAN_MAKE_SHORT(DatumGetPointer(val))) {
        /*
         * we're anticipating converting to a short varlena header, so adjust
         * length and don't count any alignment
         */
        data_length += VARATT_CONVERTED_SHORT_SIZE(DatumGetPointer(val));
    } else {
        data_length = att_align_datum(data_length, typalign, typlen, val);
        data_length = att_addlength_datum(data_length, typlen, val);
    }

    return data_length;
}

/*
 * Write the given datum beginning at ptr (after advancing to correct
 * alignment, if needed).  Return the pointer incremented by space used.
 */
static Pointer datum_write(Pointer ptr, Datum datum, bool typbyval, char typalign, int16 typlen, char typstorage)
{
    Size data_length;
    int rc = 0;

    if (typbyval) {
        /* pass-by-value */
        ptr = (char*)att_align_nominal(ptr, typalign);
        store_att_byval(ptr, datum, typlen);
        data_length = typlen;
    } else if (typlen == -1) {
        /* varlena */
        Pointer val = DatumGetPointer(datum);

        if (VARATT_IS_EXTERNAL(val)) {
            /*
             * Throw error, because we must never put a toast pointer inside a
             * range object.  Caller should have detoasted it.
             */
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("cannot store a toast pointer inside a range")));
            data_length = 0; /* keep compiler quiet */
        } else if (VARATT_IS_SHORT(val)) {
            /* no alignment for short varlenas */
            data_length = VARSIZE_SHORT(val);
            rc = memcpy_s(ptr, data_length, val, data_length);
            securec_check(rc, "\0", "\0");
        } else if (TYPE_IS_PACKABLE(typlen, typstorage) && VARATT_CAN_MAKE_SHORT(val)) {
            /* convert to short varlena -- no alignment */
            data_length = VARATT_CONVERTED_SHORT_SIZE(val);
            SET_VARSIZE_SHORT(ptr, data_length);
            rc = memcpy_s(ptr + 1, data_length - 1, VARDATA(val), data_length - 1);
            securec_check(rc, "\0", "\0");
        } else {
            /* full 4-byte header varlena */
            ptr = (char*)att_align_nominal(ptr, typalign);
            data_length = VARSIZE(val);
            rc = memcpy_s(ptr, data_length, val, data_length);
            securec_check(rc, "\0", "\0");
        }
    } else if (typlen == -2) {
        /* cstring ... never needs alignment */
        Assert(typalign == 'c');
        data_length = strlen(DatumGetCString(datum)) + 1;
        rc = memcpy_s(ptr, data_length, DatumGetPointer(datum), data_length);
        securec_check(rc, "\0", "\0");
    } else {
        /* fixed-length pass-by-reference */
        ptr = (char*)att_align_nominal(ptr, typalign);
        Assert(typlen > 0);
        data_length = typlen;
        rc = memcpy_s(ptr, data_length, DatumGetPointer(datum), data_length);
        securec_check(rc, "\0", "\0");
    }

    ptr += data_length;

    return ptr;
}
