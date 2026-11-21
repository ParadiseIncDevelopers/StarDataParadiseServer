// Native session response capture. No SQL interpolation or delayed assignment.
#ifndef SQL_SDP_AI_CAPTURE_H
#define SQL_SDP_AI_CAPTURE_H
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include "scope_guard.h"
#include "sql/item_func.h"
#include "sql/sql_class.h"
#include "sql/sql_error.h"
#include "sql-common/json_dom.h"

namespace sdp_ai_capture {
inline bool error(THD *thd,uint number,const char *state,const std::string &message) {
  auto *area=thd->get_stmt_da();
  if (!area->is_error()) {
    area->set_error_status(number,message.c_str(),state);
    area->push_warning(thd,number,state,Sql_condition::SL_ERROR,message.c_str());
  }
  return true;
}
inline bool shape_error(THD *thd) {
  return error(thd,61026,"22000","504006: Response capture type is incompatible.");
}
inline const Json_dom *member(const Json_dom *document,const char *name) {
  return document && document->json_type()==enum_json_type::J_OBJECT
    ? static_cast<const Json_object *>(document)->get(name) : nullptr;
}
inline std::string string(const Json_dom *document,const char *name) {
  const auto *value=member(document,name);
  return value && value->json_type()==enum_json_type::J_STRING ? static_cast<const Json_string *>(value)->value() : "";
}
inline Json_dom_ptr parse(THD *thd,const std::string &source) {
  return Json_dom::parse(source.data(),source.size(),
      [&](const char *,size_t) {shape_error(thd);},[&]() {shape_error(thd);});
}
inline bool encode(const Json_dom *value,std::string *output) {
  if (!value) return true;
  Json_wrapper wrapper(const_cast<Json_dom *>(value),true);
  String buffer;
  if (wrapper.to_string(&buffer,true,"AI capture",JsonDepthErrorHandler)) return true;
  output->assign(buffer.ptr(),buffer.length());
  return false;
}
inline std::string variable(THD *thd,const char *name) {
  mysql_mutex_lock(&thd->LOCK_thd_data);
  const auto found=thd->user_vars.find(name);
  std::string result;
  if (found!=thd->user_vars.end() && found->second->ptr()!=nullptr)
    result.assign(found->second->ptr(),found->second->length());
  mysql_mutex_unlock(&thd->LOCK_thd_data);
  return result;
}
inline void clear_pending(THD *thd) {
  mysql_mutex_lock(&thd->LOCK_thd_data);
  for (auto &entry:thd->user_vars)
    if (entry.second->sdp_capture_pending()) entry.second->set_null_value(STRING_RESULT);
  mysql_mutex_unlock(&thd->LOCK_thd_data);
}
struct Targets {std::string value,metadata,format;};
inline bool target(THD *thd,const Json_dom *capture,const char *key,std::string *output) {
  const auto *value=member(capture,key);
  if (!value || value->json_type()==enum_json_type::J_NULL) return false;
  if (string(value,"kind")!="user_variable") return shape_error(thd);
  *output=string(value,"name");
  std::string lower=*output;
  std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c) {return std::tolower(c);});
  if (output->empty() || output->size()>64 || lower.rfind("sdp_",0)==0 ||
      std::any_of(output->begin(),output->end(),[](unsigned char c) {return c==0 || c<32;})) return shape_error(thd);
  return false;
}
inline bool targets(THD *thd,const Json_dom *capture,Targets *output) {
  if (!capture || capture->json_type()==enum_json_type::J_NULL) return false;
  if (capture->json_type()!=enum_json_type::J_OBJECT ||
      target(thd,capture,"value_target",&output->value) || target(thd,capture,"metadata_target",&output->metadata)) return true;
  output->format=string(capture,"format");
  if (!output->format.empty() && output->format!="JSON" && output->format!="TEXT") return shape_error(thd);
  if (!output->value.empty() && !output->metadata.empty() &&
      !my_strcasecmp(system_charset_info,output->value.c_str(),output->metadata.c_str())) return shape_error(thd);
  return false;
}
inline void destroy_variable(user_var_entry *entry) {entry->destroy();}
// Called with LOCK_thd_data held. The session's map uses the same name collation
// and ownership conventions as get_variable in item_func.cc.
inline user_var_entry *entry(THD *thd,const std::string &name) {
  const auto found=thd->user_vars.find(name);
  if (found!=thd->user_vars.end()) return found->second.get();
  auto *value=user_var_entry::create(thd,Name_string(name.c_str(),name.size()),&my_charset_utf8mb4_bin);
  if (!value) return nullptr;
  thd->user_vars.emplace(name,unique_ptr_with_deleter<user_var_entry>(value,&destroy_variable));
  return value;
}
inline bool register_pending(THD *thd) {
  const std::string source=variable(thd,"sdp_ai_session");
  if (source.empty()) return false;
  auto document=parse(thd,source);
  if (!document) return true;
  Targets names;
  if (targets(thd,member(document.get(),"capture"),&names)) return true;
  if (names.value.empty() && names.metadata.empty()) return false;
  mysql_mutex_lock(&thd->LOCK_thd_data);
  auto unlock=create_scope_guard([&]() {mysql_mutex_unlock(&thd->LOCK_thd_data);});
  std::vector<user_var_entry *> reserved;
  for (const auto &name:{names.value,names.metadata}) if (!name.empty()) {
    auto *value=entry(thd,name);
    if (!value) return true;
    reserved.push_back(value);
  }
  // A second response request can replace its capture targets. The discarded
  // targets become SQL NULL; they cannot retain a result from an older turn.
  for (auto &value:thd->user_vars)
    if (value.second->sdp_capture_pending() && std::find(reserved.begin(),reserved.end(),value.second.get())==reserved.end())
      value.second->set_null_value(STRING_RESULT);
  for (auto *value:reserved) if (!value->sdp_capture_pending()) value->sdp_reserve_capture();
  return false;
}
inline bool assign(THD *thd,const std::string &name,const std::string &value,bool json) {
  if (name.empty()) return false;
  mysql_mutex_lock(&thd->LOCK_thd_data);
  auto unlock=create_scope_guard([&]() {mysql_mutex_unlock(&thd->LOCK_thd_data);});
  auto *target=entry(thd,name);
  if (!target || target->store(value.data(),value.size(),STRING_RESULT,&my_charset_utf8mb4_bin,DERIVATION_IMPLICIT,false)) return true;
  if (json) target->sdp_set_json();
  return false;
}
inline bool publish(THD *thd,const std::string &source,bool synchronous) {
  clear_pending(thd);
  auto document=parse(thd,source);
  if (!document) return true;
  const std::string state=string(document.get(),"state");
  Targets names;
  if (targets(thd,member(document.get(),"capture"),&names)) return true;
  if (!synchronous && (!names.value.empty() || !names.metadata.empty())) return shape_error(thd);
  if (state=="failed" || state=="cancelled") {
    const auto *failure=member(document.get(),"error");
    std::string diagnostic;
    if (!failure || failure->json_type()!=enum_json_type::J_OBJECT || encode(failure,&diagnostic)) return shape_error(thd);
    const auto *number=member(failure,"classic_errno"),*code=member(failure,"code");
    if (!number || !code || number->json_type()!=enum_json_type::J_INT || code->json_type()!=enum_json_type::J_INT) return shape_error(thd);
    const longlong classic=static_cast<const Json_int *>(number)->value(),public_code=static_cast<const Json_int *>(code)->value();
    const std::string sqlstate=string(failure,"sqlstate");
    if (classic<1 || classic>65535 || public_code<100000 || public_code>999999 || sqlstate.size()!=5) return shape_error(thd);
    return error(thd,static_cast<uint>(classic),sqlstate.c_str(),std::to_string(public_code)+": "+diagnostic);
  }
  if (!synchronous || (state!="succeeded" && state!="refused")) return false;
  std::string value,metadata;
  if (state=="succeeded" && !names.value.empty()) {
    const auto *response=member(document.get(),"result");
    const auto *content=member(response,"value");
    const std::string format=string(response,"format");
    if (!content || (format!="TEXT" && format!="JSON") || (!names.format.empty() && names.format!=format)) return shape_error(thd);
    if (format=="TEXT") {
      if (content->json_type()!=enum_json_type::J_STRING) return shape_error(thd);
      value=static_cast<const Json_string *>(content)->value();
    } else if (encode(content,&value)) return shape_error(thd);
    if (assign(thd,names.value,value,format=="JSON")) {clear_pending(thd);return true;}
  }
  if (!names.metadata.empty()) {
    if (encode(member(document.get(),"metadata"),&metadata) || assign(thd,names.metadata,metadata,true)) {
      // Never leave just the VALUE assigned if METADATA allocation fails.
      mysql_mutex_lock(&thd->LOCK_thd_data);
      for (const auto &name:{names.value,names.metadata}) if (!name.empty()) {
        const auto found=thd->user_vars.find(name);
        if (found!=thd->user_vars.end()) found->second->set_null_value(STRING_RESULT);
      }
      mysql_mutex_unlock(&thd->LOCK_thd_data);
      return true;
    }
  }
  return false;
}
}
#endif
