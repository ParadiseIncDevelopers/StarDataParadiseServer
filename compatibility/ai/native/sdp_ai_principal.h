// Native authenticated/logical identity and active-role proof. Never sourced
// from SQL variables or from the private catalog routine's DEFINER context.
#ifndef SQL_SDP_AI_PRINCIPAL_H
#define SQL_SDP_AI_PRINCIPAL_H
#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include "sql/sdp_ai_invoker.h"
#include "sql/sdp_ai_knowledge.h"
#include "sql/auth/auth_internal.h"
#include "sql/auth/sql_auth_cache.h"
#include "sql/auth/sql_authorization.h"
#include "sql/item_json_func.h"

extern Role_index_map *g_authid_to_vertex;
namespace sdp_ai_principal {
inline bool denied(THD *thd) {
  return sdp_knowledge::error(thd,61081,"42000","510001: Current active-role authorization is unavailable or has changed.");
}
inline std::string account(LEX_CSTRING user,LEX_CSTRING host) {
  std::string out;
  const auto append=[&](LEX_CSTRING part) {
    for (size_t i=0;i<part.length;++i) {if (part.str[i]=='@' || part.str[i]=='\\') out+='\\';out+=part.str[i];}
  };
  append(user);out+='@';append(host);return out;
}
inline std::string account(Security_context *context) {return account(context->priv_user(),context->priv_host());}
// Native accounts contain exactly one unescaped '@'. This separate namespace
// cannot be forged by an authenticated account with '@' in its username.
inline std::string role_account(LEX_CSTRING user,LEX_CSTRING host) {return "role@"+account(user,host);}
inline std::pair<std::string,std::string> canonical(const ACL_USER *value) {
  return {value->user?value->user:"",value->host.get_host()};
}
inline std::string role_account(const ACL_USER *value) {
  const auto id=canonical(value);return role_account({id.first.c_str(),id.first.size()},{id.second.c_str(),id.second.size()});
}
inline bool text(const Json_object *object,const char *key,std::string *result,size_t maximum) {
  const Json_dom *value=object->get(key);
  if (!value || value->json_type()!=enum_json_type::J_STRING) return true;
  *result=static_cast<const Json_string *>(value)->value();
  return result->size()>maximum || result->find('\0')!=std::string::npos;
}

// The array must come from a protected run/action record. Each root is
// validated against the CURRENT native role graph, never blindly activated.
inline bool activate(THD *thd,Security_context *target,const Json_dom *sealed_roles) {
  if (!target || target->is_skip_grants_user() || !sealed_roles ||
      sealed_roles->json_type()!=enum_json_type::J_ARRAY) return denied(thd);
  const auto *roles=static_cast<const Json_array *>(sealed_roles);
  if (roles->size()>64) return denied(thd);
  Acl_cache_lock_guard lock(thd,Acl_cache_lock_mode::READ_MODE);
  if (!lock.lock()) return true;
  ACL_USER *base=find_acl_user(target->priv_host().str,target->priv_user().str,true);
  if (!base || !acl_restrictions) return denied(thd);
  target->clear_active_roles();
  // acl_getroot() automatically activates DEFAULT roles. Reset their global
  // bits and partial-revoke restrictions before applying the sealed roots;
  // checkout_access_maps() alone leaves master_access untouched for [] roots.
  target->set_master_access(base->access,acl_restrictions->find_restrictions(base));
  for (size_t i=0;i<roles->size();++i) {
    const Json_dom *value=(*roles)[i];std::string user,host;
    if (!value || value->json_type()!=enum_json_type::J_OBJECT) {target->clear_active_roles();return denied(thd);}
    const auto *role=static_cast<const Json_object *>(value);
    if (role->cardinality()!=2 || text(role,"user",&user,USERNAME_LENGTH) || text(role,"host",&host,HOSTNAME_LENGTH) ||
        target->activate_role({user.c_str(),user.size()},{host.c_str(),host.size()},true)) {
      target->clear_active_roles();return denied(thd);
    }
  }
  target->checkout_access_maps();
  const auto db=thd->db();
  target->cache_current_db_access(Security_context::check_db_level_access(thd,target,
    target->host().str,target->ip().str,target->priv_user().str,db.str,db.length,false));return false;
}

inline Json_dom_ptr snapshot(THD *thd,Security_context *owner) {
  if (!owner || owner->is_skip_grants_user()) {denied(thd);return {};}
  auto result=create_dom_ptr<Json_object>();auto roots=create_dom_ptr<Json_array>();auto principals=create_dom_ptr<Json_array>();
  if (!result || !roots || !principals) return {};
  std::set<std::string> names;names.insert(account(owner));
  std::vector<std::pair<std::string,std::string>> pending;
  Acl_cache_lock_guard lock(thd,Acl_cache_lock_mode::READ_MODE);
  if (!lock.lock()) return {};
  if (owner->get_num_active_roles()>64) {denied(thd);return {};}
  for (const auto &role:*owner->get_active_roles()) {
    // An old session's root list is not evidence that a revoked role remains
    // activatable. Revalidate before exposing its protected catalog grants.
    if (!check_if_granted_role(owner->priv_user(),owner->priv_host(),role.first,role.second)) {denied(thd);return {};}
    auto entry=create_dom_ptr<Json_object>();
    ACL_USER *native=find_acl_user(role.second.str,role.first.str,true);
    if (!native) {denied(thd);return {};}
    const auto normalized=canonical(native);const std::string &user=normalized.first,&host=normalized.second;
    if (!entry || entry->add_alias("user",create_dom_ptr<Json_string>(user)) ||
        entry->add_alias("host",create_dom_ptr<Json_string>(host)) || roots->append_alias(std::move(entry))) return {};
    pending.emplace_back(user,host);
  }
  // Traverse only ACTIVE roots; starting at owner would activate every
  // assigned but inactive role. The native ACL cache read lock pins the DAG.
  for (size_t i=0;i<pending.size();++i) {
    if (pending.size()>1024) {denied(thd);return {};}
    const auto role=pending[i];Auth_id_ref id{{role.first.c_str(),role.first.size()},{role.second.c_str(),role.second.size()}};
    if (!names.insert(role_account(id.first,id.second)).second) continue;
    const auto vertex=g_authid_to_vertex->find(create_authid_str_from(id));
    if (vertex==g_authid_to_vertex->end()) {denied(thd);return {};}
    auto key=vertex->second;
    get_granted_roles(key,[&](const Role_id &child,bool) {pending.emplace_back(child.user(),child.host());});
  }
  for (const auto &name:names) if (principals->append_alias(create_dom_ptr<Json_string>(name))) return {};
  if (result->add_alias("actor",create_dom_ptr<Json_string>(account(owner))) ||
      result->add_alias("roles",std::move(roots)) || result->add_alias("principals",std::move(principals))) return {};
  return result;
}
inline Json_dom_ptr snapshot(THD *thd) {return snapshot(thd,sdp_ai_invoker::logical_context(thd));}
} // namespace sdp_ai_principal

class Item_func_sdp_principals final : public Item_json_func {
 public:
  explicit Item_func_sdp_principals(const POS &pos) : Item_json_func(current_thd,pos) {}
  const char *func_name() const override {return "stardataparadise_principals";}
  table_map get_initial_pseudo_tables() const override {return RAND_TABLE_BIT;}
  bool do_itemize(Parse_context *pc,Item **res) override {
    if (skip_itemize(res)) return false;
    if (Item_json_func::do_itemize(pc,res)) return true;
    pc->thd->lex->safe_to_cache_query=false;
    pc->thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);return false;
  }
  bool val_json(Json_wrapper *result) override {
    assert(fixed);auto value=sdp_ai_principal::snapshot(current_thd);if (!value) return true;
    *result=Json_wrapper(std::move(value));null_value=false;return false;
  }
};
#endif
