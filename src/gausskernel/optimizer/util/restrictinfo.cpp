/* -------------------------------------------------------------------------
 *
 * restrictinfo.cpp
 *	  RestrictInfo node manipulation routines.
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/optimizer/util/restrictinfo.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "optimizer/clauses.h"
#include "optimizer/predtest.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"

static RestrictInfo* make_restrictinfo_internal(Expr* clause, Expr* orclause, bool is_pushed_down,
    bool outerjoin_delayed, bool pseudoconstant, Index security_level, Relids required_relids, Relids outer_relids,
    Relids nullable_relids, bool is_asof);
static Expr* make_sub_restrictinfos(Expr* clause, bool is_pushed_down, bool outerjoin_delayed, bool pseudoconstant,
    Index sucurity_level, Relids required_relids, Relids outer_relids, Relids nullable_relids, bool is_asof);

/*
 *
 * Build a RestrictInfo node containing the given subexpression.
 *
 * The is_pushed_down, outerjoin_delayed, and pseudoconstant flags for the
 * RestrictInfo must be supplied by the caller, as well as the correct values
 * for security_level, outer_relids, and nullable_relids.
 * required_relids can be NULL, in which case it defaults to the actual
 * clause contents (i.e., clause_relids).
 *
 * We initialize fields that depend only on the given subexpression, leaving
 * others that depend on context (or may never be needed at all) to be filled
 * later.
 */
RestrictInfo* make_restrictinfo(Expr* clause, bool is_pushed_down, bool outerjoin_delayed, bool pseudoconstant,
    Index security_level, Relids required_relids, Relids outer_relids, Relids nullable_relids, bool is_asof)
{
    /*
     * If it's an OR clause, build a modified copy with RestrictInfos inserted
     * above each subclause of the top-level AND/OR structure.
     */
    if (or_clause((Node*)clause))
        return (RestrictInfo*)make_sub_restrictinfos(clause,
            is_pushed_down,
            outerjoin_delayed,
            pseudoconstant,
            security_level,
            required_relids,
            outer_relids,
            nullable_relids,
            is_asof);

    /* Shouldn't be an AND clause, else AND/OR flattening messed up */
    AssertEreport(!and_clause((Node*)clause), MOD_OPT, "");

    return make_restrictinfo_internal(clause,
        NULL,
        is_pushed_down,
        outerjoin_delayed,
        pseudoconstant,
        security_level,
        required_relids,
        outer_relids,
        nullable_relids,
        is_asof);
}

/*
 * make_restrictinfo_from_bitmapqual
 *
 * Given the bitmapqual Path structure for a bitmap indexscan, generate
 * RestrictInfo node(s) equivalent to the condition represented by the
 * indexclauses of the Path structure.
 *
 * The result is a List (effectively, implicit-AND representation) of
 * RestrictInfos.
 *
 * The caller must pass is_pushed_down, but we assume outerjoin_delayed
 * and pseudoconstant are false while outer_relids and nullable_relids
 * are NULL (no other kind of qual should ever get into a bitmapqual).
 *
 * If include_predicates is true, we add any partial index predicates to
 * the explicit index quals.  When this is not true, we return a condition
 * that might be weaker than the actual scan represents.
 *
 * To do this through the normal make_restrictinfo() API, callers would have
 * to strip off the RestrictInfo nodes present in the indexclauses lists, and
 * then make_restrictinfo() would have to build new ones.  It's better to have
 * a specialized routine to allow sharing of RestrictInfos.
 *
 * The qual manipulations here are much the same as in create_bitmap_subplan;
 * keep the two routines in sync!
 */
