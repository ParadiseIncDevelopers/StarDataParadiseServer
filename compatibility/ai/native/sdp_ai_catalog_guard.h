// Fixed transitive catalog boundary. Replaced namespace helpers are untrusted.
#ifndef SQL_SDP_AI_CATALOG_GUARD_H
#define SQL_SDP_AI_CATALOG_GUARD_H
#include <memory>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include "sdp_routine_guard.h"

namespace sdp_ai {
class Catalog_guard final {
  std::vector<std::unique_ptr<sdp_native::Routine_guard>> m_guards;
  inline static constexpr const char *functions[]={"json_kind","can_use","has_pending_turn","run_receipt","merge_memory","boolean_value","knowledge_value"};
  inline static constexpr const char *invoker_functions[]={"has_pending_turn"};
  inline static constexpr const char *procedures[]={"raise_error","assert_worker","validate_options","check_response_schema","validate_response",
    "object_document","dispatch","knowledge_document","before_commit","after_commit","poll_commit","rollback_turn","claim_run",
    "renew_lease","publish_run","claim_model_import","renew_model_import","publish_model_import","check_capture","prepare_response",
    "check_behaviors","apply_behaviors","stage_behavior","behavior_binding","named_behavior"};
  inline static constexpr const char *tool_procedures[]={"publish_tool_requests","begin_tool_call","finish_tool_call","knowledge_tool_document","tool_call_status","publish_tool_failure","tool_owner_context","bind_tool_action","tool_action_document","resolve_tool_action"};
  inline static constexpr const char *action_procedures[]={"action_dispatch"};
  bool one(THD *thd,const char *name,bool function) {
    auto guard=std::make_unique<sdp_native::Routine_guard>();
    bool invoker=false;
    if (function) for (const char *value:invoker_functions) if (strcmp(name,value)==0) invoker=true;
    if (guard->pin(thd,"sdp_ai",name,"sdp_ai_internal","localhost",invoker?dd::View::ST_INVOKER:dd::View::ST_DEFINER,function)) return true;
    m_guards.push_back(std::move(guard));return false;
  }
 public:
  void clear() {m_guards.clear();}
  static bool worker_entry(std::string name,bool *tools) {
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) {return std::tolower(c);});
    *tools=false;
    for (const char *value:{"claim_run","renew_lease","publish_run","claim_model_import","renew_model_import","publish_model_import"})
      if (name==value) return true;
    for (const char *value:{"publish_tool_requests","tool_call_status","publish_tool_failure"})
      if (name==value) {*tools=true;return true;}
    return false;
  }
  static bool known_rpc(std::string name,bool *tools) {
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) {return std::tolower(c);});
    *tools=false;
    for (const char *value:procedures) if (name==value) return true;
    for (const char *value:tool_procedures) if (name==value) {*tools=true;return true;}
    // Action RPC has an additional explicit pin_actions()/target guard held
    // by its native dispatcher. It is never a raw public worker entry.
    for (const char *value:action_procedures) if (name==value) return true;
    return false;
  }
  // The native caller selects its compiled feature manifest. SQL/document data
  // cannot opt out of a required member or supply a definer to trust.
  bool pin(THD *thd,bool tools=false) {
    for (const char *name:functions) if (one(thd,name,true)) return true;
    for (const char *name:procedures) if (one(thd,name,false)) return true;
    if (tools) for (const char *name:tool_procedures) if (one(thd,name,false)) return true;
    return false;
  }
  bool pin_actions(THD *thd) {
    if (pin(thd)) return true;
    for (const char *name:action_procedures) if (one(thd,name,false)) return true;
    return false;
  }
};
} // namespace sdp_ai
#endif
