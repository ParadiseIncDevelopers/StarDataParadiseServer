// Typed native action approval. Product metadata SQL is a bounded storage RPC;
// no provider-generated name or SQL text is ever executed as application code.
#ifndef SQL_SDP_AI_ACTION_H
#define SQL_SDP_AI_ACTION_H
#include "sql/sdp_ai_principal.h"
#include "sql/sdp_ai_identity.h"
#include "sql/sdp_ai_routine.h"
#include "sql/sdp_ai_capture.h"
#include "sql/sdp_ai_local_ast.h"
#include "sql/sdp_knowledge_source.h"
#include "sql/dd/types/index.h"
#include "sql/dd/types/index_element.h"
#include "sql/transaction.h"
#include "sql/transaction_info.h"
#include "sql/sql_timer.h"
#include "sql/tztime.h"
#include "sql/key.h"

extern bool opt_sdp_ai_actions;
namespace sdp_ai_action {
inline bool denied(THD *thd) {return sdp_knowledge::error(thd,61081,"42000","510001: Action is outside the current authorized approval contract.");}
inline bool stale(THD *thd) {return sdp_knowledge::error(thd,61084,"40001","510004: Approval definition, arguments, resource state, policy or authority has changed.");}
inline bool required(THD *thd) {return sdp_knowledge::error(thd,61083,"42000","510003: Explicit action approval is required.");}
inline bool unsupported(THD *thd) {return sdp_knowledge::error(thd,61081,"0A000","510001: Action resources require bounded InnoDB tables with primary keys and supported lossless types.");}
inline bool hex_id(const std::string &value,size_t size) {
  return value.size()==size && std::all_of(value.begin(),value.end(),[](char c) {return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
inline std::string encode(const Json_dom *value) {
  if (!value) return {};String data;Json_wrapper wrapper(value->clone());
  if (wrapper.to_string(&data,true,"AI action",JsonDepthErrorHandler)) return {};
  return std::string(data.ptr(),data.length());
}
class Dispatch_scope;
inline thread_local const Dispatch_scope *dispatcher=nullptr;
class Dispatch_scope final {
  THD *m_thd;std::string m_operation;const Dispatch_scope *m_previous;
 public:
  Dispatch_scope(THD *thd,std::string operation) : m_thd(thd),m_operation(std::move(operation)),m_previous(dispatcher) {dispatcher=this;}
  ~Dispatch_scope() {assert(dispatcher==this);dispatcher=m_previous;}
  Dispatch_scope(const Dispatch_scope &)=delete;
  Dispatch_scope &operator=(const Dispatch_scope &)=delete;
  bool active(THD *thd) const {return m_thd==thd;}
  bool matches(THD *thd,const std::string &operation) const {return m_thd==thd && m_operation==operation;}
};
inline bool dispatch_active(THD *thd,const std::string &operation) {return dispatcher && dispatcher->matches(thd,operation);}
// Used only after Sql_cmd_call's current private-RPC DD guard has pinned the
// exact routine. Keep SUB_STMT_FUNCTION transaction/DDL guards; the document
// goes exclusively to the server's Protocol_local collector, never a client.
inline bool document_result_call(THD *thd,const sp_name *target) {
  if (!target || !dispatcher || !dispatcher->active(thd) ||
      !sdp_ai_invoker::worker_rpc_active(thd) || thd->get_protocol()->type()!=Protocol::PROTOCOL_LOCAL ||
      sdp_ai_principal::account(thd->security_context())!="sdp_ai_internal@localhost" ||
      target->m_db.length!=6 || native_strcasecmp(target->m_db.str,"sdp_ai")) return false;
  return !native_strcasecmp(target->m_name.str,"action_dispatch") ||
      (dispatch_active(thd,"tool") && !native_strcasecmp(target->m_name.str,"tool_action_document"));
}

inline Json_dom_ptr rpc(THD *thd,const char *operation,const Json_dom *document) {
  const std::string payload=encode(document);if (payload.empty() || payload.size()>4*1024*1024) {denied(thd);return {};}
  sdp_knowledge::Nested_read nested(thd);
  sdp_ai::Catalog_guard catalog;sdp_native::Routine_guard target;
  if (catalog.pin(thd,true) || target.pin(thd,"sdp_ai","action_dispatch","sdp_ai_internal","localhost",dd::View::ST_DEFINER)) {denied(thd);return {};}
  Security_context context;Security_context *saved=nullptr;
  const LEX_CSTRING user{STRING_WITH_LEN("sdp_ai_internal")},host{STRING_WITH_LEN("localhost")};
  if (context.change_security_context(thd,user,host,"sdp_ai",&saved,true)) return {};
  auto restore_identity=create_scope_guard([&]() {context.restore_security_context(thd,saved);});
  if (sdp_ai_principal::account(&context)!="sdp_ai_internal@localhost") {denied(thd);return {};}
  sdp_ai_invoker::Metadata_timezone_scope metadata_timezone(thd);
  Dispatch_scope dispatch(thd,operation);sdp_ai_invoker::Worker_rpc_scope native_rpc(thd);
  sp_rcontext *caller=thd->sp_runtime_ctx;thd->sp_runtime_ctx=nullptr;
  auto restore_frame=create_scope_guard([&]() {thd->sp_runtime_ctx=caller;});
  std::string sql="CALL sdp_ai.action_dispatch("+sdp_knowledge::sql_text(operation)+",CAST("+sdp_knowledge::sql_text(payload)+" AS JSON))";
  Ed_connection connection(thd,true);LEX_STRING query{sql.data(),sql.size()};
  if (connection.execute_direct(query)) {sdp_knowledge::error(thd,connection.get_last_errno(),connection.get_last_sqlstate(),connection.get_last_error());return {};}
  auto *result=connection.get_result_sets();
  if (!result || result->size()!=1 || result->get_field_count()!=1) {denied(thd);return {};}
  List<Ed_row> &rows=*result;const auto &value=(*rows.head())[0];
  if (!value.str || value.length>4*1024*1024) {denied(thd);return {};}
  return sdp_knowledge::parse_document(thd,value.str,value.length);
}
inline Json_dom_ptr target_document(const std::string &schema,const std::string &name) {
  auto result=create_dom_ptr<Json_object>();
  if (!result || result->add_alias("schema_name",create_dom_ptr<Json_string>(schema)) || result->add_alias("name",create_dom_ptr<Json_string>(name))) return {};
  return result;
}
inline bool requires_approval(THD *thd,const std::string &schema,const std::string &name,bool *result,ulonglong *version=nullptr) {
  *result=false;if (version) *version=0;
  if (!opt_sdp_ai_actions || sdp_ai_tool_policy::private_schema(schema.c_str())) return false;
  sdp_ai_identity::Routine_target identity(thd);if (identity.pin(schema,name)) return true;
  sdp_knowledge::Nested_read nested(thd);
  sdp_ai::Catalog_guard catalog;sdp_native::Routine_guard dispatcher_guard;
  if (catalog.pin(thd,true) || dispatcher_guard.pin(thd,"sdp_ai","action_dispatch","sdp_ai_internal","localhost",dd::View::ST_DEFINER)) return denied(thd);
  Security_context private_context;Security_context *saved=nullptr;
  if (private_context.change_security_context(thd,{STRING_WITH_LEN("sdp_ai_internal")},{STRING_WITH_LEN("localhost")},"sdp_ai",&saved,true)) return true;
  auto restore=create_scope_guard([&]() {private_context.restore_security_context(thd,saved);});
  if (sdp_ai_principal::account(&private_context)!="sdp_ai_internal@localhost") return denied(thd);
  // PREPARE must not leave a policy gap/record lock or consume/commit the
  // caller's business transaction. Use MySQL's real attachable readonly
  // transaction/table path: current RC read, isolated handler state, native
  // cleanup restores the caller's transaction/isolation/open-table state.
  Table_ref reference("sdp_ai","action_policies",TL_READ);
  reference.open_type=OT_BASE_ONLY;reference.required_type=dd::enum_table_type::BASE_TABLE;
  if (open_trans_system_tables_for_read(thd,&reference)) return true;
  auto close=create_scope_guard([&]() {close_trans_system_tables(thd);});
  TABLE *table=reference.table;
  if (!table || !table->file->has_transactions() || ha_legacy_type(table->file->ht)!=DB_TYPE_INNODB || table->s->fields!=6 || table->s->primary_key==MAX_KEY) return denied(thd);
  const char *fields[]={"schema_name","routine_name","version","required","changed_by","updated_at"};
  for (size_t i=0;i<6;++i) if (strcmp(table->field[i]->field_name,fields[i])) return denied(thd);
  if (table->field[0]->real_type()!=MYSQL_TYPE_VARCHAR || table->field[1]->real_type()!=MYSQL_TYPE_VARCHAR ||
      table->field[2]->real_type()!=MYSQL_TYPE_LONGLONG || !table->field[2]->is_unsigned() || table->field[3]->real_type()!=MYSQL_TYPE_TINY ||
      table->field[0]->charset()!=&my_charset_utf8mb4_bin || table->field[1]->charset()!=&my_charset_utf8mb3_general_ci) return denied(thd);
  KEY *key=&table->key_info[table->s->primary_key];
  if (key->user_defined_key_parts!=2 || key->key_part[0].field!=table->field[0] || key->key_part[1].field!=table->field[1] ||
      key->key_part[0].length!=table->field[0]->key_length() || key->key_part[1].length!=table->field[1]->key_length() || key->key_length>MAX_KEY_LENGTH) return denied(thd);
  table->field[0]->set_notnull();table->field[1]->set_notnull();
  if (table->field[0]->store(identity.schema().data(),identity.schema().size(),&my_charset_utf8mb4_bin)!=TYPE_OK ||
      table->field[1]->store(identity.name().data(),identity.name().size(),&my_charset_utf8mb4_bin)!=TYPE_OK) return denied(thd);
  uchar bytes[MAX_KEY_LENGTH];key_copy(bytes,table->record[0],key,key->key_length);
  int error=table->file->ha_index_init(table->s->primary_key,true);
  if (error) {table->file->print_error(error,MYF(0));return true;}
  auto end=create_scope_guard([&]() {table->file->ha_index_end();});
  error=table->file->ha_index_read_map(table->record[0],bytes,HA_WHOLE_KEY,HA_READ_KEY_EXACT);
  if (error==HA_ERR_KEY_NOT_FOUND || error==HA_ERR_END_OF_FILE) return false;
  if (error) {table->file->print_error(error,MYF(0));return true;}
  const longlong required_value=table->field[3]->val_int();const ulonglong revision=static_cast<ulonglong>(table->field[2]->val_int());
  if (!revision || (required_value!=0 && required_value!=1)) return denied(thd);
  *result=required_value==1;if (version) *version=revision;return false;
}
// Invoked from ordinary CALL prepare/execute, not from an Item or SQL flag.
// A native Action executes its freshly verified program directly; arbitrary
// user CALL is never allowed to borrow that execution's approval marker.
inline bool check_call(THD *thd,const sp_name *target) {
  if (!target || !opt_sdp_ai_actions) return false;
  const auto schema=target->m_db.length ? target->m_db : thd->db();
  if (!schema.str) return false;
  bool approval=false;
  if (requires_approval(thd,std::string(schema.str,schema.length),std::string(target->m_name.str,target->m_name.length),&approval)) return true;
  return approval ? required(thd) : false;
}
// Native account DDL calls this only after its managed-module DD preflight.
// It deliberately ignores actions-OFF: maintenance DROP/RENAME must not leave
// grants or a still-live approval for a later same-name authenticated account.
// Shares the ACL statement's existing transaction; never commits the caller.
inline bool lifecycle(THD *thd,const LEX_USER *from,const LEX_USER *to=nullptr) {
  if (!from || !from->user.str || !from->host.str) return denied(thd);
  const std::string actor=sdp_ai_principal::account(from->user,from->host);
  if (to && actor==sdp_ai_principal::account(to->user,to->host)) return false;
  auto request=create_dom_ptr<Json_object>();
  if (!request || request->add_alias("actor",create_dom_ptr<Json_string>(actor)) ||
      request->add_alias("role",create_dom_ptr<Json_string>(sdp_ai_principal::role_account(from->user,from->host)))) return true;
  if (to && (!to->user.str || !to->host.str || request->add_alias("new_actor",create_dom_ptr<Json_string>(sdp_ai_principal::account(to->user,to->host))) ||
      request->add_alias("new_role",create_dom_ptr<Json_string>(sdp_ai_principal::role_account(to->user,to->host))))) return denied(thd);
  return !rpc(thd,to?"rename_principal":"drop_principal",request.get());
}

inline std::string identifier(const std::string &source) {
  std::string value="`";for (char c:source) {if (c=='`') value+='`';value+=c;}return value+'`';
}
inline bool supported(dd::enum_column_types type) {
  switch (type) {
    case dd::enum_column_types::DECIMAL:case dd::enum_column_types::NEWDECIMAL:
    case dd::enum_column_types::TINY:case dd::enum_column_types::SHORT:case dd::enum_column_types::LONG:
    case dd::enum_column_types::LONGLONG:case dd::enum_column_types::INT24:case dd::enum_column_types::YEAR:
    case dd::enum_column_types::DATE:case dd::enum_column_types::NEWDATE:case dd::enum_column_types::TIME:
    case dd::enum_column_types::TIME2:case dd::enum_column_types::DATETIME:case dd::enum_column_types::DATETIME2:
    case dd::enum_column_types::VARCHAR:case dd::enum_column_types::VAR_STRING:case dd::enum_column_types::STRING:
    case dd::enum_column_types::TINY_BLOB:case dd::enum_column_types::BLOB:case dd::enum_column_types::MEDIUM_BLOB:
    case dd::enum_column_types::LONG_BLOB:case dd::enum_column_types::BIT:case dd::enum_column_types::JSON:return true;
    // FLOAT/DOUBLE formatting is not a lossless storage fingerprint. Timezone
    // dependent TIMESTAMP and spatial/vector types need dedicated encoders.
    default:return false;
  }
}

// Hashes whole dependency tables, not an inferred WHERE predicate. Its public
// receipt contains only table identity, count and digests. The internal SELECT
// is assembled exclusively from pinned DD identifiers and fixed expressions.
// FORCE INDEX(PRIMARY), full EOF and RR next-key locking include the supremum
// gap, so a concurrent INSERT cannot slip between validation and the write.
inline Json_dom_ptr resources(THD *thd,const Json_dom *descriptor,bool locking) {
  const auto *dependencies=sdp_knowledge::member(descriptor,"resources");
  if (!dependencies || dependencies->json_type()!=enum_json_type::J_ARRAY ||
      (locking && thd->tx_isolation!=ISO_REPEATABLE_READ)) {unsupported(thd);return {};}
  const auto *list=static_cast<const Json_array *>(dependencies);
  if (list->size()>32) {unsupported(thd);return {};}
  std::vector<std::pair<std::string,std::string>> targets;
  for (size_t i=0;i<list->size();++i) {
    std::string schema,name;
    if (!sdp_knowledge::string_member((*list)[i],"schema_name",&schema) || !sdp_knowledge::string_member((*list)[i],"name",&name) ||
        sdp_ai_tool_policy::private_schema(schema.c_str())) {unsupported(thd);return {};}
    targets.emplace_back(schema,name);
  }
  std::sort(targets.begin(),targets.end());auto result=create_dom_ptr<Json_array>();
  size_t budget=0;
  for (const auto &target:targets) {
    const auto &schema=target.first;const auto &name=target.second;
    const dd::cache::Dictionary_client::Auto_releaser release(thd->dd_client());
    const dd::Table *table=nullptr;
    if (thd->dd_client()->acquire<dd::Table>(schema.c_str(),name.c_str(),&table)) return {};
    if (!table || table->engine()!="InnoDB" || table->is_temporary() || table->foreign_keys().size()!=0 || table->foreign_key_parents().size()!=0) {unsupported(thd);return {};}
    const dd::Index *primary=nullptr;
    for (const dd::Index *index:table->indexes()) if (index->type()==dd::Index::IT_PRIMARY && !index->is_hidden()) primary=index;
    if (!primary) {unsupported(thd);return {};}
    std::string order,framed,total="0";
    for (const dd::Index_element *element:primary->elements()) if (!element->is_hidden()) {
      if (!order.empty()) order+=',';order+=identifier(element->column().name().c_str());
    }
    if (order.empty()) {unsupported(thd);return {};}
    size_t columns=0;
    for (const dd::Column *column:table->columns()) {
      if (column->hidden()==dd::Column::enum_hidden_type::HT_HIDDEN_SE || column->hidden()==dd::Column::enum_hidden_type::HT_HIDDEN_SQL) continue;
      if (++columns>256 || !supported(column->type())) {unsupported(thd);return {};}
      const std::string ref=identifier(column->name().c_str()),raw="CAST("+ref+" AS BINARY)";
      if (!framed.empty()) framed+=',';
      framed+="IF("+ref+" IS NULL,'N',CONCAT('V',OCTET_LENGTH("+raw+"),':',HEX("+raw+")))";
      total+="+COALESCE(OCTET_LENGTH("+raw+"),0)";
    }
    if (!columns) {unsupported(thd);return {};}
    // Protocol_local stores native integer columns as binary machine values.
    // Both collector fields must be exact ASCII bytes, also when the caller
    // chooses a multibyte character_set_results. BINARY suppresses conversion.
    std::string sql="SELECT CAST(IF("+total+">4194304,NULL,SHA2(CONCAT_WS('#',"+framed+"),256)) AS BINARY),CAST("+total+" AS BINARY)"+
      " FROM "+identifier(schema)+"."+identifier(name)+" FORCE INDEX(PRIMARY) ORDER BY "+order+" LIMIT 10001"+(locking?" FOR UPDATE":"");
    sdp_knowledge::Nested_read nested(thd);sp_rcontext *caller=thd->sp_runtime_ctx;thd->sp_runtime_ctx=nullptr;
    auto restore=create_scope_guard([&]() {thd->sp_runtime_ctx=caller;});
    Ed_connection connection(thd,true);LEX_STRING query{sql.data(),sql.size()};
    // Uses the actual owner security context; every referenced column must be
    // SELECT-authorized. No private principal reads application row values.
    if (connection.execute_direct(query)) {sdp_knowledge::error(thd,connection.get_last_errno(),connection.get_last_sqlstate(),connection.get_last_error());return {};}
    auto *rows=connection.get_result_sets();if (!rows || rows->get_field_count()!=2 || rows->size()>10000) {unsupported(thd);return {};}
    std::string row_hashes;List<Ed_row> &values=*rows;
    for (const auto &row:values) {
      const auto &hash=row[0],&size=row[1];
      if (!hash.str || hash.length!=64 || !size.str) {unsupported(thd);return {};}
      size_t bytes=0;
      for (size_t i=0;i<size.length;++i) {if (size.str[i]<'0'||size.str[i]>'9'||bytes>4194304) {unsupported(thd);return {};}bytes=bytes*10+size.str[i]-'0';}
      budget+=bytes;if (budget>4194304) {unsupported(thd);return {};}
      row_hashes.append(hash.str,hash.length);
    }
    auto entry=create_dom_ptr<Json_object>();
    if (!entry || entry->add_alias("schema_name",create_dom_ptr<Json_string>(schema)) || entry->add_alias("name",create_dom_ptr<Json_string>(name)) ||
        entry->add_alias("rows",create_dom_ptr<Json_uint>(rows->size())) || entry->add_alias("sha256",create_dom_ptr<Json_string>(sdp_ai_routine::digest(row_hashes))) || result->append_alias(std::move(entry))) return {};
  }
  return result;
}

// Changes isolation for this owned transaction only; restores even a caller's
// one-shot NEXT TRANSACTION choice after all action locks have been released.
class Transaction final {
  THD *m_thd;enum_tx_isolation m_isolation;bool m_read_only,m_started=false,m_finished=false;
 public:
  explicit Transaction(THD *thd) : m_thd(thd),m_isolation(thd->tx_isolation),m_read_only(thd->tx_read_only) {}
  bool begin(bool allow_prepare_frame=false,bool native_acl_statement=false) {
    // GRANT/REVOKE enters with native Disable_autocommit_guard after its
    // normal implicit-commit boundary. Accept only that clean DCL statement;
    // never loosen the top-level/manual Action business-transaction rule.
    const bool native_acl=native_acl_statement && m_thd->lex &&
        (m_thd->lex->sql_command==SQLCOM_GRANT || m_thd->lex->sql_command==SQLCOM_REVOKE) &&
        !m_thd->in_active_multi_stmt_transaction() &&
        m_thd->get_transaction()->is_empty(Transaction_ctx::SESSION) &&
        m_thd->get_transaction()->is_empty(Transaction_ctx::STMT);
    if (native_acl_statement && !native_acl) return denied(m_thd);
    if ((m_thd->in_multi_stmt_transaction_mode() && !native_acl) || m_thd->in_sub_stmt || (m_thd->sp_runtime_ctx && !allow_prepare_frame) || m_thd->locked_tables_mode || sdp_ai_invoker::active(m_thd)) return denied(m_thd);
    for (auto scope:{Transaction_ctx::SESSION,Transaction_ctx::STMT})
      for (const auto &engine:m_thd->get_transaction()->ha_trx_info(scope))
        if (engine.is_started() && engine.is_trx_read_write()) return denied(m_thd);
    // Native CALL's readonly policy/argument reads may have registered the
    // statement engine. Complete only that proven-readonly statement before
    // beginning an owned metadata transaction; business writes never reach it.
    if (!m_thd->get_transaction()->is_empty(Transaction_ctx::STMT) && trans_commit_stmt(m_thd)) return true;
    if (set_tx_isolation(m_thd,ISO_REPEATABLE_READ,true) || trans_begin(m_thd)) return true;
    m_started=true;return false;
  }
  bool commit() {
    if (!m_started || trans_commit_stmt(m_thd) || trans_commit(m_thd)) return true;
    m_finished=true;return false;
  }
  ~Transaction() {
    if (m_started && !m_finished) {trans_rollback_stmt(m_thd);trans_rollback(m_thd);}
    if (m_started) m_thd->mdl_context.release_transactional_locks();
    m_thd->tx_isolation=m_isolation;
    m_thd->tx_read_only=m_read_only;
  }
};
class Deadline final {
  THD *m_thd;bool m_owned=false;
 public:
  explicit Deadline(THD *thd) : m_thd(thd) {}
  bool arm() {
    if (m_thd->timer || m_thd->killed!=THD::NOT_KILLED) return denied(m_thd);
    m_thd->timer=thd_timer_set(m_thd,m_thd->timer_cache,30000);m_thd->timer_cache=nullptr;
    m_owned=m_thd->timer!=nullptr;return m_owned?false:denied(m_thd);
  }
  bool finish() {
    if (m_owned) {m_thd->timer_cache=thd_timer_reset(m_thd->timer);m_thd->timer=nullptr;m_owned=false;}
    return m_thd->killed!=THD::NOT_KILLED;
  }
  ~Deadline() {finish();}
};

class Return_binding final {
  THD *m_thd;sp_rcontext *m_frame=nullptr;Field *m_field=nullptr;std::string m_name="sdp_action_id",m_original;
  bool m_local=false,m_written=false,m_done=false,m_attempted=false,m_null=true,m_exists=false,m_json=false,m_unsigned=false;
  Item_result m_type=STRING_RESULT;const CHARSET_INFO *m_charset=&my_charset_utf8mb4_bin;Derivation m_derivation=DERIVATION_IMPLICIT;
  query_id_t m_used=0;my_decimal m_decimal;
 public:
  explicit Return_binding(THD *thd):m_thd(thd) {}
  bool init(Item *&target) {
    if (auto *local=dynamic_cast<Item_sdp_ai_local_target *>(target)) {
      const auto &descriptor=local->target();m_frame=m_thd->sp_runtime_ctx;m_local=true;
      m_field=m_frame?m_frame->sdp_variable_field(descriptor.index):nullptr;
      if (!m_field || m_frame->sdp_is_row_active(descriptor.index) || m_frame->sdp_capture_pending(descriptor.index) ||
          (m_field->real_type()!=MYSQL_TYPE_STRING && m_field->real_type()!=MYSQL_TYPE_VARCHAR && m_field->real_type()!=MYSQL_TYPE_VAR_STRING) || m_field->char_length()<32)
        return sdp_ai_capture::shape_error(m_thd);
      m_null=m_field->is_null();m_charset=m_field->charset();
      if (!m_null) {String buffer;String *value=m_field->val_str(&buffer);if (!value) return true;m_original.assign(value->ptr(),value->length());}
      return false;
    }
    if (target) {
      if ((!target->fixed && target->fix_fields(m_thd,&target)) || target->check_cols(1)) return true;
      Json_wrapper wrapper;if (target->val_json(&wrapper) || target->null_value) return sdp_ai_capture::shape_error(m_thd);
      const Json_dom *value=wrapper.to_dom();std::string kind;
      if (!sdp_knowledge::string_member(value,"kind",&kind) || kind!="user_variable" || !sdp_knowledge::string_member(value,"name",&m_name)) return sdp_ai_capture::shape_error(m_thd);
    }
    if (m_name.empty() || m_name.size()>64 || std::any_of(m_name.begin(),m_name.end(),[](unsigned char c){return c<32;})) return sdp_ai_capture::shape_error(m_thd);
    std::string lower=m_name;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});
    if (lower!="sdp_action_id" && (lower.rfind("sdp_",0)==0 || lower.rfind("__sdp",0)==0)) return sdp_ai_capture::shape_error(m_thd);
    mysql_mutex_lock(&m_thd->LOCK_thd_data);auto unlock=create_scope_guard([&](){mysql_mutex_unlock(&m_thd->LOCK_thd_data);});
    const auto found=m_thd->user_vars.find(m_name);if (found==m_thd->user_vars.end()) return false;
    auto *entry=found->second.get();if (entry->sdp_capture_pending()) return sdp_ai_capture::shape_error(m_thd);
    m_exists=true;m_null=entry->ptr()==nullptr;m_json=entry->sdp_json();m_unsigned=entry->unsigned_flag;m_type=entry->type();m_charset=entry->collation.collation;m_derivation=entry->collation.derivation;m_used=entry->used_query_id();
    if (!m_null) {if (m_type==DECIMAL_RESULT) m_decimal=*reinterpret_cast<const my_decimal *>(entry->ptr());else m_original.assign(entry->ptr(),entry->length());}
    return false;
  }
  bool assign(const std::string &id) {
    m_written=true;
    if (!m_local) return sdp_ai_capture::assign(m_thd,m_name,id,false);
    m_field->set_notnull();return m_field->store(id.data(),id.size(),&my_charset_utf8mb4_bin)==TYPE_OK?false:sdp_ai_capture::shape_error(m_thd);
  }
  void attempted() {m_attempted=true;}
  void done() {m_done=true;}
  ~Return_binding() {
    if (!m_written || m_done || m_attempted) return;
    if (m_local) {if (m_null) m_field->set_null();else {m_field->set_notnull();m_field->store(m_original.data(),m_original.size(),m_charset);}return;}
    mysql_mutex_lock(&m_thd->LOCK_thd_data);auto unlock=create_scope_guard([&](){mysql_mutex_unlock(&m_thd->LOCK_thd_data);});
    if (!m_exists) {m_thd->user_vars.erase(m_name);return;}
    auto *entry=sdp_ai_capture::entry(m_thd,m_name);if (!entry) return;
    if (m_null) {entry->set_null_value(m_type);entry->collation.set(m_charset,m_derivation);entry->unsigned_flag=m_unsigned;}
    else entry->store(m_type==DECIMAL_RESULT?static_cast<const void *>(&m_decimal):static_cast<const void *>(m_original.data()),m_type==DECIMAL_RESULT?sizeof(my_decimal):m_original.size(),m_type,m_charset,m_derivation,m_unsigned);
    if (m_json) entry->sdp_set_json();entry->set_used_query_id(m_used);
  }
};

inline bool principal_parts(const std::string &source,std::string *user,std::string *host) {
  bool escaped=false,separated=false;
  for (char c:source) {
    if (escaped) {if (c!='\\'&&c!='@') return true;(separated?*host:*user)+=c;escaped=false;}
    else if (c=='\\') escaped=true;
    else if (c=='@') {if (separated) return true;separated=true;}
    else (separated?*host:*user)+=c;
  }
  return escaped || !separated || user->size()>USERNAME_LENGTH || host->size()>HOSTNAME_LENGTH || source.find('\0')!=std::string::npos;
}
inline bool equivalent(const Json_dom *left,const Json_dom *right) {
  return left && right && encode(left)==encode(right);
}
inline bool same_routine(const Json_dom *left,const Json_dom *right) {
  for (const char *field:{"kind","schema_name","name","native_id","definition_hash","parameters","parameter_columns","resources","writes"})
    if (!equivalent(sdp_knowledge::member(left,field),sdp_knowledge::member(right,field))) return false;
  return true;
}

class Execution final {
  THD *m_thd;std::string m_id;Json_dom_ptr m_record;bool m_verified=false,m_replayed=false,m_tool=false;
  bool authority() {
    std::string actor,user,host;
    if (!sdp_knowledge::string_member(m_record.get(),"approver",&actor) || principal_parts(actor,&user,&host)) return stale(m_thd);
    Security_context context;Security_context *saved=nullptr;
    if (context.change_security_context(m_thd,{user.c_str(),user.size()},{host.c_str(),host.size()},"",&saved,true)) return true;
    auto restore=create_scope_guard([&]() {context.restore_security_context(m_thd,saved);});
    if (sdp_ai_principal::account(&context)!=actor || context.account_is_locked() || context.password_expired() ||
        sdp_ai_principal::activate(m_thd,&context,sdp_knowledge::member(m_record.get(),"approver_roles"))) return stale(m_thd);
    auto principals=sdp_ai_principal::snapshot(m_thd,&context);
    if (!principals) return true;
    auto request=create_dom_ptr<Json_object>();
    const auto *values=sdp_knowledge::member(principals.get(),"principals");
    if (!request || !values || request->add_alias("action_id",create_dom_ptr<Json_string>(m_id)) || request->add_alias("principals",values->clone())) return true;
    // The logical actor stays the action owner while this ordinary native SC
    // proves the historical approver's CURRENT grant/role activatability.
    context.restore_security_context(m_thd,saved);saved=nullptr;restore.release();
    return !rpc(m_thd,"validate_approval",request.get());
  }
 public:
  explicit Execution(THD *thd) : m_thd(thd) {}
  Execution(const Execution &)=delete;Execution &operator=(const Execution &)=delete;
  bool replayed() const {return m_replayed;}
  const Json_dom *receipt() const {return m_record.get();}
  Json_dom_ptr public_receipt() const {
    std::string state;const auto *value=sdp_knowledge::member(m_record.get(),"result");
    if (!value || !sdp_knowledge::string_member(m_record.get(),"state",&state) || state!="succeeded") {denied(m_thd);return {};}
    auto output=create_dom_ptr<Json_object>();
    if (!output || output->add_alias("action_id",create_dom_ptr<Json_string>(m_id)) ||
        output->add_alias("state",create_dom_ptr<Json_string>(state)) || output->add_alias("result",value->clone())) return {};
    return output;
  }
  const Json_dom *arguments() const {return sdp_knowledge::member(m_record.get(),"arguments");}
  const Json_dom *bindings() const {return sdp_knowledge::member(m_record.get(),"bindings");}
  const Json_dom *descriptor() const {return sdp_knowledge::member(m_record.get(),"descriptor");}
  bool consume(const std::string &id,const std::string &call_id,const Json_dom *expected_descriptor,const Json_dom *expected_arguments) {
    if (m_record || !hex_id(id,32) || m_thd->tx_isolation!=ISO_REPEATABLE_READ || !m_thd->in_active_multi_stmt_transaction()) return denied(m_thd);
    if (!call_id.empty() && (!sdp_ai_invoker::active(m_thd) || sdp_ai_invoker::current_scope->call_id()!=call_id || !sdp_ai_invoker::dispatching(m_thd,call_id))) return denied(m_thd);
    m_id=id;auto request=create_dom_ptr<Json_object>();
    if (!request || request->add_alias("action_id",create_dom_ptr<Json_string>(id))) return true;
    m_record=rpc(m_thd,"consume",request.get());if (!m_record) return true;
    std::string state;if (!sdp_knowledge::string_member(m_record.get(),"state",&state)) return denied(m_thd);
    std::string bound_call;const auto *tool=sdp_knowledge::member(m_record.get(),"tool_call_id");
    if (tool && tool->json_type()==enum_json_type::J_STRING) bound_call=static_cast<const Json_string *>(tool)->value();
    if (bound_call!=call_id || (expected_descriptor && !same_routine(descriptor(),expected_descriptor)) ||
        (expected_arguments && !equivalent(arguments(),expected_arguments))) return stale(m_thd);
    m_tool=!call_id.empty();
    if (state=="succeeded") {m_replayed=true;return false;}
    return authority();
  }
  bool validate(sdp_ai_routine::Routine &routine) {
    if (m_replayed) return false;
    auto current=routine.descriptor(m_tool);
    if (!current || !same_routine(current.get(),descriptor())) return stale(m_thd);
    auto current_resources=resources(m_thd,current.get(),true);
    if (!current_resources || !equivalent(current_resources.get(),sdp_knowledge::member(m_record.get(),"resources"))) return stale(m_thd);
    m_verified=true;return false;
  }
  bool finish(const Json_dom *result) {
    if (!m_verified || m_replayed || !result) return denied(m_thd);
    auto request=create_dom_ptr<Json_object>();
    if (!request || request->add_alias("action_id",create_dom_ptr<Json_string>(m_id)) || request->add_alias("result",result->clone())) return true;
    auto receipt=rpc(m_thd,"finish",request.get());if (!receipt) return true;m_record=std::move(receipt);m_verified=false;return false;
  }
};

inline std::string time_zone(THD *thd) {
  const String *name=thd->variables.time_zone?thd->variables.time_zone->get_name():nullptr;
  return name?std::string(name->ptr(),name->length()):std::string();
}
inline Json_dom_ptr binding_context(THD *thd) {
  auto result=create_dom_ptr<Json_object>();
  if (!result || result->add_alias("sql_mode",create_dom_ptr<Json_uint>(thd->variables.sql_mode)) ||
      result->add_alias("time_zone",create_dom_ptr<Json_string>(time_zone(thd)))) return {};
  return result;
}
inline std::string bytes_hex(const char *bytes,size_t size) {
  static const char alphabet[]="0123456789abcdef";std::string output;output.reserve(size*2);
  for (size_t i=0;i<size;++i) {const auto c=static_cast<unsigned char>(bytes[i]);output+=alphabet[c>>4];output+=alphabet[c&15];}return output;
}
// Cache each source Item exactly once before deriving both human-readable
// arguments and native source-type provenance. JSON scalar/null and SQLNULL,
// plus arbitrary binary bytes, cannot be reconstructed from JSON text alone.
inline bool bind(THD *thd,Item *source,Json_dom_ptr *value,Json_dom_ptr *binding) {
  Item_cache *cached=Item_cache::get_cache(source);
  if (!cached || cached->setup(source)) return true;
  cached->store(source);const bool success=cached->cache_value();
  if (thd->is_error() || (!success && !cached->null_value)) return true;
  if (cached->null_value) cached->store_null();
  auto proof=create_dom_ptr<Json_object>();if (!proof) return true;
  const char *kind="native";
  if (cached->null_value) {*value=create_dom_ptr<Json_null>();kind="sql_null";}
  else if (source->data_type()==MYSQL_TYPE_JSON) {*value=sdp_ai_routine::value(thd,cached);kind="json";}
  else if (source->result_type()==STRING_RESULT && source->collation.collation==&my_charset_bin) {
    String buffer;String *raw=cached->val_str(&buffer);
    if (!raw || raw->length()>2*1024*1024) return unsupported(thd);
    const std::string hex=bytes_hex(raw->ptr(),raw->length());kind="binary";
    *value=create_dom_ptr<Json_string>("0x"+hex);
    if (proof->add_alias("hex",create_dom_ptr<Json_string>(hex))) return true;
  } else *value=sdp_ai_routine::value(thd,cached);
  if (!*value || proof->add_alias("kind",create_dom_ptr<Json_string>(kind))) return true;
  *binding=std::move(proof);return false;
}
inline Item *argument(THD *thd,const Json_dom *value,const Json_dom *binding=nullptr) {
  if (!value) return nullptr;
  if (binding) {
    std::string kind;if (!sdp_knowledge::string_member(binding,"kind",&kind)) {denied(thd);return nullptr;}
    if (kind=="sql_null") return new(thd->mem_root) Item_null();
    if (kind=="json") {auto *wrapper=new(thd->mem_root) Json_wrapper(value->clone());return wrapper?new(thd->mem_root) Item_json(unique_ptr_destroy_only<Json_wrapper>(wrapper),Item_name_string()):nullptr;}
    if (kind=="binary") {
      std::string hex;if (!sdp_knowledge::string_member(binding,"hex",&hex) || hex.size()%2 || hex.size()>4*1024*1024 || !hex_id(hex,hex.size())) {denied(thd);return nullptr;}
      std::string bytes;bytes.reserve(hex.size()/2);
      const auto nibble=[](char c) {return c<='9'?c-'0':c-'a'+10;};
      for (size_t i=0;i<hex.size();i+=2) bytes+=static_cast<char>((nibble(hex[i])<<4)|nibble(hex[i+1]));
      char *copy=thd->strmake(bytes.data(),bytes.size());return copy?new(thd->mem_root) Item_string(copy,bytes.size(),&my_charset_bin):nullptr;
    }
    if (kind!="native") {denied(thd);return nullptr;}
  }
  switch (value->json_type()) {
    case enum_json_type::J_NULL:return new(thd->mem_root) Item_null();
    case enum_json_type::J_BOOLEAN:return static_cast<const Json_boolean *>(value)->value() ? static_cast<Item *>(new(thd->mem_root) Item_func_true()) : static_cast<Item *>(new(thd->mem_root) Item_func_false());
    case enum_json_type::J_INT:return new(thd->mem_root) Item_int(static_cast<const Json_int *>(value)->value());
    case enum_json_type::J_UINT:return new(thd->mem_root) Item_uint(static_cast<const Json_uint *>(value)->value());
    case enum_json_type::J_DECIMAL:{my_decimal number=*static_cast<const Json_decimal *>(value)->value();return new(thd->mem_root) Item_decimal(&number);}
    case enum_json_type::J_DOUBLE:return new(thd->mem_root) Item_float(Name_string("",0),static_cast<const Json_double *>(value)->value(),DECIMAL_NOT_SPECIFIED,DBL_DIG+8);
    case enum_json_type::J_STRING:{const auto &text=static_cast<const Json_string *>(value)->value();char *copy=thd->strmake(text.data(),text.size());return copy?new(thd->mem_root) Item_string(copy,text.size(),&my_charset_utf8mb4_bin):nullptr;}
    case enum_json_type::J_ARRAY:case enum_json_type::J_OBJECT:{auto *wrapper=new(thd->mem_root) Json_wrapper(value->clone());return wrapper?new(thd->mem_root) Item_json(unique_ptr_destroy_only<Json_wrapper>(wrapper),Item_name_string()):nullptr;}
    default:return nullptr;
  }
}
inline bool arguments(THD *thd,const sdp_ai_routine::Routine &routine,const Json_dom *document,mem_root_deque<Item *> *output,const Json_dom *bindings=nullptr) {
  if (!document || document->json_type()!=enum_json_type::J_OBJECT || static_cast<const Json_object *>(document)->cardinality()!=routine.definition().parameters.size()) return denied(thd);
  const auto *parameters=sdp_knowledge::member(bindings,"parameters");
  if (const auto *context=sdp_knowledge::member(bindings,"context")) {
    auto current=binding_context(thd);if (!current || !equivalent(context,current.get())) return stale(thd);
  }
  for (const auto &parameter:routine.definition().parameters) {
    const auto *binding=sdp_knowledge::member(parameters,parameter.name.c_str());
    if (parameters && !binding) return denied(thd);
    Item *value=argument(thd,sdp_knowledge::member(document,parameter.name.c_str()),binding);
    if (!value) return denied(thd);output->push_back(value);
  }
  return false;
}
inline bool pure(THD *thd,Item *item) {
  auto reject=[](Item *value) {return sdp_knowledge::pure_item(value,static_cast<size_t>(-1)) || value->type()==Item::SUBQUERY_ITEM;};
  return item && item->walk(&Item::walk_helper_thunk<decltype(reject)>,enum_walk::PREFIX|enum_walk::SUBQUERY,reinterpret_cast<uchar *>(&reject)) ? denied(thd) : false;
}
inline bool id_value(THD *thd,Item *&item,std::string *value) {
  if (!item || pure(thd,item) || (!item->fixed && item->fix_fields(thd,&item)) || item->check_cols(1)) return true;
  if (item->data_type()==MYSQL_TYPE_INVALID && item->propagate_type(thd,Type_properties(MYSQL_TYPE_VARCHAR))) return true;
  String buffer;String *result=item->val_str(&buffer);
  if (!result || item->null_value) return denied(thd);
  value->assign(result->ptr(),result->length());return hex_id(*value,32)?false:denied(thd);
}
inline bool send(THD *thd,Json_dom_ptr document) {
  auto *wrapper=new(thd->mem_root) Json_wrapper(std::move(document));if (!wrapper) return true;
  Item *item=new(thd->mem_root) Item_json(unique_ptr_destroy_only<Json_wrapper>(wrapper),Item_name_string(Name_string("document",8)));if (!item) return true;
  mem_root_deque<Item *> fields(thd->mem_root);fields.push_back(item);Query_result_send output;
  return output.send_result_set_metadata(thd,fields,Protocol::SEND_NUM_ROWS|Protocol::SEND_EOF) || output.send_data(thd,fields) || output.send_eof(thd);
}

inline bool grant(THD *thd,const char *kind,const char *schema,const char *name,Access_bitmask rights,
  const List<LEX_CSTRING> *dynamic,List<LEX_USER> *recipients,bool revoke,bool roles_only) {
  if (!opt_sdp_ai_actions || !thd->variables.sdp_ai_sql || !kind || !schema || !name || !recipients || !recipients->elements || !roles_only) return denied(thd);
  Acl_cache_lock_guard lock(thd,Acl_cache_lock_mode::READ_MODE);if (!lock.lock()) return true;
  sdp_ai_identity::Routine_target target(thd);std::string canonical_schema(schema),canonical_name(name);
  const std::string type(kind);std::set<std::string> privileges;
  if (dynamic) for (const auto &entry:*dynamic) {
    std::string value(entry.str,entry.length);std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c) {return std::toupper(c);});
    if (!privileges.insert(value).second) return denied(thd);
  }
  if (type=="knowledge") {if (rights!=EXECUTE_ACL || !privileges.empty()) return denied(thd);privileges.insert("EXECUTE");}
  else if (type=="conversation") {
    if (rights&~SELECT_ACL || (privileges.size()>1 || (!privileges.empty() && !privileges.count("APPEND")))) return denied(thd);
    if (rights&SELECT_ACL) privileges.insert("SELECT");if (privileges.empty()) return denied(thd);
  } else if (type=="procedure") {
    if (rights || privileges!=std::set<std::string>{"APPROVE"} || sdp_ai_tool_policy::private_schema(schema)) return denied(thd);
    sdp_knowledge::Main_identity identity(thd);if (check_routine_access(thd,ALTER_PROC_ACL,schema,name,Acl_type::PROCEDURE,false)) return true;
    if (target.pin(schema,name)) return true;canonical_schema=target.schema();canonical_name=target.name();
  } else if (type=="model" || type=="behavior" || type=="response_schema") {
    if (rights || privileges!=std::set<std::string>{"USE"}) return denied(thd);
  } else return denied(thd);
  std::vector<std::string> principals;
  // Keep role identity stable until the protected grant transaction commits.
    for (const LEX_USER &recipient:*recipients) {
      LEX_USER role=recipient;
      // MySQL roles and users share auth IDs. CREATE ROLE is a locked account;
      // is_role_id() stays false until the first membership edge is created.
      ACL_USER *native=role.user.str && role.host.str?find_acl_user(role.host.str,role.user.str,true):nullptr;
      if (!native) {my_error(ER_UNKNOWN_AUTHID,MYF(0),role.user.str?role.user.str:"",role.host.str?role.host.str:"");return true;}
      principals.push_back(sdp_ai_principal::role_account(native));
    }
  Transaction transaction(thd);if (transaction.begin(false,true)) return true;
  for (const auto &principal:principals) {
    auto request=create_dom_ptr<Json_object>();auto list=create_dom_ptr<Json_array>();
    if (!request || !list || request->add_alias("schema_name",create_dom_ptr<Json_string>(canonical_schema)) || request->add_alias("name",create_dom_ptr<Json_string>(canonical_name)) ||
        request->add_alias("principal",create_dom_ptr<Json_string>(principal))) return true;
    const char *operation;
    if (type=="procedure") operation=revoke?"revoke_approve":"grant_approve";
    else {
      for (const auto &privilege:privileges) if (list->append_alias(create_dom_ptr<Json_string>(privilege))) return true;
      if (request->add_alias("kind",create_dom_ptr<Json_string>(type)) || request->add_alias("privileges",std::move(list))) return true;
      operation=revoke?"revoke_object":"grant_object";
    }
    if (!rpc(thd,operation,request.get())) return true;
  }
  if (transaction.commit()) return true;my_ok(thd);return false;
}
} // namespace sdp_ai_action

