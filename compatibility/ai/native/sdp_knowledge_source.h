// Native typed Knowledge table source. No stored routine or SQL-text rewrite.
#ifndef SQL_SDP_KNOWLEDGE_SOURCE_H
#define SQL_SDP_KNOWLEDGE_SOURCE_H
#include "sql/sdp_ai_knowledge.h"
#include "sql/table_function.h"
#include "sql/mysqld.h"

namespace sdp_knowledge {
// A nested read must share the caller transaction and restore its open-table,
// stored-program and lexical state. SUB_STMT_FUNCTION prevents an internal
// metadata SELECT from committing the outer application statement.
class Nested_read final {
  THD *m_thd;
  LEX *m_lex;
  Query_tables_list m_query_tables;
  Open_tables_backup m_tables;
  Sub_statement_state m_substatement;
 public:
  explicit Nested_read(THD *thd) : m_thd(thd),m_lex(thd->lex) {
    m_lex->reset_n_backup_query_tables_list(&m_query_tables);
    thd->reset_n_backup_open_tables_state(&m_tables,0);
    thd->reset_sub_statement_state(&m_substatement,SUB_STMT_FUNCTION);
  }
  ~Nested_read() {
    close_thread_tables(m_thd);
    m_thd->restore_sub_statement_state(&m_substatement);
    // Retain the nested read's transactional MDL until the actual caller
    // transaction/statement ends; never release an outer query's tickets.
    m_tables.mdl_system_tables_svp=m_thd->mdl_context.mdl_savepoint();
    m_thd->restore_backup_open_tables_state(&m_tables);
    m_lex->restore_backup_query_tables_list(&m_query_tables);
  }
};

inline Json_dom_ptr source_document(THD *thd,const std::string &schema,const std::string &name) {
  Main_identity identity(thd);
  Nested_read nested(thd);
  sdp_ai::Catalog_guard catalog;
  if (catalog.pin(thd)) {error(thd,61081,"42000","510001: Native AI catalog routine identity is not trusted.");return {};}
  const std::string query="SELECT sdp_ai.knowledge_value("+sql_text(schema)+","+sql_text(name)+")";
  sp_rcontext *caller=thd->sp_runtime_ctx;thd->sp_runtime_ctx=nullptr;
  auto restore=create_scope_guard([&]() {thd->sp_runtime_ctx=caller;});
  sdp_ai_invoker::Worker_rpc_scope native_rpc(thd);
  Ed_connection connection(thd,true);
  LEX_STRING statement{const_cast<char *>(query.data()),query.size()};
  if (connection.execute_direct(statement)) {
    error(thd,connection.get_last_errno(),connection.get_last_sqlstate(),connection.get_last_error());return {};
  }
  auto *result=connection.get_result_sets();
  if (!result || result->size()!=1 || result->get_field_count()!=1) {shape_error(thd);return {};}
  List<Ed_row> &rows=*result;
  const auto &value=(*rows.head())[0];
  if (!value.str || value.length>4*1024*1024) {shape_error(thd);return {};}
  return parse_document(thd,value.str,value.length);
}
inline std::string source_signature(const Json_dom *value) {
  String encoded;Json_wrapper wrapper(value->clone());
  if (wrapper.to_string(&encoded,true,"Knowledge source",JsonDepthErrorHandler)) return {};
  return std::string(encoded.ptr(),encoded.length());
}
} // namespace sdp_knowledge