List* make_restrictinfo_from_bitmapqual(Path* bitmapqual, bool is_pushed_down, bool include_predicates)
{
    List* result = NIL;
    ListCell* l = NULL;

    if (IsA(bitmapqual, BitmapAndPath)) {
        BitmapAndPath* apath = (BitmapAndPath*)bitmapqual;

        /*
         * There may well be redundant quals among the subplans, since a
         * top-level WHERE qual might have gotten used to form several
         * different index quals.  We don't try exceedingly hard to eliminate
         * redundancies, but we do eliminate obvious duplicates by using
         * list_concat_unique.
         */
        result = NIL;
        foreach (l, apath->bitmapquals) {
            List* sublist = NIL;

            sublist = make_restrictinfo_from_bitmapqual((Path*)lfirst(l), is_pushed_down, include_predicates);
            result = list_concat_unique(result, sublist);
        }
    } else if (IsA(bitmapqual, BitmapOrPath)) {
        BitmapOrPath* opath = (BitmapOrPath*)bitmapqual;
        List* withris = NIL;
        List* withoutris = NIL;

        /*
         * Here, we only detect qual-free subplans.  A qual-free subplan would
         * cause us to generate "... OR true ..."  which we may as well reduce
         * to just "true".	We do not try to eliminate redundant subclauses
         * because (a) it's not as likely as in the AND case, and (b) we might
         * well be working with hundreds or even thousands of OR conditions,
         * perhaps from a long IN list.  The performance of list_append_unique
         * would be unacceptable.
         */
        foreach (l, opath->bitmapquals) {
            List* sublist = NIL;

            sublist = make_restrictinfo_from_bitmapqual((Path*)lfirst(l), is_pushed_down, include_predicates);
            if (sublist == NIL) {
                /*
                 * If we find a qual-less subscan, it represents a constant
                 * TRUE, and hence the OR result is also constant TRUE, so we
                 * can stop here.
                 */
                return NIL;
            }

            /*
             * If the sublist contains multiple RestrictInfos, we create an
             * AND subclause.  If there's just one, we have to check if it's
             * an OR clause, and if so flatten it to preserve AND/OR flatness
             * of our output.
             *
             * We construct lists with and without sub-RestrictInfos, so as
             * not to have to regenerate duplicate RestrictInfos below.
             */
            if (list_length(sublist) > 1) {
                withris = lappend(withris, make_andclause(sublist));
                sublist = get_actual_clauses(sublist);
                withoutris = lappend(withoutris, make_andclause(sublist));
            } else {
                RestrictInfo* subri = (RestrictInfo*)linitial(sublist);

                AssertEreport(IsA(subri, RestrictInfo), MOD_OPT, "");
                if (restriction_is_or_clause(subri)) {
                    BoolExpr* subor = (BoolExpr*)subri->orclause;

                    AssertEreport(or_clause((Node*)subor), MOD_OPT, "");
                    withris = list_concat(withris, list_copy(subor->args));
                    subor = (BoolExpr*)subri->clause;
                    AssertEreport(or_clause((Node*)subor), MOD_OPT, "");
                    withoutris = list_concat(withoutris, list_copy(subor->args));
                } else {
                    withris = lappend(withris, subri);
                    withoutris = lappend(withoutris, subri->clause);
                }
            }
        }

        /*
         * Avoid generating one-element ORs, which could happen due to
         * redundancy elimination or ScalarArrayOpExpr quals.
         */
        if (list_length(withris) <= 1)
            result = withris;
        else {
            /* Here's the magic part not available to outside callers */
            result = list_make1(make_restrictinfo_internal(
                make_orclause(withoutris), make_orclause(withris), is_pushed_down, false, false, 0, NULL, NULL, NULL, false));
        }
    } else if (IsA(bitmapqual, IndexPath)) {
        IndexPath* ipath = (IndexPath*)bitmapqual;

        result = list_copy(ipath->indexclauses);
        if (include_predicates && ipath->indexinfo->indpred != NIL) {
            foreach (l, ipath->indexinfo->indpred) {
                Expr* pred = (Expr*)lfirst(l);

                /*
                 * We know that the index predicate must have been implied by
                 * the query condition as a whole, but it may or may not be
                 * implied by the conditions that got pushed into the
                 * bitmapqual.	Avoid generating redundant conditions.
                 */
                if (!predicate_implied_by(list_make1(pred), result))
                    result =
                        lappend(result, make_restrictinfo(pred, is_pushed_down, false, false, 0, NULL, NULL, NULL, false));
            }
        }
    } else {
        ereport(ERROR,
            (errmodule(MOD_OPT),
                errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                errmsg("unrecognized node type: %d", nodeTag(bitmapqual))));
        result = NIL; /* keep compiler quiet */
    }

    return result;
}

