// Compile explicit model-tool registrations under the initiating owner ACL.
// Client JSON cannot supply executable SQL, a hash, or an authorization flag.
#ifndef SQL_SDP_AI_TOOL_REGISTRY_H
#define SQL_SDP_AI_TOOL_REGISTRY_H
#include "sql/sdp_knowledge_source.h"
#include "sql/sdp_ai_routine.h"
#include "sql/sdp_ai_action.h"

namespace sdp_ai_tool_registry {
inline bool denied(THD *thd) {
  return sdp_knowledge::error(thd,61081,"42000","510001: Explicit typed tool registration is unavailable or has changed.");
}
inline Json_dom_ptr knowledge(THD *thd,const std::string &schema,const std::string &name) {
  sdp_knowledge::Main_identity owner(thd);
  sdp_knowledge::Nested_read nested(thd);
  sdp_ai::Catalog_guard catalog;
  if (catalog.pin(thd)) {denied(thd);return {};}
  // The digest is produced by SQL from the exact stored JSON document, not
  // from a separately reconstructed or client-provided representation.
  const std::string value="sdp_ai.knowledge_value("+sdp_knowledge::sql_text(schema)+","+sdp_knowledge::sql_text(name)+")";
  const std::string source="SELECT JSON_OBJECT('document',"+value+",'definition_hash',SHA2(CAST("+value+" AS CHAR),256))";
  sp_rcontext *caller=thd->sp_runtime_ctx;thd->sp_runtime_ctx=nullptr;
  auto restore=create_scope_guard([&]() {thd->sp_runtime_ctx=caller;});
  sdp_ai_invoker::Worker_rpc_scope native_rpc(thd);
  Ed_connection connection(thd,true);LEX_STRING statement{const_cast<char *>(source.data()),source.size()};
  if (connection.execute_direct(statement)) {
    sdp_knowledge::error(thd,connection.get_last_errno(),connection.get_last_sqlstate(),connection.get_last_error());return {};
  }
  auto *result=connection.get_result_sets();
  if (!result || result->size()!=1 || result->get_field_count()!=1) {denied(thd);return {};}
  List<Ed_row> &rows=*result;const auto &cell=(*rows.head())[0];
  if (!cell.str || cell.length>4194304) {denied(thd);return {};}
  auto stored=sdp_knowledge::parse_document(thd,cell.str,cell.length);
  const auto *document=sdp_knowledge::member(stored.get(),"document");
  std::string id,hash;
  const auto *version=sdp_knowledge::member(document,"version");
  if (!sdp_knowledge::string_member(document,"id",&id) || !sdp_knowledge::string_member(stored.get(),"definition_hash",&hash) ||
      id.size()!=32 || hash.size()!=64 || !version ||
      (version->json_type()!=enum_json_type::J_INT && version->json_type()!=enum_json_type::J_UINT)) {denied(thd);return {};}
  sdp_knowledge::Definition definition;
  if (sdp_knowledge::read_definition(thd,document,&definition)) return {};
  auto program=sdp_knowledge::compile(thd,definition);
  sdp_knowledge::Table_locks dependencies(thd);
  if (!program || !sdp_knowledge::readonly_lex(thd,program.get(),definition.parameters.size(),&dependencies)) return {};
  for (uint index=0;index<program->instructions();++index)
    if (sdp_ai_tool_policy::instruction(thd,program->get_instr(index),true)) return {};
  auto parameters=sdp_knowledge::tool_parameters(program.get(),definition);
  auto descriptor=create_dom_ptr<Json_object>();
  if (!parameters || !descriptor || descriptor->add_alias("tool_id",create_dom_ptr<Json_string>(id)) ||
      descriptor->add_alias("kind",create_dom_ptr<Json_string>("knowledge")) ||
      descriptor->add_alias("schema_name",create_dom_ptr<Json_string>(schema)) || descriptor->add_alias("name",create_dom_ptr<Json_string>(name)) ||
      descriptor->add_alias("version",version->clone()) || descriptor->add_alias("definition_hash",create_dom_ptr<Json_string>(hash)) ||
      descriptor->add_alias("parameters",std::move(parameters)) ||
      descriptor->add_alias("writes",create_dom_ptr<Json_boolean>(false)) ||
      descriptor->add_alias("approval_required",create_dom_ptr<Json_boolean>(false))) return {};
  return descriptor;
}

inline Json_dom_ptr validate(THD *thd,const Json_dom *requests) {
  sdp_knowledge::Main_identity owner(thd);
  if (!requests || requests->json_type()!=enum_json_type::J_ARRAY) {denied(thd);return {};}
  const auto *items=static_cast<const Json_array *>(requests);
  if (items->size()>32) {denied(thd);return {};}
  auto output=create_dom_ptr<Json_array>();if (!output) return {};
  std::vector<std::string> seen;
  for (size_t index=0;index<items->size();++index) {
    const auto *request=(*items)[index];std::string kind,schema,name;
    if (!request || request->json_type()!=enum_json_type::J_OBJECT ||
        static_cast<const Json_object *>(request)->cardinality()!=3 ||
        !sdp_knowledge::string_member(request,"kind",&kind) || !sdp_knowledge::string_member(request,"schema_name",&schema) ||
        !sdp_knowledge::string_member(request,"name",&name) || !sdp_knowledge::valid_text(schema,64) || !sdp_knowledge::valid_text(name,64)) {denied(thd);return {};}
    if (kind!="knowledge" && kind!="function" && kind!="procedure") {denied(thd);return {};}
    Json_dom_ptr item;
    if (kind=="knowledge") item=knowledge(thd,schema,name);
    else {
      sdp_ai_routine::Routine routine(thd);
      if (routine.load(schema,name,kind=="function")) return {};
      item=routine.descriptor();
      if (!item) return {};
      auto *descriptor=static_cast<Json_object *>(item.get());std::string native_id;
      schema=routine.schema();name=routine.name();
      const std::string key=kind+'\0'+schema+'\0'+name;
      bool approval=false;ulonglong policy_version=0;
      if ((routine.writes() && !opt_sdp_ai_actions) || (kind=="procedure" &&
          sdp_ai_action::requires_approval(thd,schema,name,&approval,&policy_version))) {denied(thd);return {};}
      if (!sdp_knowledge::string_member(item.get(),"native_id",&native_id) ||
          descriptor->add_alias("tool_id",create_dom_ptr<Json_string>(sdp_ai_routine::digest(key+'\0'+native_id).substr(0,32))) ||
          descriptor->add_alias("approval_required",create_dom_ptr<Json_boolean>(approval)) ||
          descriptor->add_alias("approval_policy_version",create_dom_ptr<Json_uint>(policy_version))) return {};
    }
    if (!item) return {};
    std::string canonical_schema,canonical_name;
    if (!sdp_knowledge::string_member(item.get(),"schema_name",&canonical_schema) ||
        !sdp_knowledge::string_member(item.get(),"name",&canonical_name)) {denied(thd);return {};}
    const std::string key=kind+'\0'+canonical_schema+'\0'+canonical_name;
    if (std::find(seen.begin(),seen.end(),key)!=seen.end()) {denied(thd);return {};}
    seen.push_back(key);
    auto *descriptor=static_cast<Json_object *>(item.get());std::string id;
    if (!sdp_knowledge::string_member(item.get(),"tool_id",&id) || id.size()!=32 ||
        descriptor->add_alias("provider_name",create_dom_ptr<Json_string>("tool_"+id)) ||
        output->append_alias(std::move(item))) return {};
  }
  return output;
}
} // namespace sdp_ai_tool_registry

