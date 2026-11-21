// Protected native tool dispatch. Provider text never becomes executable SQL.
#ifndef SQL_SDP_AI_TOOL_H
#define SQL_SDP_AI_TOOL_H
#include "sdp_ai_knowledge.h"
#include "sdp_ai_invoker.h"
#include "sdp_routine_guard.h"
#include "sdp_ai_catalog_guard.h"
#include "sdp_ai_routine.h"
#include "sdp_ai_principal.h"
#include "sdp_ai_action.h"
#include "sql/transaction.h"
#include "sql/sql_timer.h"

namespace sdp_ai_tool {
inline bool denied(THD *thd) {
  return sdp_knowledge::error(thd,61081,"42000","510001: Tool execution is not authorized.");
}
inline void report_outcome(THD *thd,const std::string &run_id,const std::string &call_id,
    bool rolled_back,uint cause,const std::string &cause_state) {
  auto context=create_dom_ptr<Json_object>();auto retry=create_dom_ptr<Json_object>();
  if (!context || !retry || retry->add_alias("action",create_dom_ptr<Json_string>(rolled_back?"NONE":"QUERY_RUN")) ||
      context->add_alias("phase",create_dom_ptr<Json_string>("tool")) || context->add_alias("commit_state",create_dom_ptr<Json_string>("committed")) ||
      context->add_alias("run_id",create_dom_ptr<Json_string>(run_id)) || context->add_alias("call_id",create_dom_ptr<Json_string>(call_id)) ||
      context->add_alias("action_commit_state",create_dom_ptr<Json_string>(rolled_back?"rolled_back":"unknown")) ||
      context->add_alias("unknown_outcome",create_dom_ptr<Json_boolean>(!rolled_back)) || context->add_alias("retry",std::move(retry)) ||
      context->add_alias("cause_native_errno",create_dom_ptr<Json_uint>(cause)) || context->add_alias("cause_sqlstate",create_dom_ptr<Json_string>(cause_state)) ||
      context->add_alias("message",create_dom_ptr<Json_string>(rolled_back?"Tool transaction rolled back.":"Action outcome must be queried.")) ||
      (rolled_back && context->add_alias("run_state",create_dom_ptr<Json_string>("running")))) return;
  String encoded;Json_wrapper wrapper(std::move(context));
  if (wrapper.to_string(&encoded,true,"Native tool outcome",JsonDepthErrorHandler)) return;
  const std::string message=std::string(rolled_back?"510005: ":"511002: ")+std::string(encoded.ptr(),encoded.length());
  thd->clear_error();
  sdp_knowledge::error(thd,rolled_back?61085:61092,rolled_back?"45000":"HY000",message.c_str());
}
class Deadline final {
  THD *m_thd;
  bool m_owned=false;
 public:
  explicit Deadline(THD *thd) : m_thd(thd) {}
  ~Deadline() {finish();}
  bool arm(const Json_dom *record) {
    const auto *value=sdp_knowledge::member(record,"timeout_ms");
    const ulonglong timeout=value && value->json_type()==enum_json_type::J_UINT ? static_cast<const Json_uint *>(value)->value() :
      value && value->json_type()==enum_json_type::J_INT && static_cast<const Json_int *>(value)->value()>0 ? static_cast<const Json_int *>(value)->value() : 0;
    if (timeout<1 || timeout>3600000 || m_thd->timer || m_thd->killed!=THD::NOT_KILLED) return denied(m_thd);
    m_thd->timer=thd_timer_set(m_thd,m_thd->timer_cache,static_cast<ulong>(timeout));m_thd->timer_cache=nullptr;
    m_owned=m_thd->timer!=nullptr;
    return m_owned ? false : denied(m_thd);
  }
  bool finish() {
    if (m_owned) {m_thd->timer_cache=thd_timer_reset(m_thd->timer);m_thd->timer=nullptr;m_owned=false;}
    // Never clear killed: an explicit concurrent KILL must remain effective.
    // Timeout causes the executor's owned transaction to roll back completely.
    return m_thd->killed!=THD::NOT_KILLED;
  }
  bool expired() const {return m_thd->killed!=THD::NOT_KILLED;}
};
inline bool hex_id(const std::string &value,size_t length) {
  return value.size()==length && std::all_of(value.begin(),value.end(),[](char c) {return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
inline bool principal(const std::string &source,std::string *user,std::string *host) {
  bool escaped=false,separated=false;
  for (char c:source) {
    if (escaped) {
      if (c!='\\' && c!='@') return false;
      (separated?*host:*user)+=c;escaped=false;
    } else if (c=='\\') escaped=true;
    else if (c=='@') {if (separated) return false;separated=true;}
    else (separated?*host:*user)+=c;
  }
  return !escaped && separated && !user->empty() && user->size()<=USERNAME_LENGTH && !host->empty() && host->size()<=HOSTNAME_LENGTH && source.find('\0')==std::string::npos;
}
inline Json_dom_ptr rpc(THD *thd,const char *routine,std::string source,bool document=true) {
  sdp_ai::Catalog_guard trusted;
  if (trusted.pin(thd,true)) {denied(thd);return {};}
  bool known=false;
  for (const char *allowed:{"assert_worker","begin_tool_call","knowledge_tool_document","finish_tool_call","tool_owner_context"})
    if (strcmp(allowed,routine)==0) known=true;
  if (!known) {denied(thd);return {};}
  // This bounded private identity can access product metadata only. Never
  // carry it into application expressions or model-selected routine bodies.
  Security_context context;Security_context *saved=nullptr;
  const LEX_CSTRING user{STRING_WITH_LEN("sdp_ai_internal")},host{STRING_WITH_LEN("localhost")};
  if (context.change_security_context(thd,user,host,"sdp_ai",&saved,true)) return {};
  auto restore_identity=create_scope_guard([&]() {context.restore_security_context(thd,saved);});
  sdp_ai_invoker::Metadata_timezone_scope metadata_time(thd);
  sdp_ai_invoker::Worker_rpc_scope worker_rpc(thd);
  sp_rcontext *outer=thd->sp_runtime_ctx;thd->sp_runtime_ctx=nullptr;
  auto restore=create_scope_guard([&]() {thd->sp_runtime_ctx=outer;});
  Ed_connection connection(thd,true);LEX_STRING query{source.data(),source.size()};
  if (connection.execute_direct(query)) {
    sdp_knowledge::error(thd,connection.get_last_errno(),connection.get_last_sqlstate(),connection.get_last_error());return {};
  }
  if (!document) return create_dom_ptr<Json_boolean>(true);
  auto *result=connection.get_result_sets();
  if (!result || result->size()!=1 || result->get_field_count()!=1) {denied(thd);return {};}
  List<Ed_row> &rows=*result;const auto &value=(*rows.head())[0];
  if (!value.str || value.length>4194304) {denied(thd);return {};}
  return sdp_knowledge::parse_document(thd,value.str,value.length);
}
inline Item *argument(THD *thd,const Json_dom *value) {
  if (!value) return nullptr;
  switch (value->json_type()) {
    case enum_json_type::J_NULL:return new(thd->mem_root) Item_null();
    case enum_json_type::J_BOOLEAN:return static_cast<const Json_boolean*>(value)->value()
      ? static_cast<Item*>(new(thd->mem_root) Item_func_true()) : static_cast<Item*>(new(thd->mem_root) Item_func_false());
    case enum_json_type::J_INT:return new(thd->mem_root) Item_int(static_cast<const Json_int*>(value)->value());
    case enum_json_type::J_UINT:return new(thd->mem_root) Item_uint(static_cast<const Json_uint*>(value)->value());
    case enum_json_type::J_DECIMAL:{my_decimal number=*static_cast<const Json_decimal*>(value)->value();return new(thd->mem_root) Item_decimal(&number);}
    case enum_json_type::J_DOUBLE:return new(thd->mem_root) Item_float(Name_string("",0),static_cast<const Json_double*>(value)->value(),DECIMAL_NOT_SPECIFIED,DBL_DIG+8);
    case enum_json_type::J_STRING:{
      const auto &text=static_cast<const Json_string*>(value)->value();char *owned=thd->strmake(text.data(),text.size());
      return owned ? new(thd->mem_root) Item_string(owned,text.size(),&my_charset_utf8mb4_bin) : nullptr;
    }
    case enum_json_type::J_ARRAY:case enum_json_type::J_OBJECT:{
      auto *wrapper=new(thd->mem_root) Json_wrapper(value->clone());
      return wrapper ? new(thd->mem_root) Item_json(unique_ptr_destroy_only<Json_wrapper>(wrapper),Item_name_string()) : nullptr;
    }
    default:return nullptr;
  }
}
inline bool canonical_decimal(const std::string &text) {
  if (text.empty() || text.size()>70) return false;
  size_t index=text[0]=='-' ? 1 : 0,start=index;
  while (index<text.size() && text[index]>='0' && text[index]<='9') ++index;
  if (index==start || (index-start>1 && text[start]=='0')) return false;
  if (index==text.size()) return true;
  if (text[index++]!='.') return false;
  start=index;while (index<text.size() && text[index]>='0' && text[index]<='9') ++index;
  return index==text.size() && index>start;
}
inline bool materialize_knowledge(THD *thd,const Json_dom *snapshot,const Json_dom *arguments,Json_array *output,Json_array *columns) {
  std::string id,hash;
  if (!sdp_knowledge::string_member(snapshot,"tool_id",&id) || !hex_id(id,32) ||
      !sdp_knowledge::string_member(snapshot,"definition_hash",&hash) || !hex_id(hash,64)) return denied(thd);
  const auto *version=sdp_knowledge::member(snapshot,"version");
  ulonglong version_value=version && version->json_type()==enum_json_type::J_UINT ? static_cast<const Json_uint*>(version)->value() :
    version && version->json_type()==enum_json_type::J_INT && static_cast<const Json_int*>(version)->value()>0 ? static_cast<const Json_int*>(version)->value() : 0;
  if (!version_value || !arguments || arguments->json_type()!=enum_json_type::J_OBJECT) return denied(thd);
  auto document=rpc(thd,"knowledge_tool_document","CALL sdp_ai.knowledge_tool_document("+sdp_knowledge::sql_text(id)+","+std::to_string(version_value)+","+sdp_knowledge::sql_text(hash)+")");
  sdp_knowledge::Definition definition;
  if (!document || sdp_knowledge::read_definition(thd,document.get(),&definition)) return true;
  if (static_cast<const Json_object*>(arguments)->cardinality()!=definition.parameters.size()) return sdp_knowledge::shape_error(thd);
  mem_root_deque<Item*> values(thd->mem_root);
  for (const auto &parameter:definition.parameters) {
    const auto *source=sdp_knowledge::member(arguments,parameter.name.c_str());
    if (parameter.type.rfind("DECIMAL(",0)==0 && source && source->json_type()!=enum_json_type::J_NULL &&
        (source->json_type()!=enum_json_type::J_STRING || !canonical_decimal(static_cast<const Json_string*>(source)->value()))) return sdp_knowledge::shape_error(thd);
    Item *value=argument(thd,source);
    if (!value) return sdp_knowledge::shape_error(thd);values.push_back(value);
  }
  for (const auto &column:definition.results) {
    auto item=create_dom_ptr<Json_object>();
    if (!item || item->add_alias("name",create_dom_ptr<Json_string>(column.name)) ||
        item->add_alias("type",create_dom_ptr<Json_string>(column.type)) || item->add_alias("nullable",create_dom_ptr<Json_boolean>(column.nullable)) ||
        columns->append_alias(std::move(item))) return true;
  }
  return sdp_knowledge::execute(thd,definition,values,output);
}
inline Json_dom_ptr materialize_routine(THD *thd,const std::string &call_id,const Json_dom *record,const Json_dom *snapshot,const Json_dom *arguments) {
  std::string kind,schema,name,hash,native_id;
  if (!sdp_knowledge::string_member(snapshot,"kind",&kind) || (kind!="function" && kind!="procedure") ||
      !sdp_knowledge::string_member(snapshot,"schema_name",&schema) || !sdp_knowledge::string_member(snapshot,"name",&name) ||
      !sdp_knowledge::string_member(snapshot,"definition_hash",&hash) || !hex_id(hash,64) ||
      !sdp_knowledge::string_member(snapshot,"native_id",&native_id) || !arguments || arguments->json_type()!=enum_json_type::J_OBJECT) {denied(thd);return {};}
  sdp_ai_routine::Routine routine(thd);
  if (routine.load(schema,name,kind=="function")) return {};
  auto current=routine.descriptor();std::string current_id;
  if (!current || routine.hash()!=hash || !sdp_knowledge::string_member(current.get(),"native_id",&current_id) ||
      current_id!=native_id || static_cast<const Json_object *>(arguments)->cardinality()!=routine.definition().parameters.size()) {denied(thd);return {};}
  bool approval=false;ulonglong policy_version=0;
  if ((routine.writes() && !opt_sdp_ai_actions) || (kind=="procedure" &&
      sdp_ai_action::requires_approval(thd,schema,name,&approval,&policy_version))) {denied(thd);return {};}
  const auto *pinned_approval=sdp_knowledge::member(snapshot,"approval_required");
  const auto *pinned_policy=sdp_knowledge::member(snapshot,"approval_policy_version");
  const ulonglong pinned_version=pinned_policy && pinned_policy->json_type()==enum_json_type::J_UINT ? static_cast<const Json_uint *>(pinned_policy)->value() :
    pinned_policy && pinned_policy->json_type()==enum_json_type::J_INT && static_cast<const Json_int *>(pinned_policy)->value()>=0 ? static_cast<const Json_int *>(pinned_policy)->value() : ULLONG_MAX;
  if (!pinned_approval || pinned_approval->json_type()!=enum_json_type::J_BOOLEAN ||
      static_cast<const Json_boolean *>(pinned_approval)->value()!=approval || pinned_version!=policy_version) {sdp_ai_action::stale(thd);return {};}
  sdp_ai_action::Execution action(thd);
  if (approval) {
    std::string action_id;
    if (!sdp_knowledge::string_member(record,"action_id",&action_id) || !hex_id(action_id,32)) {sdp_ai_action::required(thd);return {};}
    if (action.consume(action_id,call_id,snapshot,arguments)) return {};
    if (action.replayed()) {
      const auto *result=sdp_knowledge::member(action.receipt(),"result");
      if (!result) {denied(thd);return {};}
      return result->clone();
    }
    if (action.validate(routine)) return {};
  }
  mem_root_deque<Item *> values(thd->mem_root);
  for (const auto &parameter:routine.definition().parameters) {
    const auto *source=sdp_knowledge::member(arguments,parameter.name.c_str());
    if (parameter.type.rfind("DECIMAL(",0)==0 && source && source->json_type()!=enum_json_type::J_NULL &&
        (source->json_type()!=enum_json_type::J_STRING || !canonical_decimal(static_cast<const Json_string *>(source)->value()))) {sdp_knowledge::shape_error(thd);return {};}
    Item *value=argument(thd,source);
    if (!value) {sdp_knowledge::shape_error(thd);return {};}
    values.push_back(value);
  }
  auto result=routine.invoke(values);
  if (result && approval && action.finish(result.get())) return {};
  return result;
}
inline bool send_document(THD *thd,Json_dom_ptr document) {
  auto *wrapper=new(thd->mem_root) Json_wrapper(std::move(document));
  if (!wrapper) return true;
  Item *item=new(thd->mem_root) Item_json(unique_ptr_destroy_only<Json_wrapper>(wrapper),Item_name_string(Name_string("document",8)));
  if (!item) return true;
  mem_root_deque<Item*> fields(thd->mem_root);fields.push_back(item);Query_result_send output;
  return output.send_result_set_metadata(thd,fields,Protocol::SEND_NUM_ROWS|Protocol::SEND_EOF) || output.send_data(thd,fields) || output.send_eof(thd);
}

inline bool execute(THD *thd,const std::string &call_id) {
  if (!hex_id(call_id,32) || thd->in_multi_stmt_transaction_mode() || thd->sp_runtime_ctx || thd->in_sub_stmt ||
      sdp_ai_invoker::active(thd) || sdp_ai_invoker::dispatch_scope) return denied(thd);
  // Auth DDL locks ACL before cleaning approval/catalog rows. Use the same
  // order here, and pin current grants until this bounded local transaction
  // commits or rolls back. No provider operation occurs inside this scope.
  Acl_cache_lock_guard authority(thd,Acl_cache_lock_mode::READ_MODE);
  if (!authority.lock()) return true;
  if (!rpc(thd,"assert_worker","CALL sdp_ai.assert_worker()",false)) return true;
  sdp_ai_invoker::Dispatch_scope dispatch(thd,call_id);
  const auto prior_isolation=thd->tx_isolation;
  thd->tx_isolation=ISO_REPEATABLE_READ;
  auto restore_isolation=create_scope_guard([&]() {thd->tx_isolation=prior_isolation;});
  if (trans_begin(thd)) return true;
  bool committed=false,commit_attempted=false,rollback_done=false;
  std::string run_id;
  const auto rollback_owned=[&]() {
    if (committed || rollback_done) return;
    rollback_done=true;
    const auto *area=thd->get_stmt_da();
    const uint cause=thd->is_error()?area->mysql_errno():0;
    const std::string cause_state=thd->is_error()?area->returned_sqlstate():"HY000";
    const bool statement_failed=trans_rollback_stmt(thd);
    const bool transaction_failed=trans_rollback(thd);
    if (!run_id.empty()) {
      try {
        report_outcome(thd,run_id,call_id,!commit_attempted && !statement_failed && !transaction_failed,cause,cause_state);
      } catch (...) {
        // Cleanup must not throw while an allocation failure is unwinding.
        // Without a serialized proof the caller must query the receipt.
        thd->clear_error();thd->get_stmt_da()->set_error_status(61092,"511002: Tool outcome must be queried.","HY000");
      }
    }
  };
  auto rollback=create_scope_guard([&]() {rollback_owned();thd->mdl_context.release_transactional_locks();});
  const auto commit_owned=[&]() {
    if (trans_commit_stmt(thd)) return true;
    commit_attempted=true;
    if (trans_commit(thd)) return true;
    committed=true;return false;
  };
  auto record=rpc(thd,"begin_tool_call","CALL sdp_ai.begin_tool_call("+sdp_knowledge::sql_text(call_id)+")");
  if (!record) return true;
  if (!sdp_knowledge::string_member(record.get(),"run_id",&run_id) || !hex_id(run_id,32)) return denied(thd);
  std::string state;sdp_knowledge::string_member(record.get(),"state",&state);
  if (state!="running") {
    if (commit_owned()) return true;
    return send_document(thd,std::move(record));
  }
  Deadline deadline(thd);if (deadline.arm(record.get())) return true;
  std::string owner,user,host,kind,schema;
  const auto *snapshot=sdp_knowledge::member(record.get(),"snapshot");
  if (!sdp_knowledge::string_member(record.get(),"owner",&owner) || !principal(owner,&user,&host) ||
      !sdp_knowledge::string_member(snapshot,"kind",&kind)) return denied(thd);
  if (kind!="knowledge" && kind!="function" && kind!="procedure") return denied(thd);
  Security_context context;Security_context *saved=nullptr;
  const LEX_CSTRING owner_user{user.c_str(),user.size()},owner_host{host.c_str(),host.size()};
  if (context.change_security_context(thd,owner_user,owner_host,"",&saved,true)) return true;
  auto restore=create_scope_guard([&]() {context.restore_security_context(thd,saved);});
  if (context.account_is_locked() || context.password_expired() || sdp_ai_principal::account(&context)!=owner) return denied(thd);
  const auto *execution=sdp_knowledge::member(record.get(),"execution_context");
  if (sdp_ai_principal::activate(thd,&context,sdp_knowledge::member(execution,"roles"))) return true;
  std::string timezone;
  sdp_ai_invoker::Session_timezone_scope owner_timezone(thd);
  if (!sdp_knowledge::string_member(execution,"time_zone",&timezone) || owner_timezone.apply(timezone)) return denied(thd);
  sdp_ai_invoker::Scope scope(thd,&context,call_id);
  // Abort app writes while its verified logical owner identity still exists.
  // The outer guard also covers failures before entering this owner scope.
  auto rollback_owner=create_scope_guard([&]() {rollback_owned();});
  if (!rpc(thd,"tool_owner_context","CALL sdp_ai.tool_owner_context("+sdp_knowledge::sql_text(call_id)+")")) return true;
  std::string savepoint_name="__sdp_tool_"+call_id;
  LEX_STRING savepoint{savepoint_name.data(),savepoint_name.size()};
  if (trans_savepoint(thd,savepoint)) return true;
  auto rows=create_dom_ptr<Json_array>();auto columns=create_dom_ptr<Json_array>();
  Json_dom_ptr failure,materialized;
  bool failed=false;
  if (kind=="knowledge") {
    failed=!rows || !columns || materialize_knowledge(thd,snapshot,sdp_knowledge::member(record.get(),"arguments"),rows.get(),columns.get());
    if (!failed) {
      auto value=create_dom_ptr<Json_object>();
      failed=!value || value->add_alias("columns",std::move(columns)) || value->add_alias("rows",std::move(rows));
      materialized=std::move(value);
    }
  } else {
    materialized=materialize_routine(thd,call_id,record.get(),snapshot,sdp_knowledge::member(record.get(),"arguments"));
    failed=!materialized;
  }
  // Keep the one invocation deadline armed through receipt publication and
  // the physical commit. Application locks must not outlive that budget
  // merely because the routine body has returned.
  if (deadline.expired()) {if (!thd->is_error()) thd->send_kill_message();return true;}
  if (failed) {
    const auto *area=thd->get_stmt_da();
    auto detail=create_dom_ptr<Json_object>();
    const std::string message=thd->is_error() ? std::string(area->message_text()).substr(0,1024) : "Tool result allocation failed.";
    if (!detail || detail->add_alias("code",create_dom_ptr<Json_int>(510001)) ||
        detail->add_alias("native_errno",create_dom_ptr<Json_uint>(thd->is_error()?area->mysql_errno():0)) ||
        detail->add_alias("sqlstate",create_dom_ptr<Json_string>(thd->is_error()?area->returned_sqlstate():"HY001")) ||
        detail->add_alias("message",create_dom_ptr<Json_string>(message))) return true;
    failure=std::move(detail);thd->clear_error();
    if (trans_rollback_to_savepoint(thd,savepoint)) return true;
  }
  if (trans_release_savepoint(thd,savepoint)) return true;
  Json_dom_ptr result_document;
  if (failure) result_document=create_dom_ptr<Json_null>();else result_document=std::move(materialized);
  String encoded,error_encoded;Json_wrapper result(std::move(result_document));
  if (result.to_string(&encoded,true,"AI tool",JsonDepthErrorHandler)) return true;
  Json_wrapper error_result(failure ? failure->clone() : create_dom_ptr<Json_null>());
  if (error_result.to_string(&error_encoded,true,"AI tool",JsonDepthErrorHandler)) return true;
  Json_dom_ptr receipt;
  {
    sdp_ai_invoker::Publication_scope publication(thd);thd->set_security_context(&thd->m_main_security_ctx);
    auto restore_owner=create_scope_guard([&]() {thd->set_security_context(&context);});
    receipt=rpc(thd,"finish_tool_call","CALL sdp_ai.finish_tool_call("+sdp_knowledge::sql_text(call_id)+",CAST("+
      sdp_knowledge::sql_text(std::string(encoded.ptr(),encoded.length()))+" AS JSON),CAST("+
      sdp_knowledge::sql_text(std::string(error_encoded.ptr(),error_encoded.length()))+" AS JSON))");
  }
  if (!receipt || commit_owned()) return true;
  return send_document(thd,std::move(receipt));
}
} // namespace sdp_ai_tool

class Sql_cmd_sdp_execute_tool final : public Sql_cmd {
  Item *m_call;
 public:
  explicit Sql_cmd_sdp_execute_tool(Item *call) : m_call(call) {}
  enum_sql_command sql_command_code() const override {return SQLCOM_CALL;}
  bool are_dynamic_parameters_allowed() const override {return true;}
  bool execute(THD *thd) override {
    auto reject=[](Item *item) {return sdp_knowledge::pure_item(item,static_cast<size_t>(-1)) || item->type()==Item::SUBQUERY_ITEM;};
    if (m_call->walk(&Item::walk_helper_thunk<decltype(reject)>,enum_walk::PREFIX|enum_walk::SUBQUERY,reinterpret_cast<uchar*>(&reject))) return sdp_ai_tool::denied(thd);
    if (!m_call->fixed && m_call->fix_fields(thd,&m_call)) return true;
    String buffer;String *value=m_call->val_str(&buffer);
    return !value ? sdp_ai_tool::denied(thd) : sdp_ai_tool::execute(thd,std::string(value->ptr(),value->length()));
  }
};
class PT_sdp_execute_tool final : public Parse_tree_root {
  Item *m_call;
 public:
  PT_sdp_execute_tool(const POS &pos,Item *call) : Parse_tree_root(pos),m_call(call) {}
  Sql_cmd *make_cmd(THD *thd) override {
    thd->lex->sql_command=SQLCOM_CALL;Parse_context context(thd,thd->lex->query_block);
    if (m_call->itemize(&context,&m_call)) return nullptr;
    return new(thd->mem_root) Sql_cmd_sdp_execute_tool(m_call);
  }
};
#endif
