// Native session state helpers; no SQL/session variable can forge dirty state.
#ifndef SDP_AI_STATE_H
#define SDP_AI_STATE_H
#include "sql/current_thd.h"
#include "sql/handler.h"
#include "sql/item_func.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/transaction_info.h"

class Item_func_sdp_transaction_dirty final : public Item_int_func {
 public:
  explicit Item_func_sdp_transaction_dirty(const POS &pos) : Item_int_func(pos) {}
  longlong val_int() override {
    assert(fixed);
    Transaction_ctx *transaction=current_thd->get_transaction();
    for (auto scope : {Transaction_ctx::SESSION,Transaction_ctx::STMT})
      for (const auto &engine : transaction->ha_trx_info(scope))
        if (engine.is_started() && engine.is_trx_read_write()) return 1;
    return 0;
  }
  bool do_itemize(Parse_context *pc,Item **res) override {
    if (skip_itemize(res)) return false;
    if (Item_int_func::do_itemize(pc,res)) return true;
    pc->thd->lex->safe_to_cache_query=false;
    pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    return false;
  }
  table_map get_initial_pseudo_tables() const override { return RAND_TABLE_BIT; }
  const char *func_name() const override { return "stardataparadise_transaction_dirty"; }
  bool resolve_type(THD *) override { set_data_type_bool();return false; }
};
#endif
