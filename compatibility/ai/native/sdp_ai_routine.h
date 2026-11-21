// DD-pinned typed INVOKER routine validation for explicitly registered tools.
#ifndef SQL_SDP_AI_ROUTINE_H
#define SQL_SDP_AI_ROUTINE_H
#include "sql/sdp_ai_knowledge.h"
#include "sql/sdp_ai_tool_policy.h"
#include "sql/auth/auth_common.h"
#include "sql/dd/types/function.h"
#include "sql/dd/types/procedure.h"
#include "sql/dd/types/parameter.h"
#include "sql/dd/types/schema.h"
#include "sql/dd/types/abstract_table.h"
#include "sql/dd/types/table.h"
#include "sql/dd/types/trigger.h"
#include "sql/dd/impl/sdi.h"
#include "sql/mysqld.h"
#include "sha2.h"
#include "sql/trigger.h"

namespace sdp_ai_routine {
inline bool denied(THD *thd) {
  return sdp_knowledge::error(thd,61081,"42000","510001: Routine is outside the authorized typed INVOKER tool contract.");
}
inline std::string digest(const std::string &value) {
  unsigned char bytes[32];SHA_EVP256(reinterpret_cast<const unsigned char *>(value.data()),value.size(),bytes);
  static const char hex[]="0123456789abcdef";std::string result;
  for (unsigned char c:bytes) {result+=hex[c>>4];result+=hex[c&15];}return result;
}

inline Json_dom_ptr value(THD *thd,Item *item) {
  String buffer,conversion;Json_wrapper result;
  if (item->data_type()==MYSQL_TYPE_NEWDECIMAL || item->data_type()==MYSQL_TYPE_DECIMAL) {
    String *number=item->val_str(&buffer);
    return number ? Json_dom_ptr(create_dom_ptr<Json_string>(std::string(number->ptr(),number->length()))) : Json_dom_ptr(create_dom_ptr<Json_null>());
  }
  if (get_atom_null_as_null(&item,0,"AI typed routine result",&buffer,&conversion,&result)) return {};
  return result.clone_dom();
}
class Result final : public Query_result_send {
  Json_array *m_sets;
  Json_array *m_rows=nullptr;
  size_t m_bytes=0,m_count=0;
 public:
  explicit Result(Json_array *sets) : m_sets(sets) {}
  bool use_protocol_adapter() const override {return false;}
  bool send_result_set_metadata(THD *,const mem_root_deque<Item *> &fields,uint) override {
    if (m_sets->size()>=16 || fields.size()>512) return true;
    auto set=create_dom_ptr<Json_object>();auto columns=create_dom_ptr<Json_array>();auto rows=create_dom_ptr<Json_array>();
    if (!set || !columns || !rows) return true;
    m_rows=rows.get();
    for (Item *field:fields) {
      Send_field metadata;field->make_field(&metadata);auto column=create_dom_ptr<Json_object>();
      if (!column || column->add_alias("name",create_dom_ptr<Json_string>(metadata.col_name?metadata.col_name:"")) ||
          column->add_alias("type_code",create_dom_ptr<Json_int>(metadata.type)) ||
          column->add_alias("unsigned",create_dom_ptr<Json_boolean>((metadata.flags&UNSIGNED_FLAG)!=0)) ||
          column->add_alias("nullable",create_dom_ptr<Json_boolean>((metadata.flags&NOT_NULL_FLAG)==0)) ||
          column->add_alias("decimals",create_dom_ptr<Json_uint>(metadata.decimals)) ||
          column->add_alias("charset",create_dom_ptr<Json_uint>(metadata.charsetnr)) || columns->append_alias(std::move(column))) return true;
    }
    return set->add_alias("columns",std::move(columns)) || set->add_alias("rows",std::move(rows)) || m_sets->append_alias(std::move(set));
  }
  bool send_data(THD *thd,const mem_root_deque<Item *> &fields) override {
    if (!m_rows || ++m_count>10000) return denied(thd);
    auto row=create_dom_ptr<Json_array>();if (!row) return true;
    for (Item *item:fields) {
      auto cell=value(thd,item);if (!cell) return true;
      String encoded;Json_wrapper wrapper(cell->clone());
      if (wrapper.to_string(&encoded,true,"AI routine result",JsonDepthErrorHandler)) return true;
      m_bytes+=encoded.length();if (m_bytes>1048576) return denied(thd);
      if (row->append_alias(std::move(cell))) return true;
    }
    return m_rows->append_alias(std::move(row));
  }
  bool send_eof(THD *) override {return false;}
};

class Routine final {
  THD *m_thd;
  MDL_ticket *m_ticket=nullptr;
  sdp_knowledge::Program m_program;
  sdp_knowledge::Definition m_definition;
  std::string m_schema,m_name,m_hash,m_id;
  std::vector<MDL_ticket *> m_table_tickets;
  Json_array m_resources;
  std::vector<std::string> m_resource_keys;
  bool m_function=false,m_writes=false;
  size_t m_trigger_count=0;
  bool m_cascades=false;
 public:
  explicit Routine(THD *thd) : m_thd(thd) {}
  ~Routine() {for (auto *ticket:m_table_tickets) m_thd->mdl_context.release_lock(ticket);if (m_ticket) m_thd->mdl_context.release_lock(m_ticket);}
  Routine(const Routine &)=delete;
  Routine &operator=(const Routine &)=delete;
  sp_head *program() const {return m_program.get();}
  const sdp_knowledge::Definition &definition() const {return m_definition;}
  bool writes() const {return m_writes;}
  bool is_function() const {return m_function;}
  const std::string &hash() const {return m_hash;}
  const std::string &schema() const {return m_schema;}
  const std::string &name() const {return m_name;}
  // Validate native parameter conversion before any application instruction.
  // Do not rely on a warning left after execute_procedure: SP instructions
  // clear their diagnostics. Each source Item is cached under the caller's
  // frame once, then an independent native parameter table checks range,
  // scale, charset and NULL semantics without executing the body.
  bool validate_arguments(mem_root_deque<Item *> &arguments) {
    if (!m_program || arguments.size()!=m_definition.parameters.size()) return sdp_knowledge::shape_error(m_thd);
    const sql_mode_t saved_mode=m_thd->variables.sql_mode;
    m_thd->variables.sql_mode|=MODE_STRICT_ALL_TABLES;
    auto restore_mode=create_scope_guard([&]() {m_thd->variables.sql_mode=saved_mode;});
    mem_root_deque<Item *> cached(m_thd->mem_root);
    for (size_t index=0;index<arguments.size();++index) {
      Item *&argument=arguments[index];
      if (sdp_ai_tool_policy::expression(m_thd,argument,true) || (!argument->fixed && argument->fix_fields(m_thd,&argument)) || argument->check_cols(1)) return true;
      const auto *parameter=m_program->get_root_parsing_context()->find_variable(static_cast<uint>(index));
      if (!parameter) return sdp_knowledge::shape_error(m_thd);
      if (argument->data_type()==MYSQL_TYPE_INVALID && argument->propagate_type(m_thd,
          Item::type_to_result(parameter->type)==STRING_RESULT ? Type_properties(parameter->type,parameter->field_def.charset) : Type_properties(parameter->type,parameter->field_def.is_unsigned))) return true;
      Item_cache *value=Item_cache::get_cache(argument);
      if (!value || value->setup(argument)) return true;
      value->store(argument);const bool loaded=value->cache_value();
      if (m_thd->is_error() || (!loaded && !value->null_value)) return true;
      if (value->null_value) value->store_null();
      cached.push_back(value);
    }
    sp_rcontext *parameters=sp_rcontext::create(m_thd,m_program->get_root_parsing_context(),nullptr);
    if (!parameters) return true;
    parameters->sp=m_program.get();
    auto destroy=create_scope_guard([&]() {::destroy_at(parameters);});
    for (uint index=0;index<cached.size();++index) {
      const ulong warnings=m_thd->get_stmt_da()->current_statement_cond_count();
      if (parameters->set_variable(m_thd,false,index,&cached[index])) return true;
      if (m_thd->get_stmt_da()->current_statement_cond_count()!=warnings) return sdp_knowledge::shape_error(m_thd);
    }
    for (size_t index=0;index<cached.size();++index) arguments[index]=cached[index];
    return false;
  }
  bool trigger(const dd::Trigger &stored,const std::string &schema,const std::string &table_name) {
    if (++m_trigger_count>128) return denied(m_thd);
    const CHARSET_INFO *client=get_charset(static_cast<uint>(stored.client_collation_id()),MYF(0));
    const CHARSET_INFO *connection=get_charset(static_cast<uint>(stored.connection_collation_id()),MYF(0));
    const CHARSET_INFO *database=get_charset(static_cast<uint>(stored.schema_collation_id()),MYF(0));
    if (!client || !connection || !database) return denied(m_thd);
    const auto text=[](const auto &value) {return LEX_CSTRING{value.data(),value.size()};};
    MEM_ROOT memory(PSI_NOT_INSTRUMENTED,8192);
    Trigger *compiled=Trigger::create_from_dd(&memory,text(stored.name()),text(schema),text(table_name),
      text(stored.action_statement()),text(stored.action_statement_utf8()),stored.sql_mode(),text(stored.definer_user()),text(stored.definer_host()),
      {client->csname,strlen(client->csname)},{connection->m_coll_name,strlen(connection->m_coll_name)},
      {database->m_coll_name,strlen(database->m_coll_name)},
      static_cast<enum_trigger_event_type>(static_cast<int>(stored.event_type())-1),
      static_cast<enum_trigger_action_time_type>(static_cast<int>(stored.action_timing())-1),stored.action_order(),stored.created());
    if (!compiled) return true;
    auto destroy=create_scope_guard([&]() {::destroy_at(compiled);});
    if (compiled->parse(m_thd,false) || compiled->has_parse_error() || !compiled->get_sp()) return denied(m_thd);
    sp_head *program=compiled->get_sp();
    if (program->instructions()>4096) return denied(m_thd);
    for (uint i=0;i<program->instructions();++i) {
      auto *instruction=program->get_instr(i);
      if (sdp_ai_tool_policy::instruction(m_thd,instruction,true,true)) return true;
      if (auto *lexical=dynamic_cast<sp_lex_instr *>(instruction)) {
        const LEX *lex=lexical->get_lex();
        for (Table_ref *dependency=lex->query_tables;dependency;dependency=dependency->next_global)
          if (resource(dependency,true)) return true;
      }
    }
    return false;
  }
  bool resource(Table_ref *table,bool from_trigger=false) {
    if (table->is_derived() || table->is_recursive_reference()) return false;
    if (from_trigger && sdp_ai_tool_policy::metadata_table(table)) return false;
    if (table->is_table_function() || !table->db || !table->table_name ||
        sdp_ai_tool_policy::private_schema(table->db) || find_temporary_table(m_thd,table->db,table->table_name)) return denied(m_thd);
    const std::string key=std::string(table->db)+'\0'+table->table_name;
    if (std::find(m_resource_keys.begin(),m_resource_keys.end(),key)!=m_resource_keys.end()) return false;
    if (m_resource_keys.size()>=32) return denied(m_thd);
    m_resource_keys.push_back(key);
    MDL_request request;MDL_REQUEST_INIT(&request,MDL_key::TABLE,table->db,table->table_name,MDL_SHARED,MDL_EXPLICIT);
    if (m_thd->mdl_context.acquire_lock(&request,m_thd->variables.lock_wait_timeout)) return true;
    m_table_tickets.push_back(request.ticket);
    const dd::cache::Dictionary_client::Auto_releaser release(m_thd->dd_client());
    const dd::Abstract_table *abstract=nullptr;
    if (m_thd->dd_client()->acquire<dd::Abstract_table>(table->db,table->table_name,&abstract)) return true;
    const auto *definition=dynamic_cast<const dd::Table *>(abstract);
    if (!definition || definition->engine()!="InnoDB" || definition->is_temporary()) return denied(m_thd);
    if (definition->foreign_keys().size() || definition->foreign_key_parents().size()) m_cascades=true;
    const dd::String_type schema(table->db);
    const auto sdi=dd::serialize(m_thd,*definition,schema);
    if (sdi.empty() || sdi.size()>4*1024*1024) return denied(m_thd);
    std::string signature(sdi.data(),sdi.size());
    const auto append=[&](const std::string &value) {signature+=std::to_string(value.size())+":"+value;};
    // Triggers are deliberately omitted by native SDI; include their full
    // executable identity so approval cannot survive a trigger replacement.
    for (const dd::Trigger *trigger:definition->triggers()) {
      append(std::to_string(trigger->id()));append(trigger->name().c_str());append(std::to_string(static_cast<int>(trigger->event_type())));
      append(std::to_string(static_cast<int>(trigger->action_timing())));append(std::to_string(trigger->action_order()));
      append(trigger->action_statement().c_str());append(trigger->definer_user().c_str());append(trigger->definer_host().c_str());
      append(std::to_string(trigger->sql_mode()));append(std::to_string(trigger->client_collation_id()));
      append(std::to_string(trigger->connection_collation_id()));append(std::to_string(trigger->schema_collation_id()));
    }
    auto item=create_dom_ptr<Json_object>();
    if (!item || item->add_alias("schema_name",create_dom_ptr<Json_string>(table->db)) ||
        item->add_alias("name",create_dom_ptr<Json_string>(table->table_name)) ||
        item->add_alias("object_id",create_dom_ptr<Json_string>(std::to_string(definition->id()))) ||
        item->add_alias("definition_hash",create_dom_ptr<Json_string>(digest(signature))) || m_resources.append_alias(std::move(item))) return true;
    // Prelocking tables in branches/expressions can be absent from the plain
    // statement list. Compile the real native trigger IR before any binding,
    // include its transitive application table closure, and reject UDF/dynamic
    // SQL/transaction escapes. Metadata-only Decision predicates are vetted
    // separately and execute later under the logical owner's ordinary ACL.
    if (table->updating) for (const dd::Trigger *stored:definition->triggers())
      if (trigger(*stored,table->db,table->table_name)) return true;
    return false;
  }
  Json_dom_ptr invoke(mem_root_deque<Item *> &arguments) {
    if (!sdp_ai_invoker::active(m_thd) || !m_program || arguments.size()!=m_definition.parameters.size()) {denied(m_thd);return {};}
    sdp_knowledge::Main_identity identity(m_thd);
    const sql_mode_t caller_mode=m_thd->variables.sql_mode;
    m_thd->variables.sql_mode|=MODE_STRICT_ALL_TABLES;
    auto restore_mode=create_scope_guard([&]() {m_thd->variables.sql_mode=caller_mode;});
    const ulong warnings=m_thd->get_stmt_da()->current_statement_cond_count();
    if (validate_arguments(arguments)) return {};
    if (m_function) {
      auto *table=new(m_thd->mem_root) TABLE;auto *share=new(m_thd->mem_root) TABLE_SHARE;
      if (!table || !share) return {};
      table->s=share;table->in_use=m_thd;table->alias="";table->copy_blobs=true;table->set_nullable();
      share->table_cache_key=EMPTY_CSTR;share->db=EMPTY_CSTR;share->table_name=EMPTY_CSTR;
      Field *field=m_program->create_result_field(m_thd,MAX_BLOB_WIDTH,"value",table);
      if (!field) return {};
      std::vector<Item *> values(arguments.begin(),arguments.end());
      Sub_statement_state state;m_thd->reset_sub_statement_state(&state,SUB_STMT_FUNCTION);
      sdp_knowledge::Execution_lex caller(m_thd);
      const bool failed=caller.failed() || m_program->execute_function(m_thd,values.data(),static_cast<uint>(values.size()),field);
      m_thd->restore_sub_statement_state(&state);if (failed) return {};
      if (m_thd->get_stmt_da()->current_statement_cond_count()!=warnings) {sdp_knowledge::shape_error(m_thd);return {};}
      Item *item=new(m_thd->mem_root) Item_field(field);if (!item) return {};
      auto scalar=value(m_thd,item);if (!scalar) return {};
      String encoded;Json_wrapper wrapper(scalar->clone());
      if (wrapper.to_string(&encoded,true,"AI function result",JsonDepthErrorHandler)) return {};
      if (encoded.length()>1048576) {denied(m_thd);return {};}
      auto output=create_dom_ptr<Json_object>();auto columns=create_dom_ptr<Json_array>();auto column=create_dom_ptr<Json_object>();
      if (!output || !columns || !column || column->add_alias("name",create_dom_ptr<Json_string>("value")) ||
          column->add_alias("type",create_dom_ptr<Json_string>(m_definition.results[0].type)) ||
          column->add_alias("nullable",create_dom_ptr<Json_boolean>(true)) || columns->append_alias(std::move(column)) ||
          output->add_alias("columns",std::move(columns)) || output->add_alias("value",std::move(scalar))) return {};
      return output;
    }
    auto sets=create_dom_ptr<Json_array>();if (!sets) return {};
    Result result(sets.get());std::vector<std::pair<LEX *,Query_result *>> interceptors;
    for (uint index=0;index<m_program->instructions();++index) {
      if (auto *statement=dynamic_cast<sp_instr_stmt *>(m_program->get_instr(index))) {
        LEX *lex=const_cast<LEX *>(statement->get_lex());
        if (lex->sql_command==SQLCOM_SELECT) {interceptors.emplace_back(lex,lex->result);lex->result=&result;}
      }
    }
    auto restore=create_scope_guard([&]() {for (auto &entry:interceptors) entry.first->result=entry.second;});
    sdp_knowledge::Execution_lex caller(m_thd);
    if (caller.failed() || m_program->execute_procedure(m_thd,&arguments)) return {};
    if (m_thd->get_stmt_da()->current_statement_cond_count()!=warnings) {sdp_knowledge::shape_error(m_thd);return {};}
    auto output=create_dom_ptr<Json_object>();
    if (!output || output->add_alias("result_sets",std::move(sets))) return {};
    return output;
  }