/*
 * make_restrictinfos_from_actual_clauses
 *
 * Given a list of implicitly-ANDed restriction clauses, produce a list
 * of RestrictInfo nodes.  This is used to reconstitute the RestrictInfo
 * representation after doing transformations of a list of clauses.
 *
 * We assume that the clauses are relation-level restrictions and therefore
 * we don't have to worry about is_pushed_down, outerjoin_delayed,
 * outer_relids, and nullable_relids (these can be assumed true, false,
 * NULL, and NULL, respectively).
 * We do take care to recognize pseudoconstant clauses properly.
 */
List* make_restrictinfos_from_actual_clauses(PlannerInfo* root, List* clause_list)
{
    List* result = NIL;
    ListCell* l = NULL;

    foreach (l, clause_list) {
        Expr* clause = (Expr*)lfirst(l);
        bool pseudoconstant = false;
        RestrictInfo* rinfo = NULL;

        /*
         * It's pseudoconstant if it contains no Vars and no volatile
         * functions.  We probably can't see any sublinks here, so
         * contain_var_clause() would likely be enough, but for safety use
         * contain_vars_of_level() instead.
         */
        pseudoconstant = !contain_vars_of_level((Node*)clause, 0) && !contain_volatile_functions((Node*)clause);
        if (pseudoconstant) {
            /* tell createplan.c to check for gating quals */
            root->hasPseudoConstantQuals = true;
        }

        rinfo = make_restrictinfo(clause, true, false, pseudoconstant, 0, NULL, NULL, NULL, false);
        result = lappend(result, rinfo);
    }
    return result;
}

/*
 * make_restrictinfo_internal
 *
 * Common code for the main entry points and the recursive cases.
 */
static RestrictInfo* make_restrictinfo_internal(Expr* clause, Expr* orclause, bool is_pushed_down,
    bool outerjoin_delayed, bool pseudoconstant, Index security_level, Relids required_relids, Relids outer_relids,
    Relids nullable_relids, bool is_asof)
{
    RestrictInfo* restrictinfo = makeNode(RestrictInfo);
    errno_t rc = EOK; /* Initialize rc to keep compiler slient */

    restrictinfo->clause = clause;
    restrictinfo->orclause = orclause;
    restrictinfo->is_pushed_down = is_pushed_down;
    restrictinfo->outerjoin_delayed = outerjoin_delayed;
    restrictinfo->pseudoconstant = pseudoconstant;
    restrictinfo->can_join = false; /* may get set below */
    restrictinfo->security_level = security_level;
    restrictinfo->outer_relids = outer_relids;
    restrictinfo->nullable_relids = nullable_relids;
    restrictinfo->is_asof = is_asof;

    /*
     * If it's potentially delayable by lower-level security quals, figure out
     * whether it's leakproof.  We can skip testing this for level-zero quals,
     * since they would never get delayed on security grounds anyway.
     */
    if (security_level > 0)
        restrictinfo->leakproof = (contain_leaky_functions((Node*)clause) == false);
    else
        restrictinfo->leakproof = false; /* really, "don't know" */

    /*
     * If it's a binary opclause, set up left/right relids info. In any case
     * set up the total clause relids info.
     */
    if (is_opclause(clause) && list_length(((OpExpr*)clause)->args) == 2) {
        restrictinfo->left_relids = pull_varnos(get_leftop(clause));
        restrictinfo->right_relids = pull_varnos(get_rightop(clause));

        restrictinfo->clause_relids = bms_union(restrictinfo->left_relids, restrictinfo->right_relids);

        /*
         * Does it look like a normal join clause, i.e., a binary operator
         * relating expressions that come from distinct relations? If so we
         * might be able to use it in a join algorithm.  Note that this is a
         * purely syntactic test that is made regardless of context.
         */
        if (!bms_is_empty(restrictinfo->left_relids) && !bms_is_empty(restrictinfo->right_relids) &&
            !bms_overlap(restrictinfo->left_relids, restrictinfo->right_relids)) {
            restrictinfo->can_join = true;
            /* pseudoconstant should certainly not be true */
            AssertEreport(!restrictinfo->pseudoconstant, MOD_OPT, "");
        }
    } else {
        /* Not a binary opclause, so mark left/right relid sets as empty */
        restrictinfo->left_relids = NULL;
        restrictinfo->right_relids = NULL;
        /* and get the total relid set the hard way */
        restrictinfo->clause_relids = pull_varnos((Node*)clause);
    }

    /* required_relids defaults to clause_relids */
    if (required_relids != NULL)
        restrictinfo->required_relids = required_relids;
    else
        restrictinfo->required_relids = restrictinfo->clause_relids;

    /*
     * Fill in all the cacheable fields with "not yet set" markers. None of
     * these will be computed until/unless needed.	Note in particular that we
     * don't mark a binary opclause as mergejoinable or hashjoinable here;
     * that happens only if it appears in the right context (top level of a
     * joinclause list).
     */
    restrictinfo->parent_ec = NULL;

    restrictinfo->eval_cost.startup = -1;
    restrictinfo->norm_selec = -1;
    restrictinfo->outer_selec = -1;

    restrictinfo->mergeopfamilies = NIL;

    restrictinfo->left_ec = NULL;
    restrictinfo->right_ec = NULL;
    restrictinfo->left_em = NULL;
    restrictinfo->right_em = NULL;
    restrictinfo->scansel_cache = NIL;

    restrictinfo->outer_is_left = false;

    restrictinfo->hashjoinoperator = InvalidOid;

    rc = memset_s(&restrictinfo->left_bucketsize, sizeof(BucketSize), 0, sizeof(BucketSize));
    securec_check(rc, "\0", "\0");
    rc = memset_s(&restrictinfo->right_bucketsize, sizeof(BucketSize), 0, sizeof(BucketSize));
    securec_check(rc, "\0", "\0");

    return restrictinfo;
}

