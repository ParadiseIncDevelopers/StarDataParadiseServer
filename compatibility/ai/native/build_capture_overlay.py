"""Compose native AI capture changes in an isolated tree while a build is frozen."""
from pathlib import Path
import argparse
import shutil


def replace(path, old, new):
    text = path.read_text(encoding='utf-8')
    if text.count(old) != 1: raise RuntimeError('capture anchor mismatch: ' + str(path) + ': ' + old[:70])
    path.write_text(text.replace(old, new), encoding='utf-8', newline='\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    target = args.output.resolve()
    if target.exists(): raise RuntimeError('output must be fresh')
    (target/'sql').mkdir(parents=True)
    files = ('item_func.h', 'item_func.cc', 'sql_call.h', 'sql_call.cc', 'sdp_ai_knowledge.h', 'sdp_ai_commit.h', 'sql_parse.cc')
    for file in files: shutil.copy2(args.source/'sql'/file, target/'sql'/file)
    shutil.copy2(Path(__file__).with_name('sdp_ai_capture.h'), target/'sql'/'sdp_ai_capture.h')
    header = target/'sql'/'item_func.h'
    replace(header, '  THD *m_owner;\n', '  THD *m_owner;\n  bool m_sdp_json;\n  bool m_sdp_capture_pending;\n')
    replace(header, '  void set_null_value(Item_result type) {\n    assert_locked();',
            '  void set_null_value(Item_result type) {\n    assert_locked();\n    m_sdp_json=false;\n    m_sdp_capture_pending=false;')
    replace(header, '  THD *owner_session() const { return m_owner; }', '''  THD *owner_session() const { return m_owner; }
  bool sdp_json() const { return m_sdp_json; }
  bool sdp_capture_pending() const { return m_sdp_capture_pending; }
  void sdp_reserve_capture() {
    set_null_value(STRING_RESULT);
    m_sdp_capture_pending=true;
  }
  void sdp_set_json() { assert_locked(); m_sdp_json=true; }
''')
    replace(header, '  Item_result m_cached_result_type;\n', '''  Item_result m_cached_result_type;
  bool m_sdp_json=false;
  bool sdp_check_capture(THD *thd);
''')
    # The signature occurs once in the getter's exact surrounding context.
    replace(header, '''  String *val_str(String *str) override;
  const CHARSET_INFO *charset_for_protocol() override;''', '''  String *val_str(String *str) override;
  bool val_json(Json_wrapper *result) override;
  const CHARSET_INFO *charset_for_protocol() override;''')
    implementation = target/'sql'/'item_func.cc'
    replace(implementation, '  m_owner = thd;\n', '  m_owner = thd;\n  m_sdp_json=false;\n  m_sdp_capture_pending=false;\n')
    replace(implementation, '''bool user_var_entry::store(const void *from, size_t length, Item_result type) {
  assert_locked();''', '''bool user_var_entry::store(const void *from, size_t length, Item_result type) {
  assert_locked();
  m_sdp_json=false;
  m_sdp_capture_pending=false;''')
    replace(implementation, 'String *Item_func_get_user_var::val_str(String *str) {', '''bool Item_func_get_user_var::sdp_check_capture(THD *thd) {
  if (var_entry->sdp_capture_pending()) {
    auto *area=thd->get_stmt_da();
    const char *message="504005: Response capture is pending; commit the turn before reading its value.";
    if (!area->is_error()) {
      area->set_error_status(61025,message,"55000");
      area->push_warning(thd,61025,"55000",Sql_condition::SL_ERROR,message);
    }
    null_value=true;
    return true;
  }
  if (var_entry->sdp_json()!=m_sdp_json) {
    // A prepared @variable may have been prepared before a native JSON
    // capture, or overwritten by ordinary SET since then. Refresh metadata.
    if (ask_to_reprepare(thd)) return true;
    auto *area=thd->get_stmt_da();
    const char *message="504006: Response capture type changed during statement execution.";
    if (!area->is_error()) area->set_error_status(61026,message,"22000");
    null_value=true;
    return true;
  }
  return false;
}

bool Item_func_get_user_var::val_json(Json_wrapper *result) {
  THD *thd=current_thd;
  if (var_entry==nullptr && get_var_with_binlog(thd,thd->lex->sql_command,name,&var_entry)) return true;
  if (var_entry==nullptr) {null_value=true;return false;}
  if (sdp_check_capture(thd)) return true;
  null_value=var_entry->ptr()==nullptr;
  if (null_value) return false;
  if (!var_entry->sdp_json()) return Item::val_json(result);
  auto dom=Json_dom::parse(var_entry->ptr(),var_entry->length(),
      [&](const char *,size_t) {my_error(ER_INVALID_JSON_TEXT,MYF(0),1,func_name(),"invalid capture",0);},
      [&]() {my_error(ER_JSON_DOCUMENT_TOO_DEEP,MYF(0));});
  if (!dom) return true;
  *result=Json_wrapper(std::move(dom));
  return false;
}

String *Item_func_get_user_var::val_str(String *str) {''')
    replace(implementation, '  if (var_entry == nullptr) return error_str();  // No such variable\n',
            '  if (var_entry == nullptr) return error_str();  // No such variable\n  if (sdp_check_capture(thd)) return error_str();\n')
    for returned in ('0.0', 'nullptr', '0LL'):
        anchor = '  if (var_entry == nullptr) return '+returned+';'+('' if returned=='nullptr' else '  // No such variable')+'\n'
        replace(implementation, anchor, anchor+'  if (sdp_check_capture(current_thd)) return '+returned+';\n')
    replace(implementation, '''    m_cached_result_type = var_entry->type();

    switch''', '''    m_cached_result_type = var_entry->type();
    m_sdp_json=var_entry->sdp_json();
    if (sdp_check_capture(thd)) return true;

    switch''')
    replace(implementation, '''    // Override collation for all data types
    collation.set(var_entry->collation);''', '''    // Override collation for all data types
    collation.set(var_entry->collation);
    if (m_sdp_json) set_data_type_json();''')
    call_header = target/'sql'/'sql_call.h'
    replace(call_header, '  enum_sql_command sql_command_code() const override { return SQLCOM_CALL; }',
            '  void sdp_enable_capture() { m_sdp_capture=true; }\n\n  enum_sql_command sql_command_code() const override { return SQLCOM_CALL; }')
    replace(call_header, '  sp_name *proc_name;\n', '  bool m_sdp_capture=false;\n  sp_name *proc_name;\n')
    call_impl = target/'sql'/'sql_call.cc'
    replace(call_impl, '#include "sql/sql_call.h"', '#include "sql/sql_call.h"\n#include "sql/sdp_ai_capture.h"')
    replace(call_impl, '  my_ok(thd, max(thd->get_row_count_func(), 0LL));',
            '  if (m_sdp_capture && sdp_ai_capture::register_pending(thd)) return true;\n\n  my_ok(thd, max(thd->get_row_count_func(), 0LL));')
    knowledge = target/'sql'/'sdp_ai_knowledge.h'
    replace(knowledge, '#include "sql/sql_cmd_dml.h"', '#include "sql/sql_cmd_dml.h"\n#include "sql/sql_call.h"')
    replace(knowledge, '''    return command;
  }
};
#endif''', '''    static_cast<Sql_cmd_call *>(command)->sdp_enable_capture();
    return command;
  }
};
#endif''')
    commit = target/'sql'/'sdp_ai_commit.h'
    replace(commit, '#include "sql/item_func.h"', '#include "sql/item_func.h"\n#include "sql/sdp_ai_capture.h"')
    replace(commit, '''      *document=packet.columns[4];
      return sdp_ai_commit_result(thd,*document);''', '''      *document=packet.columns[4];
      if (sdp_ai_capture::publish(thd,*document,synchronous)) return true;
      return sdp_ai_commit_result(thd,*document);''')
    replace(commit, '''      sdp_ai_commit_result(thd,packet.columns[4]);
      const std::string message''', '''      sdp_ai_capture::clear_pending(thd);
      sdp_ai_commit_result(thd,packet.columns[4]);
      const std::string message''')
    parse = target/'sql'/'sql_parse.cc'
    replace(parse, '      if (sdp_ai_transaction_hook(thd, "rollback_turn")) goto error;',
            '      sdp_ai_capture::clear_pending(thd);\n      if (sdp_ai_transaction_hook(thd, "rollback_turn")) goto error;')
    print('Native capture overlay composed: '+str(target))


if __name__ == '__main__': main()
