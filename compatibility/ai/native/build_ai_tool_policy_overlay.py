"""Stage runtime tool atomicity hooks; never edit the in-flight native build."""
from pathlib import Path
import difflib
import shutil

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
SOURCE=ROOT/'out/mysql-migration/server-upstream-baseline'
OUT=ROOT/'out/ai-tool-policy-next'
(OUT/'sql').mkdir(parents=True,exist_ok=True)


def once(source,old,new):
    if source.count(old)!=1:raise RuntimeError('Tool policy anchor: '+old[:100])
    return source.replace(old,new)


def parser(source):
    source=once(source,'#include "sql/sdp_ai_commit.h"','#include "sql/sdp_ai_commit.h"\n#include "sql/sdp_ai_tool_policy.h"')
    return once(source,'  LEX *const lex = thd->lex;\n  /* first Query_block',
        '  LEX *const lex = thd->lex;\n  if (sdp_ai_tool_policy::statement(thd)) return 1;\n  /* first Query_block')


def handler(source):
    source=once(source,'#include "sql/handler.h"','#include "sql/handler.h"\n#include "sql/sdp_ai_tool_policy.h"')
    for name in ('ha_write_row','ha_update_row','ha_delete_row'):
        start=source.index('int handler::'+name+'(')
        end=source.find('\nint handler::',start+1)
        if end<0:raise RuntimeError('Missing next native handler method after '+name)
        section=source[start:end]
        section=once(section,'  mark_trx_read_write();',
            '  if (sdp_ai_tool_policy::write(ha_thd(), table)) return HA_ERR_INTERNAL_ERROR;\n  mark_trx_read_write();')
        source=source[:start]+section+source[end:]
    return source


def instruction_header(source):
    for name,end in [('sp_instr_set_trigger_field','class sp_instr_freturn'),('sp_lex_branch_instr','class sp_instr_jump_if_not')]:
        start=source.index('class '+name+' ');stop=source.index(end,start)
        part=source[start:stop]
        if ' public:\n' not in part:raise RuntimeError('Missing instruction public section')
        part=part.replace(' public:\n',' public:\n  Item *sdp_tool_expression() const { return m_'+('value' if name=='sp_instr_set_trigger_field' else 'expr')+'_item; }\n',1)
        source=source[:start]+part+source[stop:]
    return source


def instruction(source):
    source=once(source,'#include "sql/sp_instr.h"','#include "sql/sp_instr.h"\n#include "sql/sdp_ai_tool_policy.h"')
    signature='bool sp_lex_instr::reset_lex_and_exec_core(THD *thd, uint *nextp,\n                                           bool open_tables) {'
    return once(source,signature,signature+'\n  if (sdp_ai_tool_policy::instruction(thd,this)) return true;')


def signal_header(source):
    return once(source,'class Sql_cmd_common_signal : public Sql_cmd {\n protected:',
      'class Sql_cmd_common_signal : public Sql_cmd {\n public:\n  const Set_signal_information *sdp_tool_information() const { return m_set_signal_information; }\n protected:')


def trigger_owner(source):
    source=once(source,'#include "sql/sp_head.h"','#include "sql/sp_head.h"\n#include "sql/sdp_ai_invoker.h"\n#include "scope_guard.h"\n#include "sql/auth/sql_auth_cache.h"')
    begin=source.index('bool sp_head::execute_trigger(')
    end=source.index('\nexternal_program_handle ',begin)
    section=source[begin:end]
    section=once(section,'  /*\n    Optimizer trace note:',
      '  // Model tools keep the ordinary trigger-definer existence/TRIGGER\n'
      '  // check above, then execute application expressions with the verified\n'
      '  // logical owner ACL. Product metadata identity never enters the body.\n'
      '  const bool tool_trigger = sdp_ai_invoker::active(thd);\n'
      '  const GRANT_INFO definer_grant = *grant_info;\n'
      '  auto restore_trigger_grant = create_scope_guard([&]() {\n'
      '    if (tool_trigger) *grant_info = definer_grant;\n'
      '  });\n'
      '  if (tool_trigger) {\n'
      '    m_security_ctx.restore_security_context(thd,save_ctx);save_ctx=nullptr;\n'
      '    thd->set_security_context(sdp_ai_invoker::logical_context(thd));\n'
      '    Acl_cache_lock_guard owner_acl(thd, Acl_cache_lock_mode::READ_MODE);\n'
      '    if (!owner_acl.lock()) return true;\n'
      '    // The native cache key includes its identity only implicitly. An unchanged\n'
      '    // grant version cannot validate it after switching definer to owner.\n'
      '    // Force a normal owner lookup, including its current active-role ACLs.\n'
      '    *grant_info = GRANT_INFO();\n'
      '    grant_info->version = grant_version - 1U;\n'
      '    fill_effective_table_privileges(thd,grant_info,db_name.str,table_name.str);\n'
      '  }\n  /*\n    Optimizer trace note:')
    return source[:begin]+section+source[end:]


patch=[]
for name,transform in [('sql_parse.cc',parser),('handler.cc',handler),('sp_instr.h',instruction_header),('sp_instr.cc',instruction),('sql_signal.h',signal_header),('sp_head.cc',trigger_owner)]:
    original=(SOURCE/'sql'/name).read_text(encoding='utf-8');updated=transform(original)
    (OUT/'sql'/name).write_text(updated,encoding='utf-8',newline='\n')
    patch.append(''.join(difflib.unified_diff(original.splitlines(True),updated.splitlines(True),fromfile='a/sql/'+name,tofile='b/sql/'+name)))
for name in ['sdp_ai_knowledge.h','sdp_ai_tool_policy.h','sdp_ai_invoker.h','sdp_ai_catalog_guard.h','sdp_routine_guard.h']:
    shutil.copyfile(HERE/name,OUT/'sql'/name)
(OUT/'ai-tool-policy-next.patch').write_text(''.join(patch),encoding='utf-8',newline='\n')
print('Isolated tool atomicity hooks:',OUT)