class Sql_cmd_sdp_action final : public Sql_cmd {
  const char *m_operation;Table_ident *m_target;mem_root_deque<Item *> *m_arguments;Item *m_id;
  Item *m_return;LEX_USER *m_role;const char *m_kind,*m_privileges;
 public:
  Sql_cmd_sdp_action(const char *operation,Table_ident *target,mem_root_deque<Item *> *arguments,Item *id,Item *result,
    LEX_USER *role,const char *kind,const char *privileges) : m_operation(operation),m_target(target),m_arguments(arguments),m_id(id),m_return(result),m_role(role),m_kind(kind),m_privileges(privileges) {}
  enum_sql_command sql_command_code() const override {return SQLCOM_CALL;}
  bool are_dynamic_parameters_allowed() const override {return true;}
  bool prepare(THD *thd) override {
    using namespace sdp_ai_action;
    if (is_prepared()) return false;
    if (!opt_sdp_ai_actions || !thd->variables.sdp_ai_sql) return denied(thd);
    // Stored-program locals need their live caller frame. Their existing
    // execute-time validation stays in place; PS expressions have no values
    // yet and must resolve CAST/parameter types before binary binding.
    if (!needs_explicit_preparation()) {set_prepared();return false;}
    if (pure(thd,m_id)) return true;
    if (m_arguments) for (Item *item:*m_arguments) if (pure(thd,item)) return true;
    sdp_knowledge::Main_identity owner(thd);
    if (m_id) {
      if ((!m_id->fixed && m_id->fix_fields(thd,&m_id)) || m_id->check_cols(1)) return true;
      if (m_id->data_type()==MYSQL_TYPE_INVALID && m_id->propagate_type(thd,Type_properties(MYSQL_TYPE_VARCHAR))) return true;
    }
    if (m_arguments) for (Item *&item:*m_arguments) {
      // A bare marker has no enclosing expression type. Keep its established
      // actual-value/formal-parameter validation at execute time.
      if (item->type()==Item::PARAM_ITEM) continue;
      if ((!item->fixed && item->fix_fields(thd,&item)) || item->check_cols(1)) return true;
    }
    // RETURNING is a compiler-owned target marker; its local capacity/value
    // checks must run before metadata writes in the actual caller frame.
    set_prepared();return false;
  }
  bool execute(THD *thd) override {
    using namespace sdp_ai_action;
    if (!opt_sdp_ai_actions || !thd->variables.sdp_ai_sql) return denied(thd);
    // Validate the complete AST before fix_fields/val_* can run any argument.
    if (pure(thd,m_id)) return true;
    if (m_arguments) for (Item *item:*m_arguments) if (pure(thd,item)) return true;
    const std::string operation(m_operation);
    sdp_knowledge::Main_identity invoker_identity(thd);
    Return_binding output(thd);if ((operation=="prepare" || operation=="prepare_tool") && output.init(m_return)) return true;
    Deadline deadline(thd);if (deadline.arm()) return true;
    // Match native ACL DDL's ACL->row ordering. Keep authority pinned until
    // transaction's destructor has completed rollback/commit, not just until
    // a procedure descriptor or one approval row has been inspected.
    Acl_cache_lock_guard authority_lock(thd,Acl_cache_lock_mode::READ_MODE);
    if (!authority_lock.lock()) return true;
    Transaction transaction(thd);if (transaction.begin(operation=="prepare" || operation=="prepare_tool")) return true;
    Json_dom_ptr result;
    sdp_ai_invoker::Session_timezone_scope initiating_timezone(thd);
    std::string schema,name,id;
    if (m_target) {
      const auto db=m_target->db.str?m_target->db:thd->db();if (!db.str) {my_error(ER_NO_DB_ERROR,MYF(0));return true;}
      schema.assign(db.str,db.length);name.assign(m_target->table.str,m_target->table.length);
    }
    auto request=create_dom_ptr<Json_object>();if (!request) return true;
    if (operation=="prepare" || operation=="prepare_tool") {
      Json_dom_ptr tool,values;std::string call_id;
      auto bindings=create_dom_ptr<Json_object>();if (!bindings) return true;
      Security_context run_owner;Security_context *saved_owner=nullptr;
      auto restore_owner=create_scope_guard([&]() {run_owner.restore_security_context(thd,saved_owner);});
      std::unique_ptr<sdp_ai_invoker::Scope> protected_scope;
      if (operation=="prepare_tool") {
        if (id_value(thd,m_id,&call_id) || request->add_alias("call_id",create_dom_ptr<Json_string>(call_id))) return true;
        tool=rpc(thd,"tool",request.get());
        std::string zone;
        if (!sdp_knowledge::string_member(sdp_knowledge::member(tool.get(),"execution_context"),"time_zone",&zone) || initiating_timezone.apply(zone)) return denied(thd);
        const auto *descriptor=sdp_knowledge::member(tool.get(),"snapshot");
        if (!descriptor || !sdp_knowledge::string_member(descriptor,"schema_name",&schema) || !sdp_knowledge::string_member(descriptor,"name",&name)) return denied(thd);
        const auto &actual=thd->m_main_security_ctx;const std::string actor=sdp_ai_principal::account(&thd->m_main_security_ctx);
        if (run_owner.change_security_context(thd,actual.priv_user(),actual.priv_host(),thd->db().str?thd->db().str:"",&saved_owner,true)) return true;
        if (sdp_ai_principal::account(&run_owner)!=actor || run_owner.account_is_locked() || run_owner.password_expired() ||
            sdp_ai_principal::activate(thd,&run_owner,sdp_knowledge::member(tool.get(),"roles"))) return denied(thd);
        protected_scope=std::make_unique<sdp_ai_invoker::Scope>(thd,&run_owner,call_id);
      }
      sdp_ai_identity::Routine_target native_target(thd);if (native_target.pin(schema,name)) return true;schema=native_target.schema();name=native_target.name();
      sdp_ai_routine::Routine routine(thd);if (routine.load(schema,name,false)) return true;
      auto descriptor=routine.descriptor(operation=="prepare_tool");if (!descriptor) return true;
      if (tool) {
        if (!same_routine(descriptor.get(),sdp_knowledge::member(tool.get(),"snapshot"))) return stale(thd);
        const auto *arguments_document=sdp_knowledge::member(tool.get(),"arguments");
        if (!arguments_document) return denied(thd);values=arguments_document->clone();
        mem_root_deque<Item *> checked(thd->mem_root);if (arguments(thd,routine,values.get(),&checked) || routine.validate_arguments(checked)) return true;
      } else {
        const size_t count=m_arguments?m_arguments->size():0;
        if (count!=routine.definition().parameters.size()) return denied(thd);
        // Do not replace the persistent prepared-statement AST's Item_param
        // pointers with caches from the first execution. Cache a local deque.
        mem_root_deque<Item *> evaluated(thd->mem_root);
        if (m_arguments) for (Item *item:*m_arguments) evaluated.push_back(item);
        if (routine.validate_arguments(evaluated)) return true;
        auto object=create_dom_ptr<Json_object>();auto proofs=create_dom_ptr<Json_object>();auto context=binding_context(thd);
        if (!object || !proofs || !context) return true;
        for (size_t i=0;i<count;++i) {
          Item *&item=evaluated[i];
          if ((!item->fixed && item->fix_fields(thd,&item)) || item->check_cols(1)) return true;
          const auto *parameter=routine.program()->get_root_parsing_context()->find_variable(static_cast<uint>(i));
          if (item->data_type()==MYSQL_TYPE_INVALID && item->propagate_type(thd,Item::type_to_result(parameter->type)==STRING_RESULT?Type_properties(parameter->type,parameter->field_def.charset):Type_properties(parameter->type,parameter->field_def.is_unsigned))) return true;
          Json_dom_ptr value,proof;
          if (bind(thd,item,&value,&proof) || object->add_alias(routine.definition().parameters[i].name,std::move(value)) ||
              proofs->add_alias(routine.definition().parameters[i].name,std::move(proof))) return true;
        }
        if (bindings->add_alias("context",std::move(context)) || bindings->add_alias("parameters",std::move(proofs))) return true;
        values=std::move(object);
      }
      auto snapshot=resources(thd,descriptor.get(),false);auto principal=sdp_ai_principal::snapshot(thd);
      const auto *roles=sdp_knowledge::member(principal.get(),"roles");if (!snapshot || !roles) return true;
      request=create_dom_ptr<Json_object>();
      if (!request || request->add_alias("descriptor",std::move(descriptor)) || request->add_alias("arguments",std::move(values)) ||
          request->add_alias("bindings",std::move(bindings)) ||
          request->add_alias("resources",std::move(snapshot)) || request->add_alias("roles",roles->clone()) ||
          (!call_id.empty() && request->add_alias("tool_call_id",create_dom_ptr<Json_string>(call_id)))) return true;
      result=rpc(thd,"prepare",request.get());if (!result || !sdp_knowledge::string_member(result.get(),"action_id",&id)) return true;
      if (output.assign(id)) return true;
    } else if (operation=="execute") {
      if (id_value(thd,m_id,&id)) return true;
      Execution action(thd);if (action.consume(id,"",nullptr,nullptr)) return true;
      if (action.replayed()) result=action.public_receipt();
      else {
        if (!sdp_knowledge::string_member(action.descriptor(),"schema_name",&schema) || !sdp_knowledge::string_member(action.descriptor(),"name",&name)) return denied(thd);
        sdp_ai_invoker::Scope owner(thd,&thd->m_main_security_ctx,id);
        sdp_ai_routine::Routine routine(thd);if (routine.load(schema,name,false) || action.validate(routine)) return true;
        mem_root_deque<Item *> parameters(thd->mem_root);if (arguments(thd,routine,action.arguments(),&parameters,action.bindings())) return true;
        auto value=routine.invoke(parameters);if (!value || action.finish(value.get())) return true;
        result=action.public_receipt();
      }
    } else if (operation=="require" || operation=="grant_approve" || operation=="revoke_approve" || operation=="grant_object" || operation=="revoke_object") {
      if (request->add_alias("schema_name",create_dom_ptr<Json_string>(schema)) || request->add_alias("name",create_dom_ptr<Json_string>(name))) return true;
      if (operation=="require" || operation=="grant_approve" || operation=="revoke_approve") {
        sdp_knowledge::Main_identity identity(thd);
        if (sdp_ai_tool_policy::private_schema(schema.c_str()) || check_routine_access(thd,ALTER_PROC_ACL,schema.c_str(),name.c_str(),Acl_type::PROCEDURE,false)) return true;
        sdp_ai_identity::Routine_target native_target(thd);if (native_target.pin(schema,name)) return true;
        request=create_dom_ptr<Json_object>();
        if (!request || request->add_alias("schema_name",create_dom_ptr<Json_string>(native_target.schema())) || request->add_alias("name",create_dom_ptr<Json_string>(native_target.name()))) return true;
      }
      if (m_role) {
        Acl_cache_lock_guard role_lock(thd,Acl_cache_lock_mode::READ_MODE);if (!role_lock.lock()) return true;
        ACL_USER *native=find_acl_user(m_role->host.str,m_role->user.str,true);
        if (!native) {my_error(ER_UNKNOWN_AUTHID,MYF(0),m_role->user.str,m_role->host.str);return true;}
        if (request->add_alias("principal",create_dom_ptr<Json_string>(sdp_ai_principal::role_account(native)))) return true;
      }
      if (m_kind) {
        auto privileges=create_dom_ptr<Json_array>();if (!privileges) return true;
        const std::string source(m_privileges?m_privileges:"");size_t start=0;
        for (;;) {const auto end=source.find(',',start);if (privileges->append_alias(create_dom_ptr<Json_string>(source.substr(start,end-start)))) return true;if (end==std::string::npos) break;start=end+1;}
        if (request->add_alias("kind",create_dom_ptr<Json_string>(m_kind)) || request->add_alias("privileges",std::move(privileges))) return true;
      }
      result=rpc(thd,m_operation,request.get());
    } else {
      if (id_value(thd,m_id,&id) || request->add_alias("action_id",create_dom_ptr<Json_string>(id))) return true;
      result=rpc(thd,m_operation,request.get());
    }
    if (!result || deadline.finish()) {if (!thd->is_error() && thd->killed!=THD::NOT_KILLED) thd->send_kill_message();return true;}
    output.attempted();if (transaction.commit()) return true;output.done();
    return send(thd,std::move(result));
  }
};

