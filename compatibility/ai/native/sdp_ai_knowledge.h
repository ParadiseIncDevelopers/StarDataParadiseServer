// Native readonly Knowledge compiler. No persistent routine or SQL rewrite.
#ifndef SQL_SDP_AI_KNOWLEDGE_H
#define SQL_SDP_AI_KNOWLEDGE_H

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include "scope_guard.h"
#include "sql/auth/sql_authorization.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/types/abstract_table.h"
#include "sql/item.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"
#include "sql/mdl.h"
#include "sql/query_result.h"
#include "sql/sp.h"
#include "sql/sp_head.h"
#include "sql/sp_instr.h"
#include "sql/sp_pcontext.h"
#include "sql/sp_rcontext.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/mysqld.h"
#include "sql/sql_cmd_dml.h"
#include "sql/sql_call.h"
#include "sql/sdp_ai_local_ast.h"
#include "sql/sql_error.h"
#include "sql/sql_lex.h"
#include "sql/item_strfunc.h"
#include "sql/item_json_func.h"
#include "sql/parse_tree_nodes.h"
#include "sql/statement/ed_connection.h"
#include "sql-common/json_dom.h"
#include "sql-common/my_decimal.h"
#include "sdp_ai_invoker.h"
#include "sdp_ai_catalog_guard.h"