class Item_func_sdp_tools_validate final : public Item_str_func {
 public:
  Item_func_sdp_tools_validate(const POS &pos,Item *document) : Item_str_func(pos,document) {}
  const char *func_name() const override {return "stardataparadise_tools_validate";}
  table_map get_initial_pseudo_tables() const override {return RAND_TABLE_BIT;}
  bool resolve_type(THD *) override {set_data_type_string(4U*1024*1024);return false;}
  bool do_itemize(Parse_context *pc,Item **result) override {
    if (skip_itemize(result)) return false;
    if (Item_str_func::do_itemize(pc,result)) return true;
    pc->thd->lex->safe_to_cache_query=false;
    pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);return false;
  }
  String *val_str(String *output) override {
    // Stored-program expression Items are reused after a handled error.
    // error_str() may have marked the prior evaluation NULL.
    null_value=false;
    String buffer;String *value=args[0]->val_str(&buffer);
    if (!value || value->length()>1048576) {sdp_ai_tool_registry::denied(current_thd);return error_str();}
    auto input=sdp_knowledge::parse_document(current_thd,value->ptr(),value->length());
    auto compiled=input ? sdp_ai_tool_registry::validate(current_thd,input.get()) : Json_dom_ptr();
    if (!compiled) return error_str();
    Json_wrapper document(std::move(compiled));output->set_charset(&my_charset_utf8mb4_bin);output->length(0);
    if (document.to_string(output,true,func_name(),[&]() {sdp_ai_tool_registry::denied(current_thd);})) return error_str();
    return output;
  }
};
#endif