  bool load(const std::string &schema,const std::string &name,bool function) {
    sdp_knowledge::Main_identity identity(m_thd);
    if (m_ticket || !sdp_knowledge::valid_text(schema,256) || !sdp_knowledge::valid_text(name,256)) return denied(m_thd);
    m_function=function;
    // Private product/system routines are never model tools, even when the
    // initiating account happens to be a trusted administrator or worker.
    for (const char *reserved:{"mysql","sys","information_schema","performance_schema","sdp_ai","sdp_jobs","sdp_hist","sdp_hist_remote","sdp_hist_data"})
      if (native_strcasecmp(schema.c_str(),reserved)==0) return denied(m_thd);
    std::string lookup_schema=schema;
    if (lower_case_table_names) {
      my_casedn_str(system_charset_info,lookup_schema.data());
      lookup_schema.resize(strlen(lookup_schema.c_str()));
    }
    if (check_routine_access(m_thd,EXECUTE_ACL,lookup_schema.c_str(),name.c_str(),function?Acl_type::FUNCTION:Acl_type::PROCEDURE,false)) return true;
    MDL_key key;
    dd::Routine::create_mdl_key(function?dd::Routine::RT_FUNCTION:dd::Routine::RT_PROCEDURE,
        lookup_schema.c_str(),name.c_str(),&key);
    MDL_request request;
    MDL_REQUEST_INIT_BY_KEY(&request,&key,MDL_SHARED,MDL_EXPLICIT);
    if (m_thd->mdl_context.acquire_lock(&request,m_thd->variables.lock_wait_timeout)) return true;
    m_ticket=request.ticket;
    const dd::cache::Dictionary_client::Auto_releaser release(m_thd->dd_client());
    const dd::Routine *routine=nullptr;
    if (function) {
      const dd::Function *value=nullptr;if (m_thd->dd_client()->acquire<dd::Function>(lookup_schema.c_str(),name.c_str(),&value)) return true;routine=value;
    } else {
      const dd::Procedure *value=nullptr;if (m_thd->dd_client()->acquire<dd::Procedure>(lookup_schema.c_str(),name.c_str(),&value)) return true;routine=value;
    }
    if (!routine || routine->security_type()!=dd::View::ST_INVOKER) return denied(m_thd);
    const dd::Schema *stored_schema=nullptr;
    if (m_thd->dd_client()->acquire<dd::Schema>(lookup_schema.c_str(),&stored_schema)) return true;
    if (!stored_schema || stored_schema->id()!=routine->schema_id()) return denied(m_thd);
    // Privilege, policy and protected snapshot keys all name the same DD
    // object even when SQL used another case/accent-equivalent spelling.
    m_schema.assign(stored_schema->name().data(),stored_schema->name().size());
    if (lower_case_table_names) {
      my_casedn_str(system_charset_info,m_schema.data());
      m_schema.resize(strlen(m_schema.c_str()));
    }
    m_name.assign(routine->name().data(),routine->name().size());
    size_t count=0;bool skip_return=function;
    for (const dd::Parameter *parameter:routine->parameters()) {
      if (skip_return) {skip_return=false;continue;}
      if (parameter->is_name_null() || parameter->is_parameter_mode_null() || parameter->mode()!=dd::Parameter::PM_IN) return denied(m_thd);
      ++count;
    }
    if (count>256) return denied(m_thd);
    LEX_CSTRING db{m_schema.c_str(),m_schema.size()};LEX_STRING local_name{m_name.data(),m_name.size()};
    sp_name target(db,local_name,true);target.init_qname(m_thd);
    sp_head *original=nullptr;
    const auto type=function?enum_sp_type::FUNCTION:enum_sp_type::PROCEDURE;
    if (sp_cache_routine(m_thd,type,&target,false,&original)!=SP_OK || !original) return true;
    if (original->m_chistics->suid!=SP_IS_NOT_SUID || !original->is_sql()) return denied(m_thd);
    // Field::sql_type() formats into the caller's allocated String buffer;
    // an empty default String can acquire a positive snprintf length while
    // retaining a NULL pointer. Match the native routine cloning path.
    String return_type(64);return_type.set_charset(system_charset_info);
    if (function) {
      if (!return_type.ptr()) return true;
      original->returns_type(m_thd,&return_type);
      if (m_thd->is_error() || !return_type.ptr() || !return_type.length() ||
          return_type.length()>1048576) return denied(m_thd);
    }
    const std::string parameters(original->m_params.str?original->m_params.str:"",original->m_params.length);
    const std::string body(original->m_body.str,original->m_body.length);
    const std::string returns=function?std::string(return_type.ptr(),return_type.length()):std::string();
    Stored_program_creation_ctx *creation=original->get_creation_ctx();
    if (!creation) return denied(m_thd);
    st_sp_chistics characteristics=*original->m_chistics;
    sp_head *raw=nullptr;
    if (db_load_routine(m_thd,type,m_schema.c_str(),m_schema.size(),m_name.c_str(),m_name.size(),&raw,
        original->m_sql_mode,parameters.c_str(),returns.c_str(),body.c_str(),&characteristics,
        routine->definer_user().c_str(),routine->definer_host().c_str(),0,0,creation)!=SP_OK || !raw) return true;
    m_program.reset(raw);m_definition.schema=m_schema;m_definition.body=body;
    for (uint index=0;index<count;++index) {
      auto *variable=raw->get_root_parsing_context()->find_variable(index);
      if (!variable || variable->mode!=sp_variable::MODE_IN) return denied(m_thd);
      const std::string normalized=sdp_knowledge::canonical_type(variable->field_def);
      if (normalized.empty()) return denied(m_thd);
      m_definition.parameters.push_back({std::string(variable->name.str,variable->name.length),normalized,true});
    }
    if (function) {
      const std::string normalized=sdp_knowledge::canonical_type(raw->m_return_field_def);
      if (normalized.empty()) return denied(m_thd);
      m_definition.results.push_back({"value",normalized,true});
    }
    if (raw->instructions()>4096) return denied(m_thd);
    for (uint index=0;index<raw->instructions();++index) {
      auto *instruction=raw->get_instr(index);
      if (sdp_ai_tool_policy::instruction(m_thd,instruction,true)) return true;
      if (auto *lexical=dynamic_cast<sp_lex_instr *>(instruction)) {
        const LEX *lex=lexical->get_lex();
        if (dynamic_cast<sp_instr_stmt *>(instruction) && lex->sql_command!=SQLCOM_SELECT && lex->sql_command!=SQLCOM_DO && lex->sql_command!=SQLCOM_SIGNAL) m_writes=true;
        for (Table_ref *table=lex->query_tables;table;table=table->next_global) if (resource(table)) return true;
      }
    }
    // Function tools are readonly expressions; writes require the procedure
    // action/approval path, whose receipt shares the data transaction.
    if ((function && m_writes) || (m_writes && m_cascades)) return denied(m_thd);
    m_id=std::to_string(routine->id());
    std::string snapshot;
    const auto append=[&](const std::string &value) {snapshot+=std::to_string(value.size())+":"+value;};
    append(function?"function":"procedure");append(m_id);append(m_schema);append(m_name);append(std::to_string(original->m_sql_mode));
    append(std::to_string(routine->client_collation_id()));append(std::to_string(routine->connection_collation_id()));
    append(std::to_string(routine->schema_collation_id()));append(routine->definer_user().c_str());append(routine->definer_host().c_str());
    append(parameters);append(returns);append(body);
    String resources;Json_wrapper resource_doc(m_resources.clone());
    if (resource_doc.to_string(&resources,true,"AI routine resources",JsonDepthErrorHandler)) return true;
    append(std::string(resources.ptr(),resources.length()));m_hash=digest(snapshot);
    return false;
  }
  Json_dom_ptr descriptor(bool require_tool_schema=true) const {
    auto result=create_dom_ptr<Json_object>();auto columns=create_dom_ptr<Json_array>();
    auto parameters=sdp_knowledge::tool_parameters(m_program.get(),m_definition);
    if (!result || !columns || (!parameters && require_tool_schema)) return {};
    // Native manual actions can bind lossless SQL values such as BLOBs that
    // have no supported provider JSON argument schema. Keep that distinction
    // explicit; model-tool registration continues to require the schema.
    if (!parameters) parameters=create_dom_ptr<Json_null>();
    if (!parameters) return {};
    for (const auto &parameter:m_definition.parameters) {
      auto column=create_dom_ptr<Json_object>();
      if (!column || column->add_alias("name",create_dom_ptr<Json_string>(parameter.name)) ||
          column->add_alias("type",create_dom_ptr<Json_string>(parameter.type)) || column->add_alias("nullable",create_dom_ptr<Json_boolean>(true)) || columns->append_alias(std::move(column))) return {};
    }
    if (result->add_alias("kind",create_dom_ptr<Json_string>(m_function?"function":"procedure")) ||
        result->add_alias("schema_name",create_dom_ptr<Json_string>(m_schema)) || result->add_alias("name",create_dom_ptr<Json_string>(m_name)) ||
        result->add_alias("native_id",create_dom_ptr<Json_string>(m_id)) || result->add_alias("definition_hash",create_dom_ptr<Json_string>(m_hash)) ||
        result->add_alias("parameters",std::move(parameters)) || result->add_alias("parameter_columns",std::move(columns)) ||
        result->add_alias("resources",m_resources.clone()) ||
        result->add_alias("writes",create_dom_ptr<Json_boolean>(m_writes))) return {};
    return result;
  }
};
} // namespace sdp_ai_routine
#endif
