// StarDataParadise native AI commit orchestration. Included by sql_parse.cc.
#ifndef SDP_AI_COMMIT_H
#define SDP_AI_COMMIT_H

#include <array>
#include <chrono>
#include <string>
#include <thread>
#include "my_rnd.h"
#include "scope_guard.h"
#include "sql/item_func.h"
#include "sql/sdp_ai_capture.h"
#include "sql/sdp_ai_catalog_guard.h"
#include "sql/sdp_ai_commit_scope.h"
#include "sql/sdp_ai_invoker.h"
#include "sql/sql_class.h"
#include "sql/sql_error.h"
#include "sql/statement/ed_connection.h"
#include "sql/transaction.h"

// This guard is native-only: a session variable cannot suppress commit hooks.
static thread_local bool sdp_ai_internal_hook = false;

static bool sdp_ai_scope_active(THD *thd) {
  if (sdp_ai_internal_hook || !global_system_variables.sdp_ai_sql) return false;
  mysql_mutex_lock(&thd->LOCK_thd_data);
  const auto found = thd->user_vars.find("sdp_ai_session");
  const bool active = found != thd->user_vars.end() && found->second->ptr() != nullptr;
  mysql_mutex_unlock(&thd->LOCK_thd_data);
  return active;
}

static bool sdp_ai_error(THD *thd, uint number, const char *state,
                         const std::string &message) {
  Diagnostics_area *area = thd->get_stmt_da();
  if (!area->is_error()) {
    area->set_error_status(number, message.c_str(), state);
    area->push_warning(thd, number, state, Sql_condition::SL_ERROR, message.c_str());
  }
  return true;
}

struct Sdp_ai_commit_packet {
  bool present = false;
  std::array<std::string,5> columns;
};

static std::string sdp_ai_hook_error(uint number,const char *routine,const char *original) {
  std::string raw=original ? original : "Native AI operation failed.";
  std::string code=std::to_string(number);
  if (raw.size()>6 && raw[6]==':' && raw.substr(0,6).find_first_not_of("0123456789")==std::string::npos)
    code=raw.substr(0,6);
  std::string escaped;
  const char hex[]="0123456789abcdef";
  for (unsigned char c : raw) {
    if (c=='"' || c=='\\') { escaped+='\\'; escaped+=c; }
    else if (c<32) { escaped+="\\u00"; escaped+=hex[c>>4]; escaped+=hex[c&15]; }
    else escaped+=c;
  }
  const bool before=strcmp(routine,"before_commit")==0;
  return code+": {\"message\":\""+escaped+"\",\"phase\":\""+
    (before ? "local_commit" : "after_commit")+"\",\"commit_state\":\""+
    (before ? "not_committed" : "committed")+"\",\"unknown_outcome\":false}";
}

// A failed catalog pin can already have installed a native diagnostic.
// Copy it before clearing only the status; preserve its errno/SQLSTATE and
// original text inside the phase-aware error. A pre-existing plaintext error
// must not prevent reporting that physical COMMIT has already completed.
static bool sdp_ai_catalog_error(THD *thd,const char *routine) {
  Diagnostics_area *area=thd->get_stmt_da();
  const uint number=area->is_error()?area->mysql_errno():61081;
  const std::string state=area->is_error()?area->returned_sqlstate():"42000";
  const std::string original=area->is_error()?area->message_text():
      "510001: Native AI catalog routine identity is not trusted.";
  const bool commit_hook=strcmp(routine,"before_commit")==0 ||
      strcmp(routine,"after_commit")==0 || strcmp(routine,"poll_commit")==0;
  const std::string message=commit_hook?
      sdp_ai_hook_error(number,routine,original.c_str()):original;
  thd->clear_error();
  return sdp_ai_error(thd,number,state.c_str(),message);
}

