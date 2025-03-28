/* -------------------------------------------------------------------------
 *
 * restrictinfo.h
 *	  prototypes for restrictinfo.c.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/restrictinfo.h
 *
 * -------------------------------------------------------------------------
 */
#ifndef RESTRICTINFO_H
#define RESTRICTINFO_H

#include "nodes/relation.h"

/* Convenience macro for the common case of a valid-everywhere qual */
#define make_simple_restrictinfo(clause) make_restrictinfo(clause, true, false, false, 0, NULL, NULL, NULL, false)

extern RestrictInfo* make_restrictinfo(Expr* clause, bool is_pushed_down, bool outerjoin_delayed, bool pseudoconstant,
    Index security_level, Relids required_relids, Relids outer_relids, Relids nullable_relids, bool is_asof);
extern List* make_restrictinfo_from_bitmapqual(Path* bitmapqual, bool is_pushed_down, bool include_predicates);
extern List* make_restrictinfos_from_actual_clauses(PlannerInfo* root, List* clause_list);
extern bool restriction_is_or_clause(RestrictInfo* restrictinfo);
extern bool restriction_is_securely_promotable(RestrictInfo* restrictinfo, RelOptInfo* rel);
extern List* get_actual_clauses(List* restrictinfo_list);
extern List* get_all_actual_clauses(List* restrictinfo_list);
extern List* extract_actual_clauses(List* restrictinfo_list, bool pseudoconstant);
extern void extract_actual_join_clauses(List* restrictinfo_list, List** joinquals, List** otherquals);
extern bool join_clause_is_movable_to(RestrictInfo* rinfo, RelOptInfo *baserel);
extern bool join_clause_is_movable_into(RestrictInfo* rinfo, Relids currentrelids, Relids current_and_outer);
extern void initialize_bucket_size(RestrictInfo* info);
#endif /* RESTRICTINFO_H */
