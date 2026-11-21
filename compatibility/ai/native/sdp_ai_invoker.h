// Trusted, stack-bounded tool identity. SQL variables cannot enter this scope.
#ifndef SDP_AI_INVOKER_H
#define SDP_AI_INVOKER_H

#include <cassert>
#include <string>
#include "sql/auth/sql_security_ctx.h"
#include "sql/current_thd.h"
#include "sql/item_func.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/tztime.h"

namespace sdp_ai_invoker {
// The protected run captures the initiating SQL session's timezone. Apply it
// only while executing owner application code; all product lease/action RPCs
// use UTC. Neither scope is controllable through a client session variable.
class Session_timezone_scope final {
  THD *m_thd;
  Time_zone *m_saved;
 public:
  explicit Session_timezone_scope(THD *thd) : m_thd(thd),m_saved(thd->variables.time_zone) {}
  ~Session_timezone_scope() {m_thd->variables.time_zone=m_saved;}
  Session_timezone_scope(const Session_timezone_scope &)=delete;
  Session_timezone_scope &operator=(const Session_timezone_scope &)=delete;
  bool apply(const std::string &name) {
    if (name.empty() || name.size()>128 || name.find('\0')!=std::string::npos) return true;
    String value(name.data(),name.size(),&my_charset_utf8mb4_bin);
    Time_zone *zone=my_tz_find(m_thd,&value);
    if (!zone) return true;
    m_thd->variables.time_zone=zone;return false;
  }
  void utc() {m_thd->variables.time_zone=my_tz_UTC;}
};
class Metadata_timezone_scope final {
  Session_timezone_scope m_scope;
 public:
  explicit Metadata_timezone_scope(THD *thd) : m_scope(thd) {m_scope.utc();}
  Metadata_timezone_scope(const Metadata_timezone_scope &)=delete;
  Metadata_timezone_scope &operator=(const Metadata_timezone_scope &)=delete;
};
class Scope;
class Dispatch_scope;
class Worker_rpc_scope;
inline thread_local const Scope *current_scope = nullptr;
inline thread_local const Scope *publication_scope = nullptr;
inline thread_local const Dispatch_scope *dispatch_scope = nullptr;
inline thread_local const Worker_rpc_scope *worker_rpc_scope = nullptr;

class Worker_rpc_scope final {
  THD *m_thd;
  const Worker_rpc_scope *m_previous;
 public:
  explicit Worker_rpc_scope(THD *thd) : m_thd(thd),m_previous(worker_rpc_scope) {worker_rpc_scope=this;}
  Worker_rpc_scope(const Worker_rpc_scope &)=delete;
  Worker_rpc_scope &operator=(const Worker_rpc_scope &)=delete;
  ~Worker_rpc_scope() {assert(worker_rpc_scope==this);worker_rpc_scope=m_previous;}
  THD *thd() const {return m_thd;}
};
inline bool worker_rpc_active(const THD *thd) {
  return worker_rpc_scope && worker_rpc_scope->thd()==thd;
}

class Dispatch_scope final {
  THD *m_thd;
  std::string m_call_id;
 public:
  Dispatch_scope(THD *thd,std::string call_id) : m_thd(thd),m_call_id(std::move(call_id)) {
    assert(!dispatch_scope && !current_scope);dispatch_scope=this;
  }
  Dispatch_scope(const Dispatch_scope &)=delete;
  Dispatch_scope &operator=(const Dispatch_scope &)=delete;
  ~Dispatch_scope() { assert(dispatch_scope==this);dispatch_scope=nullptr; }
  THD *thd() const { return m_thd; }
  const std::string &call_id() const { return m_call_id; }
};

class Scope final {
  THD *m_thd;
  Security_context *m_owner;
  const Scope *m_previous;
  std::string m_call_id;
 public:
  // Only the native executor constructs this after locking and validating a
  // leased protected tool-call record. The owner uses ordinary ACL lookup.
  Scope(THD *thd, Security_context *owner, std::string call_id)
      : m_thd(thd), m_owner(owner), m_previous(current_scope),
        m_call_id(std::move(call_id)) {
    assert(thd && owner && (!m_previous || m_previous->thd()!=thd));
    current_scope=this;
  }
  Scope(const Scope &)=delete;
  Scope &operator=(const Scope &)=delete;
  ~Scope() { assert(current_scope==this); current_scope=m_previous; }
  THD *thd() const { return m_thd; }
  Security_context *owner() const { return m_owner; }
  const std::string &call_id() const { return m_call_id; }
};

inline bool active(const THD *thd) {
  return current_scope && current_scope->thd()==thd;
}
inline bool dispatching(const THD *thd,const std::string &call_id) {
  return dispatch_scope && dispatch_scope->thd()==thd && dispatch_scope->call_id()==call_id;
}
inline Security_context *logical_context(THD *thd) {
  return active(thd) ? current_scope->owner() : &thd->m_main_security_ctx;
}
inline const Security_context &actor_context(THD *thd) {
  return *logical_context(thd);
}
inline const Security_context &transport_context(THD *thd) {
  return thd->m_main_security_ctx;
}
inline bool publishing(const THD *thd,const std::string &call_id) {
  return publication_scope && publication_scope->thd()==thd &&
         publication_scope->call_id()==call_id;
}

// The native executor alone suspends its scope after app code has returned,
// while publishing the result under the authenticated worker's bounded APIs.
// Application SQL has no entry point to this guard.
class Publication_scope final {
  const Scope *m_saved;
 public:
  explicit Publication_scope(THD *thd) : m_saved(current_scope) {
    assert(active(thd) && !publication_scope);
    publication_scope=m_saved; current_scope=nullptr;
  }
  Publication_scope(const Publication_scope &)=delete;
  Publication_scope &operator=(const Publication_scope &)=delete;
  ~Publication_scope() {
    assert(current_scope==nullptr && publication_scope==m_saved);
    current_scope=m_saved;publication_scope=nullptr;
  }
};
} // namespace sdp_ai_invoker

