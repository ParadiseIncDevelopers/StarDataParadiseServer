"""Compose isolated typed Knowledge-source/behavior/conversation native grammar."""
from pathlib import Path
import difflib
import shutil

ROOT=Path(__file__).resolve().parents[3]
SOURCE=ROOT/'out/mysql-migration/server-upstream-baseline'
OVERLAY=ROOT/'out/ai-source-next'
HERE=Path(__file__).resolve().parent


def once(source,old,new):
    if source.count(old)!=1:
        raise RuntimeError('Missing/ambiguous context-AI anchor: '+old[:100])
    return source.replace(old,new)


source=(SOURCE/'sql/sql_yacc.yy').read_text(encoding='utf-8')
source=once(source,'#include "sql/sdp_ai_knowledge.h"','#include "sql/sdp_ai_knowledge.h"\n#include "sql/sdp_knowledge_source.h"')
source=once(source,'%type <item> sdp_ai_options','%type <lexer.lex_str> sdp_ai_option_name\n%type <table> sdp_ai_behavior_into sdp_ai_behavior_from\n%type <item> sdp_ai_options')
helper=r'''
static Parse_tree_root *sdp_ai_behavior_call(THD *thd,const POS &pos,const char *operation,
    Table_ident *behavior,Item *options,Table_ident *conversation) {
  auto *values=new(thd->mem_root) PT_item_list(pos);
  if (!values || (behavior->db.str && sdp_ai_pair(thd,pos,values,"behavior_schema",sdp_ai_text(thd,pos,behavior->db.str,behavior->db.length))) ||
      sdp_ai_pair(thd,pos,values,"behavior_name",sdp_ai_text(thd,pos,behavior->table.str,behavior->table.length))) return nullptr;
  Item *payload=sdp_ai_object(thd,pos,values);
  if (options) {
    auto *arguments=new(thd->mem_root) PT_item_list(pos);
    if (!arguments || arguments->push_back(options) || arguments->push_back(payload)) return nullptr;
    payload=new(thd->mem_root) Item_func_json_merge_patch(thd,pos,arguments);
  }
  return sdp_ai_call(thd,pos,operation,"conversation",conversation,payload);
}
'''
source=once(source,'static const char *sdp_ai_describe_kind(',helper+'\nstatic const char *sdp_ai_describe_kind(')
source=once(source,'        | START_SYM AI_SYM CONVERSATION_SYM table_ident\n',
    (HERE/'ai-context-next.inc').read_text(encoding='utf-8')+'        | START_SYM AI_SYM CONVERSATION_SYM table_ident\n')
source=once(source,'sdp_ai_option_list:\n          ident EQ expr',
    'sdp_ai_option_name:\n          ident { $$=$1; }\n        | DATABASE { $$={const_cast<char *>("schema"),6}; }\n        ;\n'
    'sdp_ai_behavior_into:\n          %empty { $$=nullptr; }\n        | INTO CONVERSATION_SYM table_ident { $$=$3; }\n        ;\n'
    'sdp_ai_behavior_from:\n          %empty { $$=nullptr; }\n        | FROM CONVERSATION_SYM table_ident { $$=$3; }\n        ;\n'
    'sdp_ai_option_list:\n          sdp_ai_option_name EQ expr')
source=once(source,"        | sdp_ai_option_list ',' ident EQ expr","        | sdp_ai_option_list ',' sdp_ai_option_name EQ expr")
start=source.index('single_table:\n')
end=source.index('\njoined_table_parens:',start)
section=source[start:end]
section=once(section,'        ;',r'''        | table_ident opt_use_partition opt_as table_ident '(' opt_expr_list ')' opt_table_alias
          {
            LEX_STRING name{const_cast<char *>($1->table.str),$1->table.length};
            if ($1->db.str || $2 || !is_identifier(name,"KNOWLEDGE")) MYSQL_YYABORT_ERROR(ER_PARSE_ERROR,MYF(0),"Expected KNOWLEDGE source");
            $$=NEW_PTN PT_sdp_knowledge_source(@$,$4,$6,$8); if (!$$) MYSQL_YYABORT;
          }
        ;''')
source=source[:start]+section+source[end:]
start=source.index('insert_stmt:\n')
end=source.index('\nsdp_insert_by_name:',start)
section=source[start:end]
section=once(section,'        ;',r'''        | INSERT_SYM insert_lock_option opt_ignore opt_INTO table_ident opt_use_partition table_ident sdp_ai_options sdp_ai_behavior_into
          {
            LEX_STRING name{const_cast<char *>($5->table.str),$5->table.length};
            if ($5->db.str || $2!=TL_WRITE_CONCURRENT_DEFAULT || $3 || $6 || !is_identifier(name,"BEHAVIOR")) MYSQL_YYABORT_ERROR(ER_PARSE_ERROR,MYF(0),"Expected INSERT BEHAVIOR");
            $$=sdp_ai_behavior_call(YYTHD,@$,"bind_behavior",$7,$8,$9); if (!$$) MYSQL_YYABORT;
          }
        ;''')
source=source[:start]+section+source[end:]
start=source.index('delete_stmt:\n')
end=source.index('\nopt_wild:',start)
section=source[start:end]
section=once(section,'        ;',r'''        | opt_with_clause DELETE_SYM opt_delete_options table_ident table_ident sdp_ai_behavior_from
          {
            LEX_STRING name{const_cast<char *>($4->table.str),$4->table.length};
            if ($1 || $3 || $4->db.str || !is_identifier(name,"BEHAVIOR")) MYSQL_YYABORT_ERROR(ER_PARSE_ERROR,MYF(0),"Expected DELETE BEHAVIOR");
            $$=sdp_ai_behavior_call(YYTHD,@$,"unbind_behavior",$5,nullptr,$6); if (!$$) MYSQL_YYABORT;
          }
        ;''')
source=source[:start]+section+source[end:]
(OVERLAY/'sql').mkdir(parents=True,exist_ok=True)
(OVERLAY/'sql/sql_yacc.yy').write_text(source,encoding='utf-8',newline='\n')
for name in ['sdp_ai_knowledge.h','sdp_knowledge_source.h','sdp_ai_invoker.h','sdp_ai_catalog_guard.h','sdp_routine_guard.h']:
    shutil.copyfile(HERE/name,OVERLAY/'sql'/name)
baseline=(SOURCE/'sql/sql_yacc.yy').read_text(encoding='utf-8')
patch=''.join(difflib.unified_diff(baseline.splitlines(True),source.splitlines(True),fromfile='a/sql/sql_yacc.yy',tofile='b/sql/sql_yacc.yy'))
(OVERLAY/'ai-context-next.patch').write_text(patch,encoding='utf-8',newline='\n')
print('Isolated context grammar:',OVERLAY)
