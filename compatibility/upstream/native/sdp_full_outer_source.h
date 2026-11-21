#ifndef SQL_SDP_FULL_OUTER_SOURCE_H
#define SQL_SDP_FULL_OUTER_SOURCE_H
#include "sql/opt_explain_format.h"
#include "sql/sdp_knowledge_source.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/table.h"
#include "sql/table_function.h"

inline bool sdp_prepare_full_sources(THD *thd,Query_block *qb) {
  auto fail=[&](const char *reason) {
    my_error(ER_NOT_SUPPORTED_YET,MYF(0),reason);return true;
  };
  if (qb->leaf_table_count != 2 || qb->get_table_list() == nullptr ||
      qb->get_table_list()->next_local == nullptr ||
      qb->get_table_list()->next_local->next_local != nullptr)
    return fail("FULL OUTER JOIN with additional or nested inputs");
  if (qb->is_grouped() || qb->is_distinct() || qb->has_wfs() ||
      qb->having_cond() != nullptr || qb->qualify_cond() != nullptr ||
      qb->has_ft_funcs() || qb->first_inner_query_expression() != nullptr)
    return fail("FULL OUTER JOIN with grouping, DISTINCT, windows or subqueries");
  if (thd->lex->is_explain() &&
      !thd->lex->explain_format->is_iterator_based(thd,thd))
    return fail("FULL OUTER JOIN requires EXPLAIN FORMAT=TREE or JSON version 2");
  const table_map local_map= qb->sdp_full_left->map() | qb->sdp_full_right->map();
  for (Table_ref *source : {qb->sdp_full_left,qb->sdp_full_right}) {
    if (source == nullptr || source->table == nullptr || source->schema_table ||
        source->nested_join || source->is_view_or_derived() ||
        source->common_table_expr() || source->is_recursive_reference() ||
        source->is_merged())
      return fail("FULL OUTER JOIN requires native base or typed Knowledge inputs");
    if (source->is_table_function()) {
      if (!sdp_knowledge_materialized_source(source))
        return fail("FULL OUTER JOIN with an unsupported table function");
      if (source->table_function->used_tables() & local_map)
        return fail("FULL OUTER JOIN with a Knowledge input dependent on its other input");
    } else if (!source->is_base_table()) {
      return fail("FULL OUTER JOIN with an unsupported source");
    }
    const auto &lock= source->lock_descriptor();
    if (lock.type == TL_READ_WITH_SHARED_LOCKS ||
        lock.type >= TL_WRITE_ALLOW_WRITE || lock.action != THR_DEFAULT)
      return fail("FULL OUTER JOIN with locking reads or update targets");
    // Nullability must be established before SELECT/ON/WHERE field metadata is
    // fixed, including for a NOT NULL base column on either complemented side.
    source->table->set_nullable();
  }
  return false;
}
#endif
