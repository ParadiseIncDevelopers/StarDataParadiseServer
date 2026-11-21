"""Compose next AI native changes in out; never mutate an active native build."""
from pathlib import Path
import difflib
import shutil

ROOT=Path(__file__).resolve().parents[3]
SOURCE=ROOT/'out/mysql-migration/server-upstream-baseline'
BASELINE=ROOT/'out/ai-family-next-baseline'
OVERLAY=ROOT/'out/ai-family-next'
HERE=Path(__file__).resolve().parent


def once(source,old,new):
    if source.count(old)!=1:
        raise RuntimeError('Missing/ambiguous next-AI anchor: '+old[:100])
    return source.replace(old,new)


def grammar(source):
    source=once(source,'#include "sql/sdp_ai_knowledge.h"',
      '#include "sql/sdp_ai_knowledge.h"\n#include "sql/sdp_ai_family_next.h"')
    source=once(source,'%token<lexer.keyword> CANCEL_SYM 1321',
      '%token<lexer.keyword> CANCEL_SYM 1321\n%token<lexer.keyword> METADATA_SYM 1322')
    source=once(source,'%type <item> sdp_ai_options',
      '%type <item> sdp_ai_response_format sdp_ai_response_format_opt sdp_ai_response_output sdp_ai_response_metadata sdp_ai_memory_lifetime sdp_ai_response_request\n%type <item> sdp_ai_options')
    source=once(source,'        | AI_SYM\n','        | AI_SYM\n        | METADATA_SYM\n')
    source=once(source,'        | CONVERSATION_SYM { $$= {const_cast<char *>("conversation"),12}; }',
      '        | METADATA_SYM { $$= {const_cast<char *>("metadata"),8}; }\n        | CONVERSATION_SYM { $$= {const_cast<char *>("conversation"),12}; }')
    helper=r'''
static Item *sdp_ai_user_target(THD *thd,const POS &pos,LEX_STRING name) {
  auto *values=new (thd->mem_root) PT_item_list(pos);
  if (!values || sdp_ai_pair(thd,pos,values,"kind",sdp_ai_text(thd,pos,"user_variable")) ||
      sdp_ai_pair(thd,pos,values,"name",sdp_ai_text(thd,pos,name.str,name.length))) return nullptr;
  return sdp_ai_object(thd,pos,values);
}
static Item *sdp_ai_response_payload(THD *thd,const POS &pos,Item *format,Item *options,
                                   Item *target,Item *metadata,Item *ready=nullptr) {
  auto *values=new (thd->mem_root) PT_item_list(pos);
  if (!values || sdp_ai_pair(thd,pos,values,"options",options) ||
      (format && sdp_ai_pair(thd,pos,values,"format",format))) return nullptr;
  if (target || metadata) {
    auto *capture=new (thd->mem_root) PT_item_list(pos);
    // Every AST occurrence owns a distinct Item. Reusing format here would
    // itemize it twice and make the THD intrusive cleanup list cyclic. The
    // catalog normalizes capture.format from the top-level format.
    if (!capture ||
        (target && sdp_ai_pair(thd,pos,capture,"value_target",target)) ||
        (metadata && sdp_ai_pair(thd,pos,capture,"metadata_target",metadata)) ||
        sdp_ai_pair(thd,pos,values,"capture",sdp_ai_object(thd,pos,capture))) return nullptr;
  }
  if (ready) {
    auto *supplied=new (thd->mem_root) Item_func_sdp_ready_value(thd,pos,ready);
    if (!supplied || sdp_ai_pair(thd,pos,values,"provided",supplied)) return nullptr;
  }
  return sdp_ai_object(thd,pos,values);
}
'''
    source=once(source,'static Parse_tree_root *sdp_ai_call(',helper+'\nstatic Parse_tree_root *sdp_ai_call(')
    start=source.index('        | APPEND_SYM RESPONSE_SYM expr\n')
    end=source.index('        | SHOW MEMORY_SYM\n',start)
    source=source[:start]+r'''        | APPEND_SYM RESPONSE_SYM expr sdp_ai_response_format_opt sdp_ai_options sdp_ai_response_metadata
          {
            auto *payload=sdp_ai_response_payload(YYTHD,@$,$4,$5,nullptr,$6,$3);
            if (!payload) MYSQL_YYABORT;
            $$=sdp_ai_call(YYTHD,@$,"append_ready","conversation",nullptr,payload); if (!$$) MYSQL_YYABORT;
          }
        | APPEND_SYM RESPONSE_SYM sdp_ai_response_request
          {
            $$=sdp_ai_call(YYTHD,@$,"response_options","conversation",nullptr,$3); if (!$$) MYSQL_YYABORT;
          }
        | SET_SYM MEMORY_SYM ident EQ expr sdp_ai_memory_lifetime
          {
            auto *values=NEW_PTN PT_item_list(@$);
            if (!values || sdp_ai_pair(YYTHD,@$,values,"name",sdp_ai_text(YYTHD,@3,$3.str,$3.length)) ||
                sdp_ai_pair(YYTHD,@$,values,"value",$5) || sdp_ai_pair(YYTHD,@$,values,"persistent",$6)) MYSQL_YYABORT;
            $$=sdp_ai_call(YYTHD,@$,"set_memory","conversation",nullptr,sdp_ai_object(YYTHD,@$,values)); if (!$$) MYSQL_YYABORT;
          }
'''+source[end:]
    return once(source,'sdp_ai_kind:\n',(HERE/'ai-family-next.inc').read_text(encoding='utf-8')+'\nsdp_ai_kind:\n')


def lexicon(source):
    return once(source,'    {SYM("AI", AI_SYM)},','    {SYM("AI", AI_SYM)},\n    {SYM("METADATA", METADATA_SYM)},')


def purity(source):
    return once(source,'    "convert","json_extract"','    "sdp_ready_value","convert","json_extract"')


patch=[]
for relative,transform in [('sql/sql_yacc.yy',grammar),('sql/lex.h',lexicon),('sql/sdp_ai_knowledge.h',purity)]:
    baseline=BASELINE/relative
    if not baseline.exists():
        baseline.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(SOURCE/relative,baseline)
    original=baseline.read_text(encoding='utf-8')
    updated=transform(original)
    output=OVERLAY/relative
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(updated,encoding='utf-8',newline='\n')
    patch.append(''.join(difflib.unified_diff(original.splitlines(True),updated.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative)))
header=(HERE/'sdp_ai_family_next.h').read_text(encoding='utf-8')
(OVERLAY/'sql/sdp_ai_family_next.h').write_text(header,encoding='utf-8',newline='\n')
patch.append(''.join(difflib.unified_diff([],header.splitlines(True),fromfile='/dev/null',tofile='b/sql/sdp_ai_family_next.h')))
(OVERLAY/'ai-family-next.patch').write_text(''.join(patch),encoding='utf-8',newline='\n')
print('Next AI overlay prepared:',OVERLAY)