class Table_function_sdp_knowledge final : public Table_function {
  THD *m_thd;
  std::string m_schema,m_name,m_signature;
  mem_root_deque<Item *> m_arguments;
  sdp_knowledge::Definition m_definition;
  sdp_knowledge::Program m_shape_program;
  List<Create_field> m_fields;
 public:
  Table_function_sdp_knowledge(THD *thd,const LEX_CSTRING &schema,const LEX_CSTRING &name,
      mem_root_deque<Item *> *arguments)
      : m_thd(thd),m_schema(schema.str,schema.length),m_name(name.str,name.length),m_arguments(thd->mem_root) {
    if (arguments) for (Item *argument:*arguments) m_arguments.push_back(argument);
  }
  const char *func_name() const override {return "knowledge";}
  bool init() override {
    auto document=sdp_knowledge::source_document(m_thd,m_schema,m_name);
    if (!document || sdp_knowledge::read_definition(m_thd,document.get(),&m_definition)) return true;
    if (m_definition.parameters.size()!=m_arguments.size()) return sdp_knowledge::shape_error(m_thd);
    m_signature=sdp_knowledge::source_signature(document.get());
    m_shape_program=sdp_knowledge::compile(m_thd,m_definition);
    if (!m_shape_program || m_signature.empty()) return true;
    for (size_t index=0;index<m_definition.results.size();++index) {
      const auto &column=m_definition.results[index];
      auto *variable=m_shape_program->get_root_parsing_context()->find_variable(static_cast<uint>(m_definition.parameters.size()+index));
      if (!variable || sdp_knowledge::canonical_type(variable->field_def).empty()) return sdp_knowledge::shape_error(m_thd);
      auto *field=new(m_thd->mem_root) Create_field(variable->field_def);
      if (!field) return true;
      field->field_name=m_thd->strmake(column.name.data(),column.name.size());
      if (!field->field_name) return true;
      field->is_nullable=column.nullable;
      if (column.nullable) field->flags&=~NOT_NULL_FLAG;else field->flags|=NOT_NULL_FLAG;
      if (m_fields.push_back(field)) return true;
    }
    return false;
  }
  bool fill_result_table() override {
    empty_table();
    // Prepared handles cannot silently reuse stale shape, catalog authority or
    // a changed body under the same SQL name.
    auto document=sdp_knowledge::source_document(m_thd,m_schema,m_name);
    if (!document) return true;
    if (sdp_knowledge::source_signature(document.get())!=m_signature)
      return sdp_knowledge::error(m_thd,61014,"40001","503004: Knowledge source version changed; prepare the statement again.");
    auto rows=create_dom_ptr<Json_array>();
    {
      sdp_knowledge::Nested_read nested(m_thd);
      if (sdp_knowledge::execute(m_thd,m_definition,m_arguments,rows.get())) return true;
    }
    for (size_t index=0;index<rows->size();++index) {
      const auto *row=(*rows)[index];
      for (size_t column=0;column<m_definition.results.size();++column) {
        Field *field=get_field(static_cast<uint>(column));
        const auto *value=sdp_knowledge::member(row,m_definition.results[column].name.c_str());
        if (!value) return sdp_knowledge::shape_error(m_thd);
        if (value->json_type()==enum_json_type::J_NULL) {
          if (!m_definition.results[column].nullable) return sdp_knowledge::shape_error(m_thd);
          field->set_null();
        } else {
          field->set_notnull();
          Json_wrapper wrapper(value->clone());
          if (save_json_to_field(m_thd,field,&wrapper,false)) return true;
        }
      }
      if (write_row()) return true;
    }
    return false;
  }
  table_map used_tables() const override {
    table_map used=0;for (Item *argument:m_arguments) used|=argument->used_tables();return used;
  }
  bool walk(Item_processor processor,enum_walk walk,uchar *arg) override {
    for (Item *argument:m_arguments) if (argument->walk(processor,walk,arg)) return true;return false;
  }
  bool print(const THD *thd,String *output,enum_query_type query_type) const override {
    if (output->append(STRING_WITH_LEN("KNOWLEDGE ")) || output->append(sdp_knowledge::identifier(m_schema).c_str()) ||
        output->append('.') || output->append(sdp_knowledge::identifier(m_name).c_str()) || output->append('(')) return true;
    bool first=true;
    for (Item *argument:m_arguments) {
      if (!first && output->append(',')) return true;
      first=false;argument->print(thd,output,query_type);
    }
    return output->append(')');
  }
 private:
  List<Create_field> *get_field_list() override {return &m_fields;}
  bool do_init_args() override {
    for (size_t index=0;index<m_arguments.size();++index) {
      Item *&argument=m_arguments[index];
      auto reject=[](Item *item) {return sdp_knowledge::pure_item(item,static_cast<size_t>(-1)) || item->type()==Item::SUBQUERY_ITEM || item->type()==Item::SUM_FUNC_ITEM;};
      if (argument->walk(&Item::walk_helper_thunk<decltype(reject)>,enum_walk::PREFIX|enum_walk::SUBQUERY,reinterpret_cast<uchar *>(&reject)))
        return sdp_knowledge::readonly_error(m_thd);
      if ((!argument->fixed && argument->fix_fields(m_thd,&argument)) || argument->check_cols(1)) return true;
      auto *variable=m_shape_program->get_root_parsing_context()->find_variable(static_cast<uint>(index));
      const auto &field=variable->field_def;
      if (argument->data_type()==MYSQL_TYPE_INVALID && argument->propagate_type(m_thd,
          Item::type_to_result(variable->type)==STRING_RESULT ? Type_properties(variable->type,field.charset) : Type_properties(variable->type,field.is_unsigned))) return true;
    }
    return false;
  }
  void do_cleanup() override {inited=false;for (Item *argument:m_arguments) argument->cleanup();}
  void do_fix_after_pullout(Query_block *parent,Query_block *removed) override {
    for (Item *argument:m_arguments) argument->fix_after_pullout(parent,removed);
  }
};

