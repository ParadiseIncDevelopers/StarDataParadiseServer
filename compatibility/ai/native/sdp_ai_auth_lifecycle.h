// Couple custom AI grant identities to the native authID DDL transaction.
#ifndef SQL_SDP_AI_AUTH_LIFECYCLE_H
#define SQL_SDP_AI_AUTH_LIFECYCLE_H
#include "sql/sdp_ai_action.h"
#include "sql/dd/dd_table.h"
#include "sql/dd/types/table.h"

namespace sdp_ai_action {
class Authid_lifecycle_guard final {
  THD *m_thd;
  bool m_managed{false};
  bool m_captured{false};
  std::string m_from_user,m_from_host;
  sdp_ai::Catalog_guard m_catalog;
  sdp_native::Routine_guard m_dispatch;
 public:
  explicit Authid_lifecycle_guard(THD *thd) : m_thd(thd) {}
  // Native ACL DDL has already checked its caller's authority. Probe metadata
  // before its ACL write lock and keep these table MDLs in that transaction.
  // The startup enforcement toggle cannot disable an installed module's
  // housekeeping: otherwise maintenance OFF + DROP/CREATE could revive grants.
  bool pin() {
    for (const char *name:{"action_policies","action_grants","actions"}) {
      MDL_request request;
      MDL_REQUEST_INIT(&request,MDL_key::TABLE,"sdp_ai",name,
                       MDL_SHARED_READ,MDL_TRANSACTION);
      if (m_thd->mdl_context.acquire_lock(&request,m_thd->variables.lock_wait_timeout)) return true;
      bool exists=false;
      if (dd::table_exists(m_thd->dd_client(),"sdp_ai",name,&exists)) return true;
      m_managed=m_managed || exists;
    }
    if (!m_managed) return false; // Core-only installations retain native DDL.
    // All participants must remain transactional until native ACL commit.
    // A partially removed module or a view/nontransactional replacement must
    // fail before its cleanup can make irreversible privilege changes.
    for (const char *name:{"action_policies","action_grants","actions","object_grants"}) {
      MDL_request request;
      MDL_REQUEST_INIT(&request,MDL_key::TABLE,"sdp_ai",name,
                       MDL_SHARED_READ,MDL_TRANSACTION);
      if (m_thd->mdl_context.acquire_lock(&request,m_thd->variables.lock_wait_timeout)) return true;
      const dd::cache::Dictionary_client::Auto_releaser release(m_thd->dd_client());
      const dd::Table *table=nullptr;
      if (m_thd->dd_client()->acquire<dd::Table>("sdp_ai",name,&table)) return true;
      if (!table || table->engine()!="InnoDB" || table->is_temporary()) return denied(m_thd);
    }
    if (m_catalog.pin(m_thd) || m_dispatch.pin(m_thd,"sdp_ai","action_dispatch",
          "sdp_ai_internal","localhost",dd::View::ST_DEFINER)) return denied(m_thd);
    return false;
  }
  void capture(const LEX_USER *from) {
    if (!m_managed) return;
    m_captured=false;
    if (!from || !from->user.str || !from->host.str) return;
    assert(assert_acl_cache_write_lock(m_thd));
    // Native hosts compare case-insensitively. Keep the stored authID spelling
    // before handle_grant_data removes/renames its ACL cache record.
    const ACL_USER *user=find_acl_user(from->host.str,from->user.str,true);
    m_from_user=user && user->user ? user->user : from->user.str;
    m_from_host=user && user->host.get_host() ? user->host.get_host() : from->host.str;
    m_captured=true;
  }
  bool change(const LEX_USER *from,const LEX_USER *to=nullptr) {
    if (!m_managed) return false;
    if (!m_captured || !from) return denied(m_thd);
    LEX_USER canonical=*from;
    canonical.user={m_from_user.c_str(),m_from_user.size()};
    canonical.host={m_from_host.c_str(),m_from_host.size()};
    // Called only after native handle_grant_data() reports a real mutation.
    // No nested BEGIN/COMMIT: its writes share native log_and_commit_acl_ddl().
    return lifecycle(m_thd,&canonical,to);
  }
};
} // namespace sdp_ai_action
#endif
