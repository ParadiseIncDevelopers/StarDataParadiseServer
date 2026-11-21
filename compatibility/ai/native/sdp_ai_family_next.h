#ifndef SQL_SDP_AI_FAMILY_NEXT_H
#define SQL_SDP_AI_FAMILY_NEXT_H
#include "sql/item_json_func.h"
#include "sql-common/json_dom.h"

// Preserve SQL NULL versus JSON null/string while evaluating a supplied
// assistant value once. This helper is AST-only, not a SQL-callable builtin.
class Item_func_sdp_ready_value final : public Item_json_func {
 public:
  Item_func_sdp_ready_value(THD *thd,const POS &pos,Item *value)
      : Item_json_func(thd,pos,value) {}
  const char *func_name() const override { return "sdp_ready_value"; }
  bool resolve_type(THD *thd) override {
    if (Item_json_func::resolve_type(thd) || param_type_is_default(thd,0,1,MYSQL_TYPE_VARCHAR)) return true;
    set_nullable(false);return false;
  }
  bool val_json(Json_wrapper *result) override {
    Json_wrapper value;
    if (get_atom_null_as_null(args,0,func_name(),&m_value,&m_conversion_buffer,&value)) return error_json();
    auto document=create_dom_ptr<Json_object>();
    auto scalar=value.clone_dom();
    if (!document || !scalar || document->add_alias("value",std::move(scalar)) ||
        document->add_alias("source_sql_null",create_dom_ptr<Json_boolean>(args[0]->null_value)) ||
        document->add_alias("source_json",create_dom_ptr<Json_boolean>(args[0]->data_type()==MYSQL_TYPE_JSON))) return error_json();
    *result=Json_wrapper(std::move(document));null_value=false;return false;
  }
};
#endif