static bool sdp_ai_transaction_hook(THD *thd, const char *routine,
                                    Sdp_ai_commit_packet *packet = nullptr) {
  if (!sdp_ai_scope_active(thd)) return false;
  // Pin the complete private helper graph for this one local RPC. The guard
  // dies before after_commit's polling loop can wait on an external provider.
  sdp_ai::Catalog_guard catalog;
  if (catalog.pin(thd)) return sdp_ai_catalog_error(thd,routine);
  sdp_ai_invoker::Worker_rpc_scope native_rpc(thd);
  const bool previous = sdp_ai_internal_hook;
  sdp_ai_internal_hook = true;
  auto internal_guard = create_scope_guard([&]() { sdp_ai_internal_hook = previous; });
  // Ed_connection parses an independent internal CALL. An outer stored
  // procedure's runtime frame must not be reused for its result protocol or
  // local-variable offsets. The application frame remains alive and is
  // restored before returning to the caller.
  sp_rcontext *caller_context=thd->sp_runtime_ctx;
  thd->sp_runtime_ctx=nullptr;
  auto caller_guard=create_scope_guard([&]() {thd->sp_runtime_ctx=caller_context;});
  std::string source = "CALL sdp_ai."; source += routine; source += "()";
  Ed_connection connection(thd,true);
  LEX_STRING query{source.data(), source.size()};
  if (connection.execute_direct(query))
    return sdp_ai_error(thd, connection.get_last_errno(), connection.get_last_sqlstate(),
                        sdp_ai_hook_error(connection.get_last_errno(),routine,connection.get_last_error()));
  if (packet != nullptr) {
    Ed_result_set *result = connection.get_result_sets();
    if (result == nullptr || result->size() == 0) return false;
    if (result->get_field_count() != 5 || result->size() != 1)
      return sdp_ai_error(thd,61077,"HY000","509007: Invalid native commit response; commit_state=committed.");
    List<Ed_row> &rows = *result;
    const Ed_row *row = rows.head();
    for (uint i=0;i<5;++i) {
      const Ed_column &column = (*row)[i];
      if (column.length > 16*1024*1024)
        return sdp_ai_error(thd,61077,"HY000","509007: Invalid native commit response; commit_state=committed.");
      packet->columns[i].assign(column.str ? column.str : "",column.str ? column.length : 0);
    }
    packet->present = true;
  }
  return false;
}

static bool sdp_ai_before_commit(THD *thd) {
  if (!sdp_ai_scope_active(thd)) return false;
  unsigned char random[16];
  if (my_rand_buffer(random,sizeof(random))) {
    my_error(ER_OUT_OF_RESOURCES,MYF(0)); return true;
  }
  char name[43]="__sdp_ai_";
  static const char hex[]="0123456789abcdef";
  for (size_t i=0;i<sizeof(random);++i) {
    name[9+i*2]=hex[random[i]>>4]; name[10+i*2]=hex[random[i]&15];
  }
  name[41]='\0';
  LEX_STRING savepoint{name,41};
  const bool owned=!thd->in_multi_stmt_transaction_mode();
  if (owned && trans_begin(thd)) return true;
  if (trans_savepoint(thd,savepoint)) {
    if (owned) trans_rollback(thd);
    return true;
  }
  if (sdp_ai_transaction_hook(thd,"before_commit")) {
    if (owned) trans_rollback(thd);
    else {
      trans_rollback_to_savepoint(thd,savepoint);
      trans_release_savepoint(thd,savepoint);
    }
    return true;
  }
  return trans_release_savepoint(thd,savepoint);
}

static bool sdp_ai_commit_result(THD *thd,const std::string &document) {
  mysql_mutex_lock(&thd->LOCK_thd_data);
  auto found=thd->user_vars.find("sdp_ai_commit_result");
  const bool failed=found==thd->user_vars.end() ||
    found->second->store(document.data(),document.size(),STRING_RESULT,
                          &my_charset_utf8mb4_bin,DERIVATION_IMPLICIT,false);
  if (!failed) found->second->sdp_set_json();
  mysql_mutex_unlock(&thd->LOCK_thd_data);
  if (failed) return sdp_ai_error(thd,61077,"HY000","509007: Native result capture failed; commit_state=committed.");
  return false;
}

