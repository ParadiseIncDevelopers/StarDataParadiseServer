// Runtime atomicity/purity boundary for native leased tool execution.
#ifndef SQL_SDP_AI_TOOL_POLICY_H
#define SQL_SDP_AI_TOOL_POLICY_H
#include "sql/sdp_ai_knowledge.h"
#include "sql/sdp_ai_invoker.h"
#include "sql/handler.h"
#include "sql/select_lex_visitor.h"
#include "sql/sql_signal.h"

namespace sdp_ai_tool_policy {
inline bool application(const THD *thd) {
  return sdp_ai_invoker::active(thd) && !sdp_ai_invoker::worker_rpc_active(thd);
}
inline bool denied(THD *thd) {
  return sdp_knowledge::error(thd,61081,"42000","510001: Tool statements must preserve the authorized atomic action boundary.");
}
inline bool private_schema(const char *schema) {
  if (!schema) return false;
  for (const char *reserved:{"mysql","sys","information_schema","performance_schema","sdp_ai","sdp_jobs","sdp_hist","sdp_hist_remote","sdp_hist_data"})
    if (native_strcasecmp(schema,reserved)==0) return true;
  return false;
}
inline bool trigger_frame(const THD *thd) {
  return thd->sp_runtime_ctx && thd->sp_runtime_ctx->sp && thd->sp_runtime_ctx->sp->m_type==enum_sp_type::TRIGGER;
}
inline bool metadata_table(const Table_ref *table) {
  return table->db && table->table_name && native_strcasecmp(table->db,"information_schema")==0 &&
    (native_strcasecmp(table->table_name,"tables")==0 || native_strcasecmp(table->table_name,"columns")==0);
}
inline bool allowed(enum_sql_command command) {
  switch (command) {
    case SQLCOM_SELECT:case SQLCOM_INSERT:case SQLCOM_INSERT_SELECT:
    case SQLCOM_UPDATE:case SQLCOM_UPDATE_MULTI:case SQLCOM_DELETE:case SQLCOM_DELETE_MULTI:
    case SQLCOM_REPLACE:case SQLCOM_REPLACE_SELECT:case SQLCOM_DO:return true;
    // Transaction control, implicit-DDL commits, dynamic SQL, SET, user CALLs
    // and privilege/session changes require an explicitly supported executor.
    // Default-deny also covers future native command kinds.
    default:return false;
  }
}
inline bool unsafe_item(Item *item) {
  if (sdp_knowledge::pure_item(item,static_cast<size_t>(-1))) return true;
  if (item->type()!=Item::FUNC_ITEM) return false;
  const auto *function=static_cast<Item_func *>(item);
  // USER()/SESSION_USER()/SYSTEM_USER() read the physical transport account.
  // Application authorization uses the verified logical ACTOR/current owner,
  // never a pooled worker's session identity or user-variable contents.
  return function->functype()==Item_func::GUSERVAR_FUNC || function->functype()==Item_func::GSYSVAR_FUNC ||
    native_strcasecmp(function->func_name(),"user")==0;
}
class Expressions final : public Select_lex_visitor {
 public:
  bool bad=false;
 protected:
  bool visit_item(Item *item) override {
    if (unsafe_item(item)) bad=true;
    return bad;
  }
};
inline bool expression(THD *thd,Item *item,bool validate=false,bool trigger=false) {
  if ((!validate && !application(thd)) || !item) return false;
  trigger=trigger || trigger_frame(thd);
  if (trigger) for (Table_ref *table=thd->lex->query_tables;table;table=table->next_global)
    if (table->updating || (private_schema(table->db) && !metadata_table(table)) ||
        (table->lock_descriptor().type!=TL_READ && table->lock_descriptor().type!=TL_READ_DEFAULT)) return denied(thd);
  auto reject=[trigger](Item *value) {
    return unsafe_item(value) || (!trigger && value->type()==Item::SUBQUERY_ITEM);
  };
  return item->walk(&Item::walk_helper_thunk<decltype(reject)>,enum_walk::PREFIX|enum_walk::SUBQUERY,reinterpret_cast<uchar *>(&reject)) ? denied(thd) : false;
}
inline bool statement(THD *thd,bool validate=false,bool trigger=false) {
  if (!validate && !application(thd)) return false;
  LEX *lex=thd->lex;
  trigger=trigger || trigger_frame(thd);
  if (lex->sql_command==SQLCOM_SIGNAL) {
    const auto *command=dynamic_cast<Sql_cmd_common_signal *>(lex->m_sql_cmd);
    if (!command) return denied(thd);
    const auto *information=command->sdp_tool_information();
    if (information) for (Item *item:information->m_item) if (item && expression(thd,item,true,false)) return true;
    return false;
  }
  if (!allowed(lex->sql_command) ||
      (lex->result && !dynamic_cast<Query_result_send *>(lex->result))) return denied(thd);
  for (Table_ref *table=lex->query_tables;table;table=table->next_global)
    if (private_schema(table->db) && !(trigger && !table->updating && metadata_table(table))) return denied(thd);
  Expressions visitor;
  if (auto *command=dynamic_cast<Sql_cmd_dml *>(lex->m_sql_cmd)) {
    // accept() includes INSERT values and UPDATE assignments that do not all
    // appear in the SELECT field list. Its nested visitor return values are
    // not uniformly propagated, so keep the independent bad flag.
    command->accept(thd,&visitor);
  } else if (lex->unit) lex->unit->accept(&visitor);
  return visitor.bad ? denied(thd) : false;
}
inline bool instruction(THD *thd,sp_instr *instruction,bool validate=false,bool trigger=false) {
  if (!validate && !application(thd)) return false;
  LEX *saved=thd->lex;
  if (auto *lexical=dynamic_cast<sp_lex_instr *>(instruction)) thd->lex=const_cast<LEX *>(lexical->get_lex());
  auto restore_lex=create_scope_guard([&]() {thd->lex=saved;});
  trigger=trigger || trigger_frame(thd);
  if (auto *set=dynamic_cast<sp_instr_set *>(instruction)) return expression(thd,set->get_value_item(),true,trigger);
  if (auto *result=dynamic_cast<sp_instr_freturn *>(instruction)) return expression(thd,result->get_expr_item(),true,trigger);
  if (auto *branch=dynamic_cast<sp_lex_branch_instr *>(instruction)) return expression(thd,branch->sdp_tool_expression(),true,trigger);
  if (auto *field=dynamic_cast<sp_instr_set_trigger_field *>(instruction)) return expression(thd,field->sdp_tool_expression(),true,trigger);
  if (auto *statement_instruction=dynamic_cast<sp_instr_stmt *>(instruction)) {
    return statement(thd,true,trigger);
  }
  switch (instruction->type()) {
    case sp_instr::INSTR_JUMP:case sp_instr::INSTR_HPOP:case sp_instr::INSTR_ERROR:
    case sp_instr::INSTR_COND_HANDLER_PUSH_JUMP:case sp_instr::INSTR_COND_HANDLER_RETURN:return false;
    default:return denied(thd);
  }
}
inline bool write(THD *thd,TABLE *table) {
  if (!application(thd)) return false;
  if (table->s->tmp_table==INTERNAL_TMP_TABLE || table->s->tmp_table==SYSTEM_TMP_TABLE) return false;
  if (table->s->tmp_table!=NO_TMP_TABLE || private_schema(table->s->db.str) || !table->file->has_transactions()) return denied(thd);
  return false;
}
} // namespace sdp_ai_tool_policy
#endif