/*
 * Recursively insert sub-RestrictInfo nodes into a boolean expression.
 *
 * We put RestrictInfos above simple (non-AND/OR) clauses and above
 * sub-OR clauses, but not above sub-AND clauses, because there's no need.
 * This may seem odd but it is closely related to the fact that we use
 * implicit-AND lists at top level of RestrictInfo lists.  Only ORs and
 * simple clauses are valid RestrictInfos.
 *
 * The same is_pushed_down, outerjoin_delayed, and pseudoconstant flag
 * values can be applied to all RestrictInfo nodes in the result.  Likewise
 * for security_level, outer_relids and nullable_relids.
 *
 * The given required_relids are attached to our top-level output,
 * but any OR-clause constituents are allowed to default to just the
 * contained rels.
 */
static Expr* make_sub_restrictinfos(Expr* clause, bool is_pushed_down, bool outerjoin_delayed, bool pseudoconstant,
    Index security_level, Relids required_relids, Relids outer_relids, Relids nullable_relids, bool is_asof)
{
    if (or_clause((Node*)clause)) {
        List* orlist = NIL;
        ListCell* temp = NULL;

        foreach (temp, ((BoolExpr*)clause)->args)
            orlist = lappend(orlist,
                make_sub_restrictinfos((Expr*)lfirst(temp),
                    is_pushed_down,
                    outerjoin_delayed,
                    pseudoconstant,
                    security_level,
                    NULL,
                    outer_relids,
                    nullable_relids,
                    is_asof));
        return (Expr*)make_restrictinfo_internal(clause,
            make_orclause(orlist),
            is_pushed_down,
            outerjoin_delayed,
            pseudoconstant,
            security_level,
            required_relids,
            outer_relids,
            nullable_relids,
            is_asof);
    } else if (and_clause((Node*)clause)) {
        List* andlist = NIL;
        ListCell* temp = NULL;

        foreach (temp, ((BoolExpr*)clause)->args)
            andlist = lappend(andlist,
                make_sub_restrictinfos((Expr*)lfirst(temp),
                    is_pushed_down,
                    outerjoin_delayed,
                    pseudoconstant,
                    security_level,
                    required_relids,
                    outer_relids,
                    nullable_relids,
                    is_asof));
        return make_andclause(andlist);
    } else
        return (Expr*)make_restrictinfo_internal(clause,
            NULL,
            is_pushed_down,
            outerjoin_delayed,
            pseudoconstant,
            security_level,
            required_relids,
            outer_relids,
            nullable_relids,
            is_asof);
}

/*
 * restriction_is_or_clause
 *
 * Returns t iff the restrictinfo node contains an 'or' clause.
 */
