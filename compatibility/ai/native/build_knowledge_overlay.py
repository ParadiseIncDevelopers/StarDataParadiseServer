"""Compose Knowledge compiler/parser hunks without modifying a running build.

The baseline already contains the base SDP, AI and short SQL patches. This
script writes only the isolated out overlay and its reviewable patch artifact.
"""
from pathlib import Path
import difflib

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'out/mysql-migration/server-upstream-baseline'
OVERLAY = ROOT / 'out/ai-knowledge-overlay'
HERE = Path(__file__).resolve().parent
OVERLAY.mkdir(parents=True, exist_ok=True)


def replace_once(source, anchor, replacement):
    if source.count(anchor) != 1:
        raise RuntimeError('Missing or ambiguous Knowledge overlay anchor: '+anchor[:80])
    return source.replace(anchor, replacement)


def grammar(source):
    source=replace_once(source,'#include "sql/sdp_plsql_short.h"',
      '#include "sql/sdp_plsql_short.h"\n#include "sql/sdp_ai_knowledge.h"')
    source=replace_once(source,'%type <item> sdp_ai_options',
      '%type <lex_cstr> sdp_knowledge_operation\n%type <item_list2> sdp_knowledge_columns sdp_knowledge_opt_columns\n%type <item> sdp_knowledge_nullable\n%type <item> sdp_ai_options')
    source=replace_once(source,'%type <lex_cstr> sdp_model_procedure sdp_ai_kind sdp_ai_plural',
      '%type <lexer.lex_str> sdp_ai_function_name\n%type <lex_cstr> sdp_model_procedure sdp_ai_kind sdp_ai_plural')
    source=replace_once(source,'static PT_call *sdp_ai_call(',
      'static Parse_tree_root *sdp_ai_call(')
    source=replace_once(source,'  return new (thd->mem_root) PT_call(pos, name, args);',
      '  auto *call=new (thd->mem_root) PT_call(pos, name, args);\n  return call ? new (thd->mem_root) PT_sdp_ai_call(pos,call,args,operation) : nullptr;')
    source=replace_once(source,"        | ident '.' ident '(' opt_expr_list ')'",
      "        | sdp_ai_function_name '(' opt_udf_expr_list ')'\n          { $$= NEW_PTN PTI_function_call_generic_ident_sys(@$, $1, $3); }\n        | ident '.' ident '(' opt_expr_list ')'")
    names=('ai','conversation','conversations','knowledge','knowledges','behavior','behaviors',
      'response','models','model','provider','generative','append','prompt','functions',
      'message','persistent','ticket','run','async','cancel','memory','context','finish')
    forwarding='sdp_ai_function_name:\n'
    for index,name in enumerate(names):
        forwarding+=('          ' if index==0 else '        | ')+name.upper()+'_SYM { $$= {const_cast<char *>("'+name+'"),'+str(len(name))+'}; }\n'
    forwarding+='        ;\n\n'
    source=replace_once(source,'fulltext_options:\n',forwarding+'fulltext_options:\n')
    rule=r'''
          sdp_knowledge_operation KNOWLEDGE_SYM table_ident '(' sdp_knowledge_opt_columns ')'
          RETURNS_SYM TABLE_SYM '(' sdp_knowledge_columns ')' READ_SYM ONLY_SYM SQL_SYM SECURITY_SYM INVOKER_SYM AS query_expression
          {
            auto *payload= NEW_PTN PT_item_list(@$);
            auto *parameters= NEW_PTN Item_func_json_array(YYTHD,@5,$5);
            auto *results= NEW_PTN Item_func_json_array(YYTHD,@10,$10);
            if (!payload || !parameters || !results ||
                sdp_ai_pair(YYTHD,@$,payload,"definition",sdp_ai_text(YYTHD,@18,@18.raw.start,@18.raw.length())) ||
                sdp_ai_pair(YYTHD,@$,payload,"parameters",parameters) || sdp_ai_pair(YYTHD,@$,payload,"result_columns",results)) MYSQL_YYABORT;
            $$= sdp_ai_call(YYTHD,@$,$1.str,"knowledge",$3,sdp_ai_object(YYTHD,@$,payload));
            if (!$$) MYSQL_YYABORT;
          }
        | EXECUTE_SYM ident table_ident '(' opt_expr_list ')'
          {
            if (!is_identifier($2,"KNOWLEDGE") || !global_system_variables.sdp_ai_sql) {
              YYTHD->syntax_error_at(@2);MYSQL_YYABORT;
            }
            $$= NEW_PTN PT_sdp_execute_knowledge(@$,$3,$5);
            if (!$$) MYSQL_YYABORT;
          }
        | '''
    source=replace_once(source,'sdp_ai_stmt:\n          SHOW sdp_ai_plural',
      'sdp_ai_stmt:\n'+rule+'SHOW sdp_ai_plural')
    source=replace_once(source,'sdp_ai_kind:\n', (HERE/'knowledge-grammar.inc').read_text(encoding='utf-8')+'\nsdp_ai_kind:\n')
    return source


def factory(source):
    source=replace_once(source,'#include "sql/item_strfunc.h"',
      '#include "sql/item_strfunc.h"\n#include "sql/sdp_ai_knowledge.h"')
    return replace_once(source,'    {"STARDATAPARADISE_ACTOR",',
      '    {"STARDATAPARADISE_KNOWLEDGE_VALIDATE", SQL_FN(Item_func_sdp_knowledge_validate, 1)},\n    {"STARDATAPARADISE_ACTOR",')


patch=[]
for relative, transform in [('sql/sql_yacc.yy',grammar),('sql/item_create.cc',factory)]:
    original=(SOURCE/relative).read_text(encoding='utf-8')
    updated=transform(original)
    target=OVERLAY/relative;target.parent.mkdir(parents=True,exist_ok=True)
    target.write_text(updated,encoding='utf-8',newline='\n')
    patch.append(''.join(difflib.unified_diff(original.splitlines(True),updated.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative)))
header=(HERE/'sdp_ai_knowledge.h').read_text(encoding='utf-8')
(OVERLAY/'sql/sdp_ai_knowledge.h').write_text(header,encoding='utf-8',newline='\n')
patch.append(''.join(difflib.unified_diff([],header.splitlines(True),fromfile='/dev/null',tofile='b/sql/sdp_ai_knowledge.h')))
(OVERLAY/'knowledge-native.patch').write_text(''.join(patch),encoding='utf-8',newline='\n')
print('Knowledge overlay generated:',OVERLAY)