namespace sdp_knowledge {

inline bool error(THD *thd, uint number, const char *state, const char *message) {
  if (!thd->is_error()) {
    auto *area = thd->get_stmt_da();
    area->set_error_status(number, message, state);
    area->push_warning(thd, number, state, Sql_condition::SL_ERROR, message);
  }
  return true;
}
inline bool shape_error(THD *thd) {
  return error(thd, 61042, "22000", "506002: Knowledge parameter or result shape is invalid.");
}
inline bool readonly_error(THD *thd) {
  return error(thd, 61006, "0A000", "502006: Knowledge requires a supported readonly SELECT.");
}

struct Column {
  std::string name;
  std::string type;
  bool nullable = false;
};
struct Dependency { std::string schema, name, id; };
struct Definition {
  std::string schema;
  std::string body;
  std::vector<Column> parameters;
  std::vector<Column> results;
  std::vector<Dependency> dependencies;
};

inline const Json_dom *member(const Json_dom *value, const char *key) {
  return value && value->json_type() == enum_json_type::J_OBJECT
      ? static_cast<const Json_object *>(value)->get(key) : nullptr;
}
inline bool string_member(const Json_dom *value, const char *key, std::string *output) {
  const auto *child = member(value, key);
  if (!child || child->json_type() != enum_json_type::J_STRING) return false;
  *output = static_cast<const Json_string *>(child)->value();
  return true;
}
inline bool read_columns(const Json_dom *document, const char *key, std::vector<Column> *output) {
  const auto *value = member(document, key);
  if (!value || value->json_type() != enum_json_type::J_ARRAY) return true;
  const auto *columns = static_cast<const Json_array *>(value);
  for (size_t index = 0; index < columns->size(); ++index) {
    const auto *column = (*columns)[index];
    Column definition;
    if (!string_member(column,"name",&definition.name) || !string_member(column,"type",&definition.type)) return true;
    if (auto *nullable = member(column,"nullable")) {
      if (nullable->json_type() != enum_json_type::J_BOOLEAN) return true;
      definition.nullable = static_cast<const Json_boolean *>(nullable)->value();
    }
    for (const auto &existing:*output)
      if (my_strcasecmp(system_charset_info,existing.name.c_str(),definition.name.c_str())==0) return true;
    output->push_back(std::move(definition));
  }
  return false;
}
inline bool read_definition(THD *thd, const Json_dom *document, Definition *output) {
  if (!string_member(document,"schema_name",&output->schema) ||
      !string_member(document,"definition",&output->body) ||
      read_columns(document,"parameters",&output->parameters) || read_columns(document,"result_columns",&output->results))
    return shape_error(thd);
  if (const auto *dependencies=member(document,"dependencies")) {
    if (dependencies->json_type()!=enum_json_type::J_ARRAY) return shape_error(thd);
    const auto *items=static_cast<const Json_array *>(dependencies);
    for (size_t index=0;index<items->size();++index) {
      std::string kind;Dependency dependency;
      if (!string_member((*items)[index],"kind",&kind)) return shape_error(thd);
      if (kind!="Table") continue;
      if (!string_member((*items)[index],"schema_name",&dependency.schema) || !string_member((*items)[index],"name",&dependency.name) ||
          !string_member((*items)[index],"object_id",&dependency.id)) return shape_error(thd);
      output->dependencies.push_back(std::move(dependency));
    }
  }
  return false;
}

inline Json_dom_ptr parse_document(THD *thd, const char *source, size_t length) {
  auto result = Json_dom::parse(source,length,
      [&](const char *,size_t) { shape_error(thd); }, [&]() { shape_error(thd); });
  if (!result) shape_error(thd);
  return result;
}

class Main_identity final {
  THD *m_thd;
  Security_context *m_saved;
 public:
  explicit Main_identity(THD *thd) : m_thd(thd), m_saved(thd->security_context()) {
    thd->set_security_context(sdp_ai_invoker::logical_context(thd));
  }
  ~Main_identity() { m_thd->set_security_context(m_saved); }
};

class Creation_context final : public Stored_program_creation_ctx {
  Creation_context(const CHARSET_INFO *client, const CHARSET_INFO *connection,
                   const CHARSET_INFO *database)
      : Stored_program_creation_ctx(client, connection, database) {}
 protected:
  Object_creation_ctx *create_backup_ctx(THD *thd) const override {
    return new Creation_context(thd);
  }
  void delete_backup_ctx() override { delete this; }
 public:
  explicit Creation_context(THD *thd) : Stored_program_creation_ctx(thd) {}
  Stored_program_creation_ctx *clone(MEM_ROOT *root) override {
    return new (root) Creation_context(get_client_cs(), get_connection_cl(), get_db_cl());
  }
};

inline std::string identifier(const std::string &value) {
  std::string result = "`";
  for (char c : value) { result += c; if (c == '`') result += '`'; }
  return result + '`';
}
inline bool valid_text(const std::string &value, size_t maximum) {
  return !value.empty() && value.size() <= maximum && value.find('\0') == std::string::npos;
}
inline std::string result_parameter(size_t index) {
  return "__sdp_result_" + std::to_string(index);
}
struct Destroy_program { void operator()(sp_head *program) const { sp_head::destroy(program); } };
using Program = std::unique_ptr<sp_head, Destroy_program>;

inline Program compile(THD *thd, const Definition &definition) {
  if (!valid_text(definition.schema, 256) || !valid_text(definition.body, 1048576) ||
      definition.parameters.size() > 256 || definition.results.empty() || definition.results.size() > 512) {
    shape_error(thd); return {};
  }
  std::string parameters;
  std::vector<std::string> names;
  auto append = [&](const std::string &name, const std::string &type) {
    if (!valid_text(name, 256) || !valid_text(type, 512)) return false;
    if (!parameters.empty()) parameters += ',';
    parameters += "IN " + identifier(name) + ' ' + type;
    names.push_back(name); return true;
  };
  for (const auto &column : definition.parameters) {
    if (column.name.compare(0, 13, "__sdp_result_") == 0 || !append(column.name, column.type)) {
      shape_error(thd); return {};
    }
  }
  for (size_t index = 0; index < definition.results.size(); ++index) {
    if (!valid_text(definition.results[index].name, 256) ||
        !append(result_parameter(index), definition.results[index].type)) {
      shape_error(thd); return {};
    }
  }
  const std::string body = "BEGIN\n" + definition.body + ";\nEND";
  st_sp_chistics characteristics;
  characteristics.suid = SP_IS_NOT_SUID;
  characteristics.daccess = SP_READS_SQL_DATA;
  characteristics.language = {STRING_WITH_LEN("SQL")};
  Creation_context context(thd);
  sp_head *raw = nullptr;
  const auto &actor = sdp_ai_invoker::actor_context(thd);
  const auto user = actor.priv_user(), host = actor.priv_host();
  const auto code = db_load_routine(thd, enum_sp_type::PROCEDURE,
      definition.schema.data(), definition.schema.size(), "__sdp_knowledge", 15, &raw,
      thd->variables.sql_mode | MODE_STRICT_ALL_TABLES, parameters.c_str(), "", body.c_str(),
      &characteristics, user.str ? user.str : "", host.str ? host.str : "", 0, 0, &context);
  Program program(raw);
  if (code != SP_OK || !program) return {};
  // Metadata callers are untrusted. A type string may not escape its typed
  // declaration or replace the INVOKER body, even though nothing is persisted.
  if (program->m_type != enum_sp_type::PROCEDURE || program->m_chistics->suid != SP_IS_NOT_SUID ||
      std::string(program->m_body.str, program->m_body.length) != body ||
      program->get_root_parsing_context()->context_var_count() != names.size()) {
    shape_error(thd); return {};
  }
  for (uint i = 0; i < names.size(); ++i) {
    auto *variable = program->get_root_parsing_context()->find_variable(i);
    if (!variable || variable->mode != sp_variable::MODE_IN ||
        std::string(variable->name.str, variable->name.length) != names[i]) {
      shape_error(thd); return {};
    }
  }
  return program;
}

class Table_locks final {
  THD *m_thd;
  std::vector<MDL_ticket *> m_tickets;
  std::vector<Dependency> m_dependencies;
  const std::vector<Dependency> *m_expected;
 public:
  explicit Table_locks(THD *thd,const std::vector<Dependency> *expected=nullptr) : m_thd(thd),m_expected(expected) {}
  ~Table_locks() { for (auto *ticket : m_tickets) m_thd->mdl_context.release_lock(ticket); }
  const std::vector<Dependency> &dependencies() const { return m_dependencies; }
  bool base_table(Table_ref *table) {
    if (table->is_derived() || table->is_recursive_reference()) return false;
    if (table->is_table_function() || table->updating || !table->db || !table->table_name ||
        find_temporary_table(m_thd, table->db, table->table_name)) return readonly_error(m_thd);
    if (check_table_access(m_thd, SELECT_ACL, table, false, 1, false)) return true;
    MDL_request request;
    MDL_REQUEST_INIT(&request, MDL_key::TABLE, table->db, table->table_name,
                     MDL_SHARED_READ, MDL_EXPLICIT);
    // MDL_EXPLICIT always owns a cloned ticket, including when the caller
    // already holds a transactional lock. Only our tickets are released.
    if (m_thd->mdl_context.acquire_lock(&request, m_thd->variables.lock_wait_timeout)) return true;
    m_tickets.push_back(request.ticket);
    dd::cache::Dictionary_client::Auto_releaser release(m_thd->dd_client());
    const dd::Abstract_table *definition = nullptr;
    if (m_thd->dd_client()->acquire(table->db, table->table_name, &definition)) return true;
    if (!definition || definition->type() != dd::enum_table_type::BASE_TABLE) return readonly_error(m_thd);
    Dependency dependency{table->db,table->table_name,std::to_string(definition->id())};
    if (m_expected && std::none_of(m_expected->begin(),m_expected->end(),[&](const Dependency &item) {
          return item.schema==dependency.schema && item.name==dependency.name && item.id==dependency.id;
        })) return error(m_thd,61041,"42000","506001: Knowledge table dependency changed.");
    if (std::none_of(m_dependencies.begin(),m_dependencies.end(),[&](const Dependency &item) {
          return item.schema==dependency.schema && item.name==dependency.name;
        })) m_dependencies.push_back(std::move(dependency));
    return false;
  }
};

inline std::string canonical_type(const Create_field &field) {
  std::string result;
  switch (field.sql_type) {
    case MYSQL_TYPE_TINY: result="TINYINT"; break;
    case MYSQL_TYPE_SHORT: result="SMALLINT"; break;
    case MYSQL_TYPE_INT24: result="MEDIUMINT"; break;
    case MYSQL_TYPE_LONG: result="INT"; break;
    case MYSQL_TYPE_LONGLONG: result="BIGINT"; break;
    case MYSQL_TYPE_FLOAT: result="FLOAT"; break;
    case MYSQL_TYPE_DOUBLE: result="DOUBLE"; break;
    case MYSQL_TYPE_NEWDECIMAL: case MYSQL_TYPE_DECIMAL:
      result="DECIMAL("+std::to_string(my_decimal_length_to_precision(
        static_cast<uint>(field.max_display_width_in_codepoints()),field.decimals,field.is_unsigned))+","+std::to_string(field.decimals)+")"; break;
    case MYSQL_TYPE_DATE: result="DATE"; break;
    case MYSQL_TYPE_YEAR: result="YEAR"; break;
    case MYSQL_TYPE_TIME: case MYSQL_TYPE_TIME2: result="TIME("+std::to_string(field.decimals)+")"; break;
    case MYSQL_TYPE_DATETIME: case MYSQL_TYPE_DATETIME2: result="DATETIME("+std::to_string(field.decimals)+")"; break;
    case MYSQL_TYPE_TIMESTAMP: case MYSQL_TYPE_TIMESTAMP2: result="TIMESTAMP("+std::to_string(field.decimals)+")"; break;
    case MYSQL_TYPE_JSON: result="JSON"; break;
    case MYSQL_TYPE_VARCHAR: case MYSQL_TYPE_VAR_STRING:
      result=(field.charset==&my_charset_bin ? "VARBINARY(" : "VARCHAR(")+std::to_string(field.max_display_width_in_codepoints())+")"; break;
    case MYSQL_TYPE_STRING:
      result=(field.charset==&my_charset_bin ? "BINARY(" : "CHAR(")+std::to_string(field.max_display_width_in_codepoints())+")"; break;
    case MYSQL_TYPE_TINY_BLOB: result=field.charset==&my_charset_bin ? "TINYBLOB" : "TINYTEXT"; break;
    case MYSQL_TYPE_MEDIUM_BLOB: result=field.charset==&my_charset_bin ? "MEDIUMBLOB" : "MEDIUMTEXT"; break;
    case MYSQL_TYPE_LONG_BLOB: result=field.charset==&my_charset_bin ? "LONGBLOB" : "LONGTEXT"; break;
    case MYSQL_TYPE_BLOB: result=field.charset==&my_charset_bin ? "BLOB" : "TEXT"; break;
    default: return {};
  }
  if (field.is_unsigned && (field.sql_type==MYSQL_TYPE_TINY || field.sql_type==MYSQL_TYPE_SHORT ||
      field.sql_type==MYSQL_TYPE_INT24 || field.sql_type==MYSQL_TYPE_LONG || field.sql_type==MYSQL_TYPE_LONGLONG ||
      field.sql_type==MYSQL_TYPE_FLOAT || field.sql_type==MYSQL_TYPE_DOUBLE || field.sql_type==MYSQL_TYPE_NEWDECIMAL || field.sql_type==MYSQL_TYPE_DECIMAL))
    result += " UNSIGNED";
  if (field.is_zerofill) result += " ZEROFILL";
  if (field.charset && field.charset!=&my_charset_bin &&
      (field.sql_type==MYSQL_TYPE_VARCHAR || field.sql_type==MYSQL_TYPE_VAR_STRING || field.sql_type==MYSQL_TYPE_STRING ||
       field.sql_type==MYSQL_TYPE_BLOB || field.sql_type==MYSQL_TYPE_TINY_BLOB || field.sql_type==MYSQL_TYPE_MEDIUM_BLOB || field.sql_type==MYSQL_TYPE_LONG_BLOB))
    result += " CHARACTER SET "+identifier(field.charset->csname)+" COLLATE "+identifier(field.charset->m_coll_name);
  return result;
}

inline bool normalize_columns(THD *thd, const Definition &definition, sp_head *program, bool results, Json_array *output) {
  const auto &columns = results ? definition.results : definition.parameters;
  const size_t start = results ? definition.parameters.size() : 0;
  for (size_t index = 0; index < columns.size(); ++index) {
    const auto *variable = program->get_root_parsing_context()->find_variable(static_cast<uint>(start+index));
    const auto type = canonical_type(variable->field_def);
    if (type.empty()) return readonly_error(thd);
    auto value = create_dom_ptr<Json_object>();
    if (!value || value->add_alias("name",create_dom_ptr<Json_string>(columns[index].name)) ||
        value->add_alias("type",create_dom_ptr<Json_string>(type)) ||
        value->add_alias("nullable",create_dom_ptr<Json_boolean>(columns[index].nullable)) || output->append_alias(std::move(value))) return true;
  }
  return false;
}

inline bool pure_item(Item *item, size_t parameter_count) {
  if (auto *local = dynamic_cast<Item_splocal *>(item)) return local->get_var_idx() >= parameter_count;
  if (item->type() == Item::SUM_FUNC_ITEM)
    return static_cast<Item_sum *>(item)->sum_func() == Item_sum::UDF_SUM_FUNC;
  if (item->type() != Item::FUNC_ITEM) return false;
  const auto *function = static_cast<Item_func *>(item);
  const auto type = function->functype();
  if (type == Item_func::FUNC_SP || type == Item_func::UDF_FUNC ||
      type == Item_func::SUSERVAR_FUNC || type == Item_func::DD_INTERNAL_FUNC) return true;
  if (type != Item_func::UNKNOWN_FUNC) return false;
  std::string name = function->func_name();
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
  // This is an AST builtin purity registry, not a filter on SQL source words.
  const char *safe[] = {"lower","upper","concat","concat_ws","coalesce","ifnull","nullif","if",
    "length","char_length","octet_length","substring","substr","left","right","trim","ltrim","rtrim",
    "replace","reverse","round","truncate","ceil","ceiling","floor","mod","pow","power","sqrt",
    "abs","sign","hex","unhex","md5","sha","sha1","sha2","ascii","char","date_format","str_to_date",
    "year","month","day","dayofmonth","hour","minute","second","timestampdiff","date_add","date_sub",
    "sdp_ready_value","convert","cast_as_json","json_extract","json_unquote","json_object","json_array","json_length","json_type","json_valid",
    "json_merge_patch","json_merge_preserve","json_set","json_remove",
    "database","current_user","user","stardataparadise_actor","stardataparadise_capabilities"};
  return std::none_of(std::begin(safe), std::end(safe), [&](const char *value) { return name == value; });
}

inline LEX *readonly_lex(THD *thd, sp_head *program, size_t parameter_count, Table_locks *locks) {
  if (program->instructions() != 1) { readonly_error(thd); return nullptr; }
  auto *instruction = dynamic_cast<sp_instr_stmt *>(program->get_instr(0));
  if (!instruction) { readonly_error(thd); return nullptr; }
  auto *lex = const_cast<LEX *>(instruction->get_lex());
  if (!lex || lex->sql_command != SQLCOM_SELECT ||
      (lex->result != nullptr && dynamic_cast<Query_result_send *>(lex->result) == nullptr)) {
    readonly_error(thd); return nullptr;
  }
  for (Query_block *block = lex->all_query_blocks_list; block; block = block->next_select_in_list()) {
    auto reject = [parameter_count](Item *item) { return pure_item(item, parameter_count); };
    if (block->walk(&Item::walk_helper_thunk<decltype(reject)>, enum_walk::PREFIX | enum_walk::SUBQUERY,
                    reinterpret_cast<uchar *>(&reject))) { readonly_error(thd); return nullptr; }
  }
  for (auto *table = lex->query_tables; table; table = table->next_global) {
    if (table->lock_descriptor().type != TL_READ && table->lock_descriptor().type != TL_READ_DEFAULT) {
      readonly_error(thd); return nullptr;
    }
    if (locks->base_table(table)) return nullptr;
  }
  return lex;
}

class Result_column final : public Item_ref {
  bool m_nullable;
 public:
  Result_column(Item **value, bool nullable)
      : Item_ref(nullptr,value,"","",""),m_nullable(nullable) {}
  void make_field(Send_field *field) override {
    Item_ref::make_field(field);
    if (m_nullable) field->flags&=~NOT_NULL_FLAG;
    else field->flags|=NOT_NULL_FLAG;
  }
};

// Native SP argument setup cleans its caller LEX and rolls back Item changes.
// A table source can invoke it while the outer SELECT owns a live join/temp
// handler. Give SP setup an independent CALL context and keep the outer Item
// tree changes intact until that SELECT's normal cleanup.
class Execution_lex final {
  THD *m_thd;
  LEX *m_saved;
  LEX m_call;
  Item_change_list m_changes;
  bool m_failed;
 public:
  explicit Execution_lex(THD *thd) : m_thd(thd),m_saved(thd->lex) {
    thd->change_list.move_elements_to(&m_changes);
    thd->lex=&m_call;m_failed=lex_start(thd);m_call.sql_command=SQLCOM_CALL;
  }
  ~Execution_lex() {
    m_thd->rollback_item_tree_changes();
    m_changes.move_elements_to(&m_thd->change_list);
    lex_end(&m_call);m_thd->lex=m_saved;
  }
  Execution_lex(const Execution_lex &)=delete;
  Execution_lex &operator=(const Execution_lex &)=delete;
  bool failed() const {return m_failed;}
};

class Typed_result final : public Query_result_send {
  sp_head *m_program;
  const Definition &m_definition;
  mem_root_deque<Item *> m_fields;
  Json_array *m_materialized;
  size_t m_rows = 0;
  size_t m_bytes = 0;
 public:
  Typed_result(THD *thd, sp_head *program, const Definition &definition,Json_array *materialized=nullptr)
      : m_program(program), m_definition(definition), m_fields(thd->mem_root),m_materialized(materialized) {}
  bool use_protocol_adapter() const override { return false; }
  bool send_result_set_metadata(THD *thd, const mem_root_deque<Item *> &source, uint flags) override {
    if (source.size() != m_definition.results.size()) return shape_error(thd);
    m_fields.clear();
    for (size_t index = 0; index < m_definition.results.size(); ++index) {
      const uint offset = static_cast<uint>(m_definition.parameters.size() + index);
      auto *variable = m_program->get_root_parsing_context()->find_variable(offset);
      auto *local = new (thd->mem_root) Item_splocal(to_lex_cstring(variable->name), offset, variable->type, 0, 0);
      if (!local) return true;
#ifndef NDEBUG
      local->m_sp = m_program;
#endif
      Item *field = local;
      if (field->fix_fields(thd, &field)) return true;
      const auto &column = m_definition.results[index];
      auto **reference=new (thd->mem_root) Item *(field);
      if (!reference) return true;
      field=new (thd->mem_root) Result_column(reference,column.nullable);
      if (!field) return true;
      // Item_ref's pointer constructor already copies the resolved slot's
      // properties and marks this reference fixed. Calling name resolution
      // again would dereference its intentionally absent query context.
      if (!field->fixed) return shape_error(thd);
      field->item_name.copy(column.name.data(), column.name.size(), system_charset_info, false);
      field->set_nullable(column.nullable);
      m_fields.push_back(field);
    }
    return m_materialized ? false : Query_result_send::send_result_set_metadata(thd, m_fields, flags);
  }
  bool send_data(THD *thd, const mem_root_deque<Item *> &source) override {
    if (++m_rows > 10000 || source.size() != m_fields.size()) return shape_error(thd);
    for (size_t index = 0; index < source.size(); ++index) {
      Item *value = source[index];
      if (thd->sp_runtime_ctx->set_variable(thd, false,
              static_cast<uint>(m_definition.parameters.size() + index), &value)) return true;
      if (!m_definition.results[index].nullable && m_fields[index]->is_null()) return shape_error(thd);
    }
    if (!m_materialized) return Query_result_send::send_data(thd, m_fields);
    auto row=create_dom_ptr<Json_object>();
    if (!row) return true;
    for (size_t index=0;index<m_fields.size();++index) {
      String buffer,value;Json_wrapper wrapper;
      Item *field=m_fields[index];
      // JSON consumers commonly parse decimal JSON numbers as IEEE doubles.
      // Materialized DECIMAL values therefore retain their exact text; their
      // declared type accompanies the tool/context result metadata.
      if (field->data_type()==MYSQL_TYPE_NEWDECIMAL || field->data_type()==MYSQL_TYPE_DECIMAL) {
        String *decimal=field->val_str(&buffer);
        wrapper=decimal ? Json_wrapper(create_dom_ptr<Json_string>(std::string(decimal->ptr(),decimal->length()))) : Json_wrapper(create_dom_ptr<Json_null>());
      } else if (get_atom_null_as_null(&field,0,"Knowledge result",&buffer,&value,&wrapper)) return true;
      String encoded;
      if (wrapper.to_string(&encoded,true,"Knowledge result",JsonDepthErrorHandler)) return true;
      m_bytes+=encoded.length();
      if (m_bytes>1048576 || row->add_alias(m_definition.results[index].name,wrapper.clone_dom())) return shape_error(thd);
    }
    return m_materialized->append_alias(std::move(row));
  }
  bool send_eof(THD *thd) override { return m_materialized ? false : Query_result_send::send_eof(thd); }
};

inline bool execute(THD *thd, const Definition &definition, const mem_root_deque<Item *> &arguments,Json_array *materialized=nullptr) {
  Main_identity identity(thd);
  auto program = compile(thd, definition);
  if (!program) return true;
  if (arguments.size() != definition.parameters.size()) return shape_error(thd);
  Table_locks locks(thd,&definition.dependencies);
  LEX *lex = readonly_lex(thd, program.get(), definition.parameters.size(), &locks);
  if (!lex) return true;
  mem_root_deque<Item *> values(thd->mem_root);
  for (size_t index = 0; index < arguments.size(); ++index) {
    auto reject = [](Item *item) { return pure_item(item, static_cast<size_t>(-1)) || item->type() == Item::SUBQUERY_ITEM; };
    if (arguments[index]->walk(&Item::walk_helper_thunk<decltype(reject)>, enum_walk::PREFIX | enum_walk::SUBQUERY,
                              reinterpret_cast<uchar *>(&reject))) return readonly_error(thd);
    Item *value = arguments[index];
    if (!value->fixed && value->fix_fields(thd, &value)) return true;
    if (!definition.parameters[index].nullable && value->is_null()) return shape_error(thd);
    values.push_back(value);
  }
  for (size_t index = 0; index < definition.results.size(); ++index)
    values.push_back(new (thd->mem_root) Item_null());
  Typed_result result(thd, program.get(), definition,materialized);
  if (!dynamic_cast<Sql_cmd_dml *>(lex->m_sql_cmd)) return shape_error(thd);
  // Sql_cmd_dml::query_result()/set_query_result() require prepare() to have
  // initialized the command's LEX pointer. Anonymous programs are unprepared
  // here; native prepare() takes its result interceptor from LEX::result.
  Query_result *saved = lex->result;
  auto restore = create_scope_guard([&]() { lex->result = saved; });
  lex->result = &result;
  if (!materialized && thd->in_sub_stmt && program->is_not_allowed_in_function(
          thd->in_sub_stmt & SUB_STMT_TRIGGER ? "trigger" : "function")) return true;
  if (!materialized && !thd->get_protocol()->has_client_capability(CLIENT_MULTI_RESULTS)) {
    my_error(ER_SP_BADSELECT,MYF(0),"Knowledge");return true;
  }
  const uint clear_bits=(~thd->server_status & SERVER_MORE_RESULTS_EXISTS);
  if (!materialized) thd->server_status|=SERVER_MORE_RESULTS_EXISTS;
  auto restore_status=create_scope_guard([&]() {thd->server_status&=~clear_bits;});
  {
    Execution_lex caller(thd);
    if (caller.failed() || program->execute_procedure(thd, &values)) return true;
  }
  thd->server_status&=~clear_bits;
  if (!materialized) my_ok(thd,0);
  return false;
}

inline Json_dom_ptr tool_parameters(sp_head *program,const Definition &definition) {
  auto schema=create_dom_ptr<Json_object>();auto properties=create_dom_ptr<Json_object>();auto required=create_dom_ptr<Json_array>();
  if (!schema || !properties || !required) return {};
  for (size_t index=0;index<definition.parameters.size();++index) {
    const auto &column=definition.parameters[index];
    const auto &field=program->get_root_parsing_context()->find_variable(static_cast<uint>(index))->field_def;
    auto property=create_dom_ptr<Json_object>();auto types=create_dom_ptr<Json_array>();
    if (!property || !types) return {};
    const char *type="string";unsigned bits=0;
    switch (field.sql_type) {
      case MYSQL_TYPE_TINY:bits=8;break;
      case MYSQL_TYPE_SHORT:bits=16;break;
      case MYSQL_TYPE_INT24:bits=24;break;
      case MYSQL_TYPE_LONG:bits=32;break;
      case MYSQL_TYPE_LONGLONG:bits=64;break;
      case MYSQL_TYPE_FLOAT:case MYSQL_TYPE_DOUBLE:type="number";break;
      case MYSQL_TYPE_DECIMAL:case MYSQL_TYPE_NEWDECIMAL:
        type="string";
        if (property->add_alias("maxLength",create_dom_ptr<Json_uint>(field.max_display_width_in_codepoints()+4))) return {};
        break;
      case MYSQL_TYPE_DATE:case MYSQL_TYPE_YEAR:case MYSQL_TYPE_TIME:case MYSQL_TYPE_TIME2:
      case MYSQL_TYPE_DATETIME:case MYSQL_TYPE_DATETIME2:case MYSQL_TYPE_TIMESTAMP:case MYSQL_TYPE_TIMESTAMP2:break;
      case MYSQL_TYPE_VARCHAR:case MYSQL_TYPE_VAR_STRING:case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_BLOB:case MYSQL_TYPE_TINY_BLOB:case MYSQL_TYPE_MEDIUM_BLOB:case MYSQL_TYPE_LONG_BLOB:
        if (field.charset==&my_charset_bin) return {};
        if (property->add_alias("maxLength",create_dom_ptr<Json_uint>(std::min<size_t>(field.max_display_width_in_codepoints(),1048576)))) return {};break;
      case MYSQL_TYPE_JSON:
        if (properties->add_alias(column.name,create_dom_ptr<Json_boolean>(true)) || required->append_alias(create_dom_ptr<Json_string>(column.name))) return {};
        continue;
      default:return {};
    }
    if (bits) {
      type="integer";
      const ulonglong maximum=bits==64 ? std::numeric_limits<ulonglong>::max() : ((1ULL<<bits)-1);
      if (field.is_unsigned) {
        if (property->add_alias("minimum",create_dom_ptr<Json_uint>(0)) || property->add_alias("maximum",create_dom_ptr<Json_uint>(maximum))) return {};
      } else {
        const longlong maximum_signed=static_cast<longlong>(maximum>>1);
        if (property->add_alias("minimum",create_dom_ptr<Json_int>(-maximum_signed-1)) || property->add_alias("maximum",create_dom_ptr<Json_int>(maximum_signed))) return {};
      }
    }
    if (types->append_alias(create_dom_ptr<Json_string>(type)) ||
        (column.nullable && types->append_alias(create_dom_ptr<Json_string>("null"))) ||
        property->add_alias("type",std::move(types)) || properties->add_alias(column.name,std::move(property)) ||
        required->append_alias(create_dom_ptr<Json_string>(column.name))) return {};
  }
  if (schema->add_alias("type",create_dom_ptr<Json_string>("object")) || schema->add_alias("properties",std::move(properties)) ||
      schema->add_alias("required",std::move(required)) || schema->add_alias("additionalProperties",create_dom_ptr<Json_boolean>(false))) return {};
  return schema;
}

inline Json_dom_ptr validate_document(THD *thd, const Json_dom *document) {
  Main_identity identity(thd);
  Definition definition;
  if (read_definition(thd,document,&definition)) return {};
  auto program = compile(thd,definition);
  if (!program) return {};
  Table_locks locks(thd);
  if (!readonly_lex(thd,program.get(),definition.parameters.size(),&locks)) return {};
  auto parameters=create_dom_ptr<Json_array>(),results=create_dom_ptr<Json_array>();
  if (!parameters || !results || normalize_columns(thd,definition,program.get(),false,parameters.get()) || normalize_columns(thd,definition,program.get(),true,results.get())) return {};
  auto output=create_dom_ptr<Json_object>();
  auto parameters_schema=tool_parameters(program.get(),definition);
  auto dependencies=create_dom_ptr<Json_array>();
  if (!dependencies) return {};
  for (const auto &table:locks.dependencies()) {
    auto dependency=create_dom_ptr<Json_object>();
    if (!dependency || dependency->add_alias("kind",create_dom_ptr<Json_string>("Table")) ||
        dependency->add_alias("schema_name",create_dom_ptr<Json_string>(table.schema)) || dependency->add_alias("name",create_dom_ptr<Json_string>(table.name)) ||
        dependency->add_alias("object_id",create_dom_ptr<Json_string>(table.id)) || dependencies->append_alias(std::move(dependency))) return {};
  }
  if (!output || output->add_alias("definition",create_dom_ptr<Json_string>(definition.body)) ||
      output->add_alias("parameters",std::move(parameters)) || output->add_alias("result_columns",std::move(results)) ||
      output->add_alias("dependencies",std::move(dependencies))) return {};
  if (parameters_schema && output->add_alias("tool_parameters",std::move(parameters_schema))) return {};
  return output;
}

inline std::string sql_text(const std::string &value) {
  static const char hex[]="0123456789abcdef";
  std::string output="CONVERT(X'";
  for (unsigned char byte:value) { output+=hex[byte>>4];output+=hex[byte&15]; }
  return output+"' USING utf8mb4)";
}

inline Json_dom_ptr lookup(THD *thd, const std::string &schema, const std::string &name) {
  sdp_ai::Catalog_guard catalog;
  if (catalog.pin(thd)) {error(thd,61081,"42000","510001: Native AI catalog routine identity is not trusted.");return {};}
  std::string source="CALL sdp_ai.knowledge_document("+sql_text(schema)+","+sql_text(name)+")";
  sp_rcontext *outer_frame=thd->sp_runtime_ctx;
  thd->sp_runtime_ctx=nullptr;
  auto restore_frame=create_scope_guard([&]() {thd->sp_runtime_ctx=outer_frame;});
  Ed_connection connection(thd,true);
  LEX_STRING statement{source.data(),source.size()};
  if (connection.execute_direct(statement)) {
    error(thd,connection.get_last_errno(),connection.get_last_sqlstate(),connection.get_last_error()); return {};
  }
  auto *result=connection.get_result_sets();
  if (!result || result->size()!=1 || result->get_field_count()!=1) { shape_error(thd);return {}; }
  List<Ed_row> &rows=*result;
  const auto &value=(*rows.head())[0];
  if (!value.str || value.length>4*1024*1024) { shape_error(thd);return {}; }
  return parse_document(thd,value.str,value.length);
}

}  // namespace sdp_knowledge

