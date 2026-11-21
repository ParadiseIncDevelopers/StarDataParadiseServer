#ifndef SQL_SDP_FULL_OUTER_PLAN_H
#define SQL_SDP_FULL_OUTER_PLAN_H
#include <algorithm>
#include "sql/filesort.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/sdp_full_outer_source.h"
#include "sql/sql_optimizer.h"

// Iterator-planned legacy path: preserve the FULL boundary and apply WHERE,
// ORDER and LIMIT only after both NULL-complemented sides have been produced.
inline bool JOIN::sdp_optimize_full() {
  if (sdp_prepare_full_sources(thd,query_block)) return true;
  if (query_block->sdp_full_on->walk(&Item::cast_incompatible_args,
                                    enum_walk::POSTFIX,nullptr)) return true;
  tables= primary_tables= 2;
  const_tables= 0;
  auto scan=[&](Table_ref *source) {
    TABLE *table= source->table;
    AccessPath *path= NewTableScanAccessPath(thd,table,true);
    const double rows= std::max(1.0,static_cast<double>(table->file->stats.records));
    path->set_num_output_rows(rows);
    path->set_cost(rows);
    path->set_init_cost(0.0);
    path->set_init_once_cost(0.0);
    if (source->is_table_function()) {
      path= NewMaterializedTableFunctionAccessPath(thd,table,
                                                  source->table_function,path);
      path->set_num_output_rows(rows);
      path->set_cost(rows);
      path->set_init_cost(rows);
      path->set_init_once_cost(0.0);
    }
    return path;
  };
  auto *path= new (thd->mem_root) AccessPath;
  path->type= AccessPath::NESTED_LOOP_JOIN;
  auto &full= path->nested_loop_join();
  full.outer= scan(query_block->sdp_full_left);
  full.inner= scan(query_block->sdp_full_right);
  full.join_type= JoinType::FULL_OUTER;
  full.join_predicate= nullptr;
  full.already_expanded_predicates= true;
  full.pfs_batch_mode= false;
  full.sdp_full_condition= query_block->sdp_full_on;
  const double left_rows= full.outer->num_output_rows();
  const double right_rows= full.inner->num_output_rows();
  path->set_num_output_rows(std::max({left_rows,right_rows,left_rows*right_rows}));
  path->set_cost(full.outer->cost()+full.inner->cost()+left_rows*right_rows);
  path->set_init_cost(full.inner->cost());
  path->set_init_once_cost(0.0);
  if (where_cond != nullptr) {
    auto *filtered= NewFilterAccessPath(thd,path,where_cond);
    CopyBasicProperties(*path,filtered);
    path= filtered;
  }
  if (!order.empty()) {
    Mem_root_array<TABLE *> sort_tables(thd->mem_root);
    if (sort_tables.push_back(query_block->sdp_full_left->table) ||
        sort_tables.push_back(query_block->sdp_full_right->table)) return true;
    auto *sort= new (thd->mem_root) Filesort(
        thd,std::move(sort_tables),false,order.order,HA_POS_ERROR,
        false,false,false);
    if (filesorts_to_cleanup.push_back(sort)) return true;
    path= NewSortAccessPath(thd,path,sort,order.order,false);
  }
  path= attach_access_paths_for_having_qualify_limit(path);
  if (thd->is_error()) return true;
  best_rowcount= static_cast<ha_rows>(std::min(
      path->num_output_rows(),static_cast<double>(HA_POS_ERROR-1)));
  best_read= path->cost();
  m_root_access_path= path;
  // FULL's spool iterator cannot be offloaded to engines that only understand
  // the legacy LEFT/INNER join representation.
  set_plan_state(PLAN_READY);
  error= 0;
  return false;
}
#endif