static bool sdp_ai_after_commit_impl(THD *thd,std::string *document) {
  if (!sdp_ai_scope_active(thd)) return false;
  // Every error/timeout path leaves unresolved captures SQL NULL. Completed
  // assignments reset the pending flag and survive this cleanup.
  auto capture_cleanup=create_scope_guard([&]() {sdp_ai_capture::clear_pending(thd);});
  // The caller's transaction is already committed. Every poll uses autocommit,
  // including callers that normally use autocommit=0. Restore that setting
  // before returning; never restore a completed transaction's BEGIN bit.
  constexpr ulonglong mask=OPTION_AUTOCOMMIT|OPTION_NOT_AUTOCOMMIT|OPTION_BEGIN;
  const ulonglong saved=thd->variables.option_bits;
  const uint saved_auto=thd->server_status & SERVER_STATUS_AUTOCOMMIT;
  const CHARSET_INFO *saved_results=thd->variables.character_set_results;
  thd->variables.option_bits=(saved & ~mask)|OPTION_AUTOCOMMIT;
  thd->server_status|=SERVER_STATUS_AUTOCOMMIT;
  thd->variables.character_set_results=&my_charset_utf8mb4_bin;
  auto restore=create_scope_guard([&]() {
    thd->variables.option_bits=(thd->variables.option_bits & ~mask)|(saved & (mask & ~OPTION_BEGIN));
    thd->server_status=(thd->server_status & ~SERVER_STATUS_AUTOCOMMIT)|saved_auto;
    thd->variables.character_set_results=saved_results;
  });
  Sdp_ai_commit_packet packet;
  if (sdp_ai_transaction_hook(thd,"after_commit",&packet)) return true;
  if (!packet.present) return false;
  if (packet.columns[1]!="async" && packet.columns[1]!="sync")
    return sdp_ai_error(thd,61077,"HY000","509007: Invalid native commit mode; commit_state=committed.");
  if (!packet.columns[0].empty() && (packet.columns[0].size()!=32 ||
      packet.columns[0].find_first_not_of("0123456789abcdef")!=std::string::npos))
    return sdp_ai_error(thd,61077,"HY000","509007: Invalid native run identity; commit_state=committed.");
  const bool synchronous=packet.columns[1]=="sync" && !packet.columns[0].empty();
  unsigned long wait_ms=30000;
  if (synchronous && !packet.columns[2].empty()) {
    char *end=nullptr;
    wait_ms=strtoul(packet.columns[2].c_str(),&end,10);
    if (end==nullptr || *end!='\0' || wait_ms<1 || wait_ms>3600000)
      return sdp_ai_error(thd,61077,"HY000","509007: Invalid native wait settings; commit_state=committed.");
  }
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(wait_ms);
  for (;;) {
    const std::string &state=packet.columns[3];
    const bool terminal=state=="succeeded" || state=="refused" || state=="failed" || state=="cancelled";
    if (!synchronous || terminal) {
      *document=packet.columns[4];
      if (sdp_ai_capture::publish(thd,*document,synchronous)) return true;
      return sdp_ai_commit_result(thd,*document);
    }
    // The internal CALL's standard statement cleanup must have released its
    // transactional context and metadata locks before any wait begins.
    if (thd->in_active_multi_stmt_transaction())
      return sdp_ai_error(thd,61077,"HY000","509007: Native wait retained a transaction; commit_state=committed.");
    if (thd->killed || std::chrono::steady_clock::now()>=deadline) {
      sdp_ai_capture::clear_pending(thd);
      sdp_ai_commit_result(thd,packet.columns[4]);
      const std::string message="511003: {\"commit_state\":\"committed\",\"run_state\":\""+
        (state=="running" ? std::string("running") : std::string("queued"))+
        "\",\"run_id\":\""+packet.columns[0]+"\",\"retry\":{\"action\":\"QUERY_RUN\"}}";
      return sdp_ai_error(thd,61093,"HYT00",message);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    packet=Sdp_ai_commit_packet();
    if (sdp_ai_transaction_hook(thd,"poll_commit",&packet)) return true;
    if (!packet.present)
      return sdp_ai_error(thd,61077,"HY000","509007: Missing native run status; commit_state=committed.");
  }
}

static bool sdp_ai_after_commit(THD *thd,std::string *document) {
  // Physical COMMIT has completed. Keep pre-commit failure handlers pinned,
  // then release before any after-commit provider polling can begin.
  sdp_ai::Commit_catalog_scope::release_for(thd);
  const bool failed=sdp_ai_after_commit_impl(thd,document);
  // Provider waiting holds no borrowed dispatcher pins. Revalidate before its
  // stored-code continuation or error handler can invoke another helper.
  if (sdp_ai::Commit_catalog_scope::resume_for(thd))
    return sdp_ai_catalog_error(thd,"after_commit");
  return failed;
}

static bool sdp_ai_send_commit_result(THD *thd,const std::string &document) {
  mem_root_deque<Item *> fields(thd->mem_root);
  auto *column=new (thd->mem_root) Item_empty_string("document",document.size(),&my_charset_utf8mb4_bin);
  if (column==nullptr) return true;
  column->set_data_type_json();
  fields.push_back(column);
  if (thd->send_result_metadata(fields,Protocol::SEND_NUM_ROWS|Protocol::SEND_EOF)) return true;
  Protocol *protocol=thd->get_protocol();
  protocol->start_row();
  if (protocol->store_string(document.data(),document.size(),&my_charset_utf8mb4_bin) || protocol->end_row()) return true;
  my_eof(thd);
  return false;
}
#endif