class Item_func_sdp_knowledge_validate final : public Item_str_func {
 public:
  Item_func_sdp_knowledge_validate(const POS &pos,Item *document) : Item_str_func(pos,document) {}
  const char *func_name() const override { return "stardataparadise_knowledge_validate"; }
  table_map get_initial_pseudo_tables() const override { return RAND_TABLE_BIT; }
  bool resolve_type(THD *) override { set_data_type_string(4U*1024*1024);return false; }
  String *val_str(String *output) override {
    // A cached SP expression must recover from a previous error_str().
    null_value=false;
    String buffer;
    String *value=args[0]->val_str(&buffer);
    if (!value || value->length()>4U*1024*1024) { sdp_knowledge::shape_error(current_thd);return error_str(); }
    auto source=sdp_knowledge::parse_document(current_thd,value->ptr(),value->length());
    if (!source) return error_str();
    auto document=sdp_knowledge::validate_document(current_thd,source.get());
    if (!document) return error_str();
    Json_wrapper wrapper(std::move(document));
    output->set_charset(&my_charset_utf8mb4_bin);output->length(0);
    if (wrapper.to_string(output,true,func_name(),[&]() {sdp_knowledge::shape_error(current_thd);})) return error_str();
    return output;
  }
};

class Sql_cmd_sdp_execute_knowledge final : public Sql_cmd {
  Table_ident *m_target;
  mem_root_deque<Item *> *m_arguments;
 public:
  Sql_cmd_sdp_execute_knowledge(Table_ident *target,mem_root_deque<Item *> *arguments)
      : m_target(target),m_arguments(arguments) {}
  enum_sql_command sql_command_code() const override { return SQLCOM_CALL; }
  bool are_dynamic_parameters_allowed() const override { return true; }
  bool prepare(THD *thd) override {
    if (is_prepared()) return false;
    if (!global_system_variables.sdp_ai_sql) {
      my_error(ER_OPTION_PREVENTS_STATEMENT,MYF(0),"--sdp-ai-sql=OFF");return true;
    }
    if (!needs_explicit_preparation()) {set_prepared();return false;}
    // Validate every argument before fix_fields can initialize expressions.
    // CAST parameter types must resolve while PS markers still have NO_VALUE.
    if (m_arguments) for (Item *item:*m_arguments) {
      auto reject=[](Item *value) {return sdp_knowledge::pure_item(value,static_cast<size_t>(-1)) || value->type()==Item::SUBQUERY_ITEM;};
      if (item->walk(&Item::walk_helper_thunk<decltype(reject)>,enum_walk::PREFIX|enum_walk::SUBQUERY,reinterpret_cast<uchar *>(&reject))) return sdp_knowledge::readonly_error(thd);
    }
    sdp_knowledge::Main_identity identity(thd);
    if (m_arguments) for (Item *&item:*m_arguments) {
      if (item->type()==Item::PARAM_ITEM) continue;
      if ((!item->fixed && item->fix_fields(thd,&item)) || item->check_cols(1)) return true;
    }
    set_prepared();return false;
  }
  bool execute(THD *thd) override {
    sdp_knowledge::Main_identity identity(thd);
    const auto db=m_target->db.str ? m_target->db : thd->db();
    if (!db.str) {my_error(ER_NO_DB_ERROR,MYF(0));return true;}
    auto document=sdp_knowledge::lookup(thd,std::string(db.str,db.length),std::string(m_target->table.str,m_target->table.length));
    sdp_knowledge::Definition definition;
    if (!document || sdp_knowledge::read_definition(thd,document.get(),&definition)) return true;
    mem_root_deque<Item *> empty(thd->mem_root);
    return sdp_knowledge::execute(thd,definition,m_arguments ? *m_arguments : empty);
  }
};