class PT_sdp_action final : public Parse_tree_root {
  const char *m_operation;Table_ident *m_target;PT_item_list *m_arguments;Item *m_id;
  Item *m_return;LEX_USER *m_role;const char *m_kind,*m_privileges;
 public:
  PT_sdp_action(const POS &pos,const char *operation,Table_ident *target=nullptr,PT_item_list *arguments=nullptr,Item *id=nullptr,
    Item *result=nullptr,LEX_USER *role=nullptr,const char *kind=nullptr,const char *privileges=nullptr)
    : Parse_tree_root(pos),m_operation(operation),m_target(target),m_arguments(arguments),m_id(id),m_return(result),m_role(role),m_kind(kind),m_privileges(privileges) {}
  Sql_cmd *make_cmd(THD *thd) override {
    thd->lex->sql_command=SQLCOM_CALL;Parse_context context(thd,thd->lex->query_block);
    if (thd->lex->sphead) {
      thd->lex->sphead->sdp_language_version=1;
      // This command emits a document without a nested Sql_cmd_call result
      // frame. Mark its SP so the caller frames the trailing OK correctly.
      thd->lex->sphead->m_flags|=sp_head::MULTI_RESULTS;
    }
    if ((m_arguments && m_arguments->contextualize(&context)) || (m_id && m_id->itemize(&context,&m_id)) || (m_return && m_return->itemize(&context,&m_return))) return nullptr;
    return new(thd->mem_root) Sql_cmd_sdp_action(m_operation,m_target,m_arguments?&m_arguments->value:nullptr,m_id,m_return,m_role,m_kind,m_privileges);
  }
};
class Item_func_sdp_action_dispatch_active final : public Item_int_func {
 public:
  Item_func_sdp_action_dispatch_active(const POS &pos,Item *operation) : Item_int_func(pos,operation) {}
  const char *func_name() const override {return "stardataparadise_action_dispatch_active";}
  table_map get_initial_pseudo_tables() const override {return RAND_TABLE_BIT;}
  bool resolve_type(THD *thd) override {if (param_type_is_default(thd,0,1,MYSQL_TYPE_VARCHAR)) return true;set_data_type_bool();return false;}
  longlong val_int() override {assert(fixed);String buffer;String *value=args[0]->val_str(&buffer);return value && sdp_ai_action::dispatch_active(current_thd,std::string(value->ptr(),value->length()));}
  bool do_itemize(Parse_context *pc,Item **res) override {
    if (skip_itemize(res)) return false;if (Item_int_func::do_itemize(pc,res)) return true;
    pc->thd->lex->safe_to_cache_query=false;pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);return false;
  }
};
#endif