class Item_func_sdp_tool_execution_active final : public Item_int_func {
 public:
  explicit Item_func_sdp_tool_execution_active(const POS &pos) : Item_int_func(pos) {}
  longlong val_int() override { assert(fixed);return sdp_ai_invoker::active(current_thd); }
  bool do_itemize(Parse_context *pc, Item **res) override {
    if (skip_itemize(res)) return false;
    if (Item_int_func::do_itemize(pc,res)) return true;
    pc->thd->lex->safe_to_cache_query=false;
    pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    return false;
  }
  table_map get_initial_pseudo_tables() const override { return RAND_TABLE_BIT; }
  const char *func_name() const override { return "stardataparadise_tool_execution_active"; }
  bool resolve_type(THD *) override { set_data_type_bool();return false; }
};
class Item_func_sdp_tool_publication_active final : public Item_int_func {
 public:
  Item_func_sdp_tool_publication_active(const POS &pos,Item *call) : Item_int_func(pos,call) {}
  longlong val_int() override {
    assert(fixed);String buffer;String *value=args[0]->val_str(&buffer);
    return value && sdp_ai_invoker::publishing(current_thd,std::string(value->ptr(),value->length()));
  }
  bool do_itemize(Parse_context *pc,Item **res) override {
    if (skip_itemize(res)) return false;
    if (Item_int_func::do_itemize(pc,res)) return true;
    pc->thd->lex->safe_to_cache_query=false;
    pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);return false;
  }
  table_map get_initial_pseudo_tables() const override { return RAND_TABLE_BIT; }
  const char *func_name() const override { return "stardataparadise_tool_publication_active"; }
  bool resolve_type(THD *thd) override {
    if (param_type_is_default(thd,0,1,MYSQL_TYPE_VARCHAR)) return true;
    set_data_type_bool();return false;
  }
};
class Item_func_sdp_tool_dispatch_active final : public Item_int_func {
 public:
  Item_func_sdp_tool_dispatch_active(const POS &pos,Item *call) : Item_int_func(pos,call) {}
  longlong val_int() override {
    assert(fixed);String buffer;String *value=args[0]->val_str(&buffer);
    return value && sdp_ai_invoker::dispatching(current_thd,std::string(value->ptr(),value->length()));
  }
  bool do_itemize(Parse_context *pc,Item **res) override {
    if (skip_itemize(res)) return false;
    if (Item_int_func::do_itemize(pc,res)) return true;
    pc->thd->lex->safe_to_cache_query=false;
    pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);return false;
  }
  table_map get_initial_pseudo_tables() const override { return RAND_TABLE_BIT; }
  const char *func_name() const override { return "stardataparadise_tool_dispatch_active"; }
  bool resolve_type(THD *thd) override {
    if (param_type_is_default(thd,0,1,MYSQL_TYPE_VARCHAR)) return true;
    set_data_type_bool();return false;
  }
};
#endif
