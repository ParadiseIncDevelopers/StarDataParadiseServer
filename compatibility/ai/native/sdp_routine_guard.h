// A private catalog RPC pins its trusted DD routine identity through execution.
#ifndef SQL_SDP_ROUTINE_GUARD_H
#define SQL_SDP_ROUTINE_GUARD_H
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/types/function.h"
#include "sql/dd/types/procedure.h"
#include "sql/dd/types/routine.h"
#include "sql/dd/types/view.h"
#include "sql/mdl.h"
#include "sql/sql_class.h"

namespace sdp_native {
class Routine_guard final {
  THD *m_thd=nullptr;
  MDL_ticket *m_ticket=nullptr;
 public:
  Routine_guard()=default;
  Routine_guard(const Routine_guard &)=delete;
  Routine_guard &operator=(const Routine_guard &)=delete;
  ~Routine_guard() {if (m_ticket) m_thd->mdl_context.release_lock(m_ticket);}
  // false=trusted and pinned; true=failure. The caller raises its domain error
  // for a metadata mismatch; existing MDL/DD engine errors remain available.
  bool pin(THD *thd,const char *schema,const char *name,const char *definer_user,
           const char *definer_host,dd::View::enum_security_type security,bool function=false) {
    assert(!m_ticket);m_thd=thd;
    // Routine MDL names use the DD's case/accent normalization. A raw
    // MDL_key::FUNCTION/PROCEDURE key belongs to a different lock domain and
    // would not exclude ordinary ALTER/DROP ROUTINE during this RPC.
    MDL_key key;
    dd::Routine::create_mdl_key(function?dd::Routine::RT_FUNCTION:dd::Routine::RT_PROCEDURE,schema,name,&key);
    MDL_request request;
    MDL_REQUEST_INIT_BY_KEY(&request,&key,MDL_SHARED,MDL_EXPLICIT);
    if (thd->mdl_context.acquire_lock(&request,thd->variables.lock_wait_timeout)) return true;
    m_ticket=request.ticket;
    const dd::cache::Dictionary_client::Auto_releaser release(thd->dd_client());
    const dd::Routine *routine=nullptr;
    if (function) {
      const dd::Function *value=nullptr;
      if (thd->dd_client()->acquire<dd::Function>(schema,name,&value)) return true;
      routine=value;
    } else {
      const dd::Procedure *value=nullptr;
      if (thd->dd_client()->acquire<dd::Procedure>(schema,name,&value)) return true;
      routine=value;
    }
    return !routine || routine->definer_user()!=definer_user || routine->definer_host()!=definer_host || routine->security_type()!=security;
  }
};
} // namespace sdp_native
#endif