bool restriction_is_or_clause(RestrictInfo* restrictinfo)
{
    if (restrictinfo->orclause != NULL)
        return true;
    else
        return false;
}

/*
 * restriction_is_securely_promotable
 *
 * Returns true if it's okay to evaluate this clause "early", that is before
 * other restriction clauses attached to the specified relation.
 */
bool restriction_is_securely_promotable(RestrictInfo* restrictinfo, RelOptInfo* rel)
{
    /*
     * It's okay if there are no baserestrictinfo clauses for the rel that
     * would need to go before this one, *or* if this one is leakproof.
     */
    if ((restrictinfo->security_level <= rel->baserestrict_min_security) || restrictinfo->leakproof)
        return true;
    else
        return false;
}

/*
 * get_actual_clauses
 *
 * Returns a list containing the bare clauses from 'restrictinfo_list'.
 *
 * This is only to be used in cases where none of the RestrictInfos can
 * be pseudoconstant clauses (for instance, it's OK on indexqual lists).
 */
List* get_actual_clauses(List* restrictinfo_list)
{
    List* result = NIL;
    ListCell* l = NULL;

    foreach (l, restrictinfo_list) {
        RestrictInfo* rinfo = (RestrictInfo*)lfirst(l);

        AssertEreport(IsA(rinfo, RestrictInfo), MOD_OPT, "");

        AssertEreport(!rinfo->pseudoconstant, MOD_OPT, "");

        result = lappend(result, rinfo->clause);
    }
    return result;
}

/*
 * get_all_actual_clauses
 *
 * Returns a list containing the bare clauses from 'restrictinfo_list'.
 *
 * This loses the distinction between regular and pseudoconstant clauses,
 * so be careful what you use it for.
 */
List* get_all_actual_clauses(List* restrictinfo_list)
{
    List* result = NIL;
    ListCell* l = NULL;

    foreach (l, restrictinfo_list) {
        RestrictInfo* rinfo = (RestrictInfo*)lfirst(l);

        AssertEreport(IsA(rinfo, RestrictInfo), MOD_OPT, "");

        result = lappend(result, rinfo->clause);
    }
    return result;
}

/*
 * extract_actual_clauses
 *
 * Extract bare clauses from 'restrictinfo_list', returning either the
 * regular ones or the pseudoconstant ones per 'pseudoconstant'.
 */
List* extract_actual_clauses(List* restrictinfo_list, bool pseudoconstant)
{
    List* result = NIL;
    ListCell* l = NULL;

    foreach (l, restrictinfo_list) {
        RestrictInfo* rinfo = (RestrictInfo*)lfirst(l);

        AssertEreport(IsA(rinfo, RestrictInfo), MOD_OPT, "");

        /* we consider the qual is real if  pseudoconstant is true and clause_relids is non-null. */
        if ((rinfo->pseudoconstant == pseudoconstant) && (!pseudoconstant || bms_is_empty(rinfo->clause_relids))) {
            result = lappend(result, rinfo->clause);
        }
    }
    return result;
}

/*
 * extract_actual_join_clauses
 *
 * Extract bare clauses from 'restrictinfo_list', separating those that
 * syntactically match the join level from those that were pushed down.
 * Pseudoconstant clauses are excluded from the results.
 *
 * This is only used at outer joins, since for plain joins we don't care
 * about pushed-down-ness.
 */
void extract_actual_join_clauses(List* restrictinfo_list, List** joinquals, List** otherquals)
{
    ListCell* l = NULL;

    *joinquals = NIL;
    *otherquals = NIL;

    foreach (l, restrictinfo_list) {
        RestrictInfo* rinfo = (RestrictInfo*)lfirst(l);

        AssertEreport(IsA(rinfo, RestrictInfo), MOD_OPT, "");

        if (rinfo->is_pushed_down) {
            if (!rinfo->pseudoconstant)
                *otherquals = lappend(*otherquals, rinfo->clause);
        } else {
            /* joinquals shouldn't have been marked pseudoconstant */
            AssertEreport(!rinfo->pseudoconstant, MOD_OPT, "");
            *joinquals = lappend(*joinquals, rinfo->clause);
        }
    }
}

