// Canonical native DD identity and MDL key for routines. Routine names are
// case/accent insensitive; a raw PROCEDURE/FUNCTION MDL key is invalid.
#ifndef SQL_SDP_AI_IDENTITY_H
#define SQL_SDP_AI_IDENTITY_H
#include <string>
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/types/function.h"
#include "sql/dd/types/procedure.h"
#include "sql/dd/types/schema.h"
#include "sql/mdl.h"
#include "sql/mysqld.h"
#include "sql/sql_class.h"
#include "mysqld_error.h"

namespace sdp_ai_identity {
class Routine_target final {
  THD *m_thd;MDL_ticket *m_ticket=nullptr;std::string m_schema,m_name;dd::Object_id m_id=dd::INVALID_OBJECT_ID;
 public:
  explicit Routine_target(THD *thd):m_thd(thd) {}
  Routine_target(const Routine_target &)=delete;Routine_target &operator=(const Routine_target &)=delete;
  ~Routine_target() {if (m_ticket) m_thd->mdl_context.release_lock(m_ticket);}
  const std::string &schema() const {return m_schema;}
  const std::string &name() const {return m_name;}
  dd::Object_id id() const {return m_id;}
  bool pin(const std::string &schema,const std::string &name,bool function=false) {
    if (m_ticket || schema.empty() || name.empty() || schema.size()>NAME_LEN || name.size()>NAME_LEN ||
        schema.find('\0')!=std::string::npos || name.find('\0')!=std::string::npos) {my_error(ER_WRONG_ARGUMENTS,MYF(0),"native routine identity");return true;}
    std::string lookup=schema;
    if (lower_case_table_names) {my_casedn_str(system_charset_info,lookup.data());lookup.resize(strlen(lookup.c_str()));}
    MDL_key key;
    if (function) dd::Function::create_mdl_key(lookup.c_str(),name.c_str(),&key);else dd::Procedure::create_mdl_key(lookup.c_str(),name.c_str(),&key);
    MDL_request request;MDL_REQUEST_INIT_BY_KEY(&request,&key,MDL_SHARED,MDL_EXPLICIT);
    if (m_thd->mdl_context.acquire_lock(&request,m_thd->variables.lock_wait_timeout)) return true;
    m_ticket=request.ticket;
    const dd::cache::Dictionary_client::Auto_releaser release(m_thd->dd_client());
    const dd::Schema *database=nullptr;
    if (m_thd->dd_client()->acquire<dd::Schema>(lookup.c_str(),&database)) return true;
    const dd::Routine *routine=nullptr;
    if (function) {const dd::Function *value=nullptr;if (m_thd->dd_client()->acquire<dd::Function>(lookup.c_str(),name.c_str(),&value)) return true;routine=value;}
    else {const dd::Procedure *value=nullptr;if (m_thd->dd_client()->acquire<dd::Procedure>(lookup.c_str(),name.c_str(),&value)) return true;routine=value;}
    if (!database || !routine) {const std::string full=schema+"."+name;my_error(ER_SP_DOES_NOT_EXIST,MYF(0),function?"FUNCTION":"PROCEDURE",full.c_str());return true;}
    m_schema.assign(database->name().data(),database->name().size());
    if (lower_case_table_names) {my_casedn_str(system_charset_info,m_schema.data());m_schema.resize(strlen(m_schema.c_str()));}
    m_name.assign(routine->name().data(),routine->name().size());m_id=routine->id();return false;
  }
};
} // namespace sdp_ai_identity
#endif