inline bool sdp_knowledge_materialized_source(const Table_ref *table) {
  return table && table->is_table_function() && dynamic_cast<const Table_function_sdp_knowledge *>(table->table_function)!=nullptr;
}

class Sql_cmd_sdp_append_knowledge final : public Sql_cmd {
  Table_ident *m_target;
  mem_root_deque<Item *> *m_arguments;
  std::string m_alias;
 public:
  Sql_cmd_sdp_append_knowledge(Table_ident *target,mem_root_deque<Item *> *arguments,const LEX_STRING &alias)
      : m_target(target),m_arguments(arguments),m_alias(alias.str,alias.length) {}
  enum_sql_command sql_command_code() const override {return SQLCOM_CALL;}
  bool are_dynamic_parameters_allowed() const override {return true;}
  bool execute(THD *thd) override {
    const auto schema=m_target->db.str ? m_target->db : thd->db();
    if (!schema.str) {my_error(ER_NO_DB_ERROR,MYF(0));return true;}
    auto document=sdp_knowledge::source_document(thd,std::string(schema.str,schema.length),std::string(m_target->table.str,m_target->table.length));
    sdp_knowledge::Definition definition;
    if (!document || sdp_knowledge::read_definition(thd,document.get(),&definition)) return true;
    auto rows=create_dom_ptr<Json_array>();auto columns=create_dom_ptr<Json_array>();
    if (!rows || !columns) return true;
    mem_root_deque<Item *> empty(thd->mem_root);
    {
      sdp_knowledge::Nested_read nested(thd);
      if (sdp_knowledge::execute(thd,definition,m_arguments ? *m_arguments : empty,rows.get())) return true;
    }
    for (const auto &column:definition.results) {
      auto value=create_dom_ptr<Json_object>();
      if (!value || value->add_alias("name",create_dom_ptr<Json_string>(column.name)) ||
          value->add_alias("type",create_dom_ptr<Json_string>(column.type)) ||
          value->add_alias("nullable",create_dom_ptr<Json_boolean>(column.nullable)) || columns->append_alias(std::move(value))) return true;
    }
    auto value=create_dom_ptr<Json_object>();auto payload=create_dom_ptr<Json_object>();
    if (!value || !payload || value->add_alias("columns",std::move(columns)) || value->add_alias("rows",std::move(rows)) ||
        payload->add_alias("name",create_dom_ptr<Json_string>(m_alias)) || payload->add_alias("value",std::move(value))) return true;
    const std::string encoded=sdp_knowledge::source_signature(payload.get());
    // Only native-serialized data becomes a hexadecimal SQL value. No model or
    // user query text is dispatched as executable SQL here.
    std::string query="CALL sdp_ai.dispatch('append_context','conversation','','',CAST("+
        sdp_knowledge::sql_text(encoded)+" AS JSON),@sdp_ai_output)";
    sdp_ai::Catalog_guard catalog;
    if (catalog.pin(thd)) return sdp_knowledge::error(thd,61081,"42000","510001: Native AI catalog routine identity is not trusted.");
    sp_rcontext *caller=thd->sp_runtime_ctx;thd->sp_runtime_ctx=nullptr;
    auto restore=create_scope_guard([&]() {thd->sp_runtime_ctx=caller;});
    sdp_ai_invoker::Worker_rpc_scope native_rpc(thd);
    Ed_connection connection(thd,true);LEX_STRING statement{query.data(),query.size()};
    if (connection.execute_direct(statement)) return sdp_knowledge::error(thd,connection.get_last_errno(),connection.get_last_sqlstate(),connection.get_last_error());
    my_ok(thd,0);return false;
  }
};