/*
 * join_clause_is_movable_to
 *		Test whether a join clause is a safe candidate for parameterization
 *		of a scan on the specified base relation.
 *
 * A movable join clause is one that can safely be evaluated at a rel below
 * its normal semantic level (ie, its required_relids), if the values of
 * variables that it would need from other rels are provided.
 *
 * We insist that the clause actually reference the target relation; this
 * prevents undesirable movement of degenerate join clauses, and ensures
 * that there is a unique place that a clause can be moved down to.
 *
 * We cannot move an outer-join clause into the non-nullable side of its
 * outer join, as that would change the results (rows would be suppressed
 * rather than being null-extended).
 *
 * Also there must not be an outer join below the clause that would null the
 * Vars coming from the target relation.  Otherwise the clause might give
 * results different from what it would give at its normal semantic level.
  *
 * Also, the join clause must not use any relations that have LATERAL
 * references to the target relation, since we could not put such rels on
 * the outer side of a nestloop with the target relation.
 */
bool join_clause_is_movable_to(RestrictInfo* rinfo, RelOptInfo *baserel)
{
    /* Clause must physically reference target rel */
    if (!bms_is_member(baserel->relid, rinfo->clause_relids))
        return false;
    
    /* Cannot move an outer-join clause into the join's outer side */
    if (bms_is_member(baserel->relid, rinfo->outer_relids))
        return false;
    
    /* Target rel must not be nullable below the clause */
    if (bms_is_member(baserel->relid, rinfo->nullable_relids))
        return false;
    
    /* Clause must not use any rels with LATERAL references to this rel */
    if (bms_overlap(baserel->lateral_referencers, rinfo->clause_relids))
        return false;
    
    return true;
}

/*
 * join_clause_is_movable_into
 *		Test whether a join clause is movable and can be evaluated within
 *		the current join context.
 *
 * currentrelids: the relids of the proposed evaluation location
 * current_and_outer: the union of currentrelids and the required_outer
 *		relids (parameterization's outer relations)
 *
 * The API would be a bit clearer if we passed the current relids and the
 * outer relids separately and did bms_union internally; but since most
 * callers need to apply this function to multiple clauses, we make the
 * caller perform the union.
 *
 * Obviously, the clause must only refer to Vars available from the current
 * relation plus the outer rels.  We also check that it does reference at
 * least one current Var, ensuring that the clause will be pushed down to
 * a unique place in a parameterized join tree.  And we check that we're
 * not pushing the clause into its outer-join outer side, nor down into
 * a lower outer join's inner side.
 *
 * The check about pushing a clause down into a lower outer join's inner side
 * is only approximate; it sometimes returns "false" when actually it would
 * be safe to use the clause here because we're still above the outer join
 * in question.  This is okay as long as the answers at different join levels
 * are consistent: it just means we might sometimes fail to push a clause as
 * far down as it could safely be pushed.  It's unclear whether it would be
 * worthwhile to do this more precisely.  (But if it's ever fixed to be
 * exactly accurate, there's an Assert in get_joinrel_parampathinfo() that
 * should be re-enabled.)
 *
 * Note: if this returns true, it means that the clause could be moved to
 * this join relation, but that doesn't mean that this is the lowest join
 * it could be moved to.  Caller may need to make additional calls to verify
 * that this doesn't succeed on either of the inputs of a proposed join.
 *
 * Note: get_joinrel_parampathinfo depends on the fact that if
 * current_and_outer is NULL, this function will always return false
 * (since one or the other of the first two tests must fail).
 */
bool join_clause_is_movable_into(RestrictInfo* rinfo, Relids currentrelids, Relids current_and_outer)
{
    /* Clause must be evaluatable given available context */
    if (!bms_is_subset(rinfo->clause_relids, current_and_outer))
        return false;

    /* Clause must physically reference at least one target rel */
    if (!bms_overlap(currentrelids, rinfo->clause_relids))
        return false;

    /* Cannot move an outer-join clause into the join's outer side */
    if (bms_overlap(currentrelids, rinfo->outer_relids))
        return false;

    /*
     * Target rel(s) must not be nullable below the clause.  This is
     * approximate, in the safe direction, because the current join might be
     * above the join where the nulling would happen, in which case the clause
     * would work correctly here.  But we don't have enough info to be sure.
     */
    if (bms_overlap(currentrelids, rinfo->nullable_relids))
        return false;

    return true;
}

