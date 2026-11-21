#ifndef SQL_SDP_AI_COMMIT_SCOPE_H
#define SQL_SDP_AI_COMMIT_SCOPE_H
#include "sql/sdp_ai_catalog_guard.h"

namespace sdp_ai {
// A native COMMIT producer must pin its dispatcher and validation graph while
// it executes. Release only those borrowed pins at the physical commit boundary
// and restore them after provider waiting, before the stored dispatcher resumes.
class Commit_catalog_scope final {
  inline static thread_local Commit_catalog_scope *active=nullptr;
  Commit_catalog_scope *m_previous;
  THD *m_thd;
  Catalog_guard *m_guard;
  bool m_released=false;
 public:
  Commit_catalog_scope(THD *thd,Catalog_guard *guard)
      :m_previous(active),m_thd(thd),m_guard(guard) {active=this;}
  ~Commit_catalog_scope() {active=m_previous;}
  Commit_catalog_scope(const Commit_catalog_scope &)=delete;
  Commit_catalog_scope &operator=(const Commit_catalog_scope &)=delete;
  static void release_for(THD *thd) {
    for (auto *scope=active;scope;scope=scope->m_previous)
      if (scope->m_thd==thd && !scope->m_released) {
        scope->m_guard->clear();scope->m_released=true;
      }
  }
  static bool resume_for(THD *thd) {
    for (auto *scope=active;scope;scope=scope->m_previous)
      if (scope->m_thd==thd && scope->m_released) {
        if (scope->m_guard->pin(thd)) return true;
        scope->m_released=false;
      }
    return false;
  }
};
}
#endif