class PT_sdp_execute_knowledge final : public Parse_tree_root {
  Table_ident *m_target;
  PT_item_list *m_arguments;
 public:
  PT_sdp_execute_knowledge(const POS &pos,Table_ident *target,PT_item_list *arguments)
      : Parse_tree_root(pos),m_target(target),m_arguments(arguments) {}
  Sql_cmd *make_cmd(THD *thd) override {
    thd->lex->sql_command=SQLCOM_CALL;
    Parse_context context(thd,thd->lex->query_block);
    if (m_arguments && m_arguments->contextualize(&context)) return nullptr;
    return new (thd->mem_root) Sql_cmd_sdp_execute_knowledge(m_target,m_arguments ? &m_arguments->value : nullptr);
  }
};

// PT_call is final. Compose the ordinary native CALL node so its contextualized
// Item tree can be checked before Sql_cmd_call::prepare_inner/fix_fields and
// before MySQL's CALL argument evaluation can commit a mutating function.
class PT_sdp_ai_call final : public Parse_tree_root {
  PT_call *m_call;
  PT_item_list *m_arguments;
  bool m_native_commit;
 public:
  PT_sdp_ai_call(const POS &pos,PT_call *call,PT_item_list *arguments,const char *operation)
      : Parse_tree_root(pos),m_call(call),m_arguments(arguments),
        m_native_commit(strcmp(operation,"commit_sync")==0 || strcmp(operation,"commit_async")==0) {}
  Sql_cmd *make_cmd(THD *thd) override {
    Sql_cmd *command=m_call->make_cmd(thd);
    if (!command) return nullptr;
    for (Item *argument:m_arguments->value) {
      auto collect=[&](Item *item) {
        if (auto *local=dynamic_cast<Item_sdp_ai_local_target *>(item))
          return static_cast<Sql_cmd_call *>(command)->sdp_add_local_target(thd,local->target());
        return false;
      };
      if (argument->walk(&Item::walk_helper_thunk<decltype(collect)>,enum_walk::PREFIX,
          reinterpret_cast<uchar *>(&collect))) return nullptr;
      auto reject=[](Item *item) {
        return item->type()==Item::SUBQUERY_ITEM ||
            sdp_knowledge::pure_item(item,static_cast<size_t>(-1));
      };
      if (argument->walk(&Item::walk_helper_thunk<decltype(reject)>,
          enum_walk::PREFIX|enum_walk::SUBQUERY,reinterpret_cast<uchar *>(&reject))) {
        sdp_knowledge::error(thd,61006,"0A000",
            "502006: AI command arguments require supported readonly expressions.");
        return nullptr;
      }
    }
    static_cast<Sql_cmd_call *>(command)->sdp_enable_capture(m_native_commit);
    return command;
  }
};
#endif
