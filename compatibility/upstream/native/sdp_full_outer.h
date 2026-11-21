#ifndef SQL_SDP_FULL_OUTER_H
#define SQL_SDP_FULL_OUTER_H
#include "sql/item_cmpfunc.h"
#include "sql/parse_tree_nodes.h"
#include "sql/sql_parse.h"
#include "sql/sp_head.h"

// Keep each original operand's aliases and column binding. FULL is not a LEFT
// join with a second query, and ON is not attached to either input's filter.
class PT_sdp_full_outer final : public PT_joined_table {
  Item *m_on;
 public:
  PT_sdp_full_outer(const POS &pos,PT_table_reference *left,const POS &join_pos,
                    PT_table_reference *right,Item *on)
      :PT_joined_table(pos,left,join_pos,JTT_INNER,right),m_on(on) {}
  bool do_contextualize(Parse_context *pc) override {
    auto *qb= pc->select;
    if (qb->sdp_full_nest != nullptr) {
      my_error(ER_NOT_SUPPORTED_YET,MYF(0),"multiple FULL OUTER JOIN boundaries");
      return true;
    }
    if (contextualize_tabs(pc)) return true;
    if (m_left_table_ref->nested_join != nullptr ||
        m_right_table_ref->nested_join != nullptr) {
      my_error(ER_NOT_SUPPORTED_YET,MYF(0),"nested FULL OUTER JOIN operands");
      return true;
    }
    if (push_new_name_resolution_context(pc,m_left_table_ref,m_right_table_ref))
      return true;
    qb->parsing_place= CTX_ON;
    if (PT_joined_table::do_contextualize(pc) || m_on->itemize(pc,&m_on))
      return true;
    if (!m_on->is_bool_func()) {
      m_on= make_condition(pc,m_on);
      if (m_on == nullptr) return true;
    }
    pc->thd->lex->pop_context();
    qb->parsing_place= CTX_NONE;
    m_table_ref= qb->nest_last_join(pc->thd);
    if (m_table_ref == nullptr) return true;
    qb->sdp_full_left= m_left_table_ref;
    qb->sdp_full_right= m_right_table_ref;
    qb->sdp_full_nest= m_table_ref;
    qb->sdp_full_on= m_on;
    if (pc->thd->lex->sphead) pc->thd->lex->sphead->sdp_language_version=1;
    return false;
  }
};
#endif