class PT_sdp_append_knowledge final : public Parse_tree_root {
  Table_ident *m_target;PT_item_list *m_arguments;LEX_STRING m_alias;
 public:
  PT_sdp_append_knowledge(const POS &pos,Table_ident *target,PT_item_list *arguments,const LEX_STRING &alias)
      : Parse_tree_root(pos),m_target(target),m_arguments(arguments),m_alias(alias) {}
  Sql_cmd *make_cmd(THD *thd) override {
    if (!global_system_variables.sdp_ai_sql) {sdp_knowledge::readonly_error(thd);return nullptr;}
    thd->lex->sql_command=SQLCOM_CALL;
    Parse_context context(thd,thd->lex->query_block);
    if (m_arguments && m_arguments->contextualize(&context)) return nullptr;
    if (m_arguments) for (Item *argument:m_arguments->value) {
      auto reject=[](Item *item) {return sdp_knowledge::pure_item(item,static_cast<size_t>(-1)) || item->type()==Item::SUBQUERY_ITEM;};
      if (argument->walk(&Item::walk_helper_thunk<decltype(reject)>,enum_walk::PREFIX|enum_walk::SUBQUERY,reinterpret_cast<uchar *>(&reject))) {
        sdp_knowledge::readonly_error(thd);return nullptr;
      }
    }
    return new(thd->mem_root) Sql_cmd_sdp_append_knowledge(m_target,m_arguments ? &m_arguments->value : nullptr,m_alias);
  }
};

class PT_sdp_knowledge_source final : public PT_table_reference {
  Table_ident *m_target;
  PT_item_list *m_arguments;
  LEX_CSTRING m_alias;
 public:
  PT_sdp_knowledge_source(const POS &pos,Table_ident *target,PT_item_list *arguments,LEX_CSTRING alias)
      : PT_table_reference(pos),m_target(target),m_arguments(arguments),m_alias(alias) {}
  bool do_contextualize(Parse_context *pc) override {
    if (!global_system_variables.sdp_ai_sql) return sdp_knowledge::readonly_error(pc->thd);
    if (PT_table_reference::do_contextualize(pc) || (m_arguments && m_arguments->contextualize(pc))) return true;
    const auto schema=m_target->db.str ? m_target->db : pc->thd->db();
    if (!schema.str) {my_error(ER_NO_DB_ERROR,MYF(0));return true;}
    if (!m_alias.str) {my_error(ER_TF_MUST_HAVE_ALIAS,MYF(0));return true;}
    auto *function=new(pc->mem_root) Table_function_sdp_knowledge(pc->thd,schema,m_target->table,m_arguments ? &m_arguments->value : nullptr);
    LEX_CSTRING name{STRING_WITH_LEN("knowledge")};
    // add_table_to_list normalizes this buffer in place on Windows. Match
    // native JSON_TABLE's writable statement-lifetime allocation.
    name.str=pc->thd->strmake(name.str,name.length);
    if (!name.str) return true;
    auto *target=new(pc->mem_root) Table_ident(name,function);
    if (!function || !target) return true;
    m_table_ref=pc->select->add_table_to_list(pc->thd,target,m_alias.str,0,TL_READ,MDL_SHARED_READ);
    return !m_table_ref || pc->select->add_joined_table(m_table_ref);
  }
};
#endif
