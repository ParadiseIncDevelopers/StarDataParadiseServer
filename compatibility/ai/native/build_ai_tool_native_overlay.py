"""Stage explicit tools/parser/factories on the frozen native12 baseline."""
from pathlib import Path
import difflib
import shutil

ROOT=Path(__file__).resolve().parents[3]
SOURCE=ROOT/'out/mysql-migration/server-upstream-baseline'
OUT=ROOT/'out/ai-tools-next13'
HERE=Path(__file__).resolve().parent


def once(source,old,new):
    if source.count(old)!=1:raise RuntimeError('AI tool native anchor: '+old[:120])
    return source.replace(old,new)


def grammar(source):
    source=once(source,'#include "sql/sdp_ai_knowledge.h"','#include "sql/sdp_ai_knowledge.h"\n#include "sql/sdp_ai_tool.h"')
    source=once(source,'%token<lexer.keyword> METADATA_SYM 1322','%token<lexer.keyword> METADATA_SYM 1322\n%token<lexer.keyword> TOOL_SYM 1323')
    source=once(source,'%type <item_list2> sdp_ai_option_list sdp_ai_conversation_options',
        '%type <item_list2> sdp_ai_option_list sdp_ai_conversation_options sdp_ai_tool_list\n%type <item> sdp_ai_tool_reference\n%type <lexer.lex_str> sdp_ai_tool_kind')
    source=once(source,'        | METADATA_SYM\n','        | METADATA_SYM\n        | TOOL_SYM\n')
    source=once(source,'        | METADATA_SYM { $$= {const_cast<char *>("metadata"),8}; }',
        '        | METADATA_SYM { $$= {const_cast<char *>("metadata"),8}; }\n        | TOOL_SYM { $$= {const_cast<char *>("tool"),4}; }')
    source=once(source,'        | EXECUTE_SYM ident table_ident \'(\' opt_expr_list \')\'',
        '''        | EXECUTE_SYM ident TOOL_SYM CALL_SYM expr
          {
            if (!is_identifier($2,"AI") || !global_system_variables.sdp_ai_sql) {YYTHD->syntax_error_at(@2);MYSQL_YYABORT;}
            $$=NEW_PTN PT_sdp_execute_tool(@$,$5);if (!$$) MYSQL_YYABORT;
          }
        | EXECUTE_SYM ident table_ident '(' opt_expr_list ')\'''')
    old='''        | sdp_ai_conversation_options WITH FUNCTIONS_SYM
          { $$=$1; if (sdp_ai_pair(YYTHD,@$,$$,"functions",NEW_PTN Item_func_true(@$))) MYSQL_YYABORT; }'''
    new=old+'''
        | sdp_ai_conversation_options WITH FUNCTIONS_SYM '(' sdp_ai_tool_list ')'
          {
            $$=$1;auto *tools=NEW_PTN Item_func_json_array(YYTHD,@5,$5);
            if (!tools || sdp_ai_pair(YYTHD,@$,$$,"functions",NEW_PTN Item_func_true(@$)) ||
                sdp_ai_pair(YYTHD,@$,$$,"tools",tools)) MYSQL_YYABORT;
          }'''
    source=once(source,old,new)
    source=once(source,"        | ALTER AI_SYM CONVERSATION_SYM table_ident WITH '(' sdp_ai_option_list ')'",
        '''        | ALTER AI_SYM CONVERSATION_SYM table_ident WITH FUNCTIONS_SYM '(' sdp_ai_tool_list ')'
          {
            auto *v=NEW_PTN PT_item_list(@$);auto *tools=NEW_PTN Item_func_json_array(YYTHD,@8,$8);
            if (!v || !tools || sdp_ai_pair(YYTHD,@$,v,"functions",NEW_PTN Item_func_true(@$)) || sdp_ai_pair(YYTHD,@$,v,"tools",tools)) MYSQL_YYABORT;
            $$=sdp_ai_call(YYTHD,@$,"alter","conversation",$4,sdp_ai_object(YYTHD,@$,v));if (!$$) MYSQL_YYABORT;
          }
        | ALTER AI_SYM CONVERSATION_SYM table_ident WITH '(' sdp_ai_option_list ')\'''')
    source=once(source,'sdp_ai_ticket:\n', '''sdp_ai_tool_list:
          sdp_ai_tool_reference
          {$$=NEW_PTN PT_item_list(@$);if (!$$ || $$->push_back($1)) MYSQL_YYABORT;}
        | sdp_ai_tool_list ',' sdp_ai_tool_reference
          {$$=$1;if ($$->push_back($3)) MYSQL_YYABORT;}
        ;
sdp_ai_tool_kind:
          KNOWLEDGE_SYM {$$={const_cast<char *>("knowledge"),9};}
        | FUNCTION_SYM {$$={const_cast<char *>("function"),8};}
        | PROCEDURE_SYM {$$={const_cast<char *>("procedure"),9};}
        ;
sdp_ai_tool_reference:
          sdp_ai_tool_kind table_ident
          {
            auto *v=NEW_PTN PT_item_list(@$);
            Item *schema=$2->db.str ? sdp_ai_text(YYTHD,@2,$2->db.str,$2->db.length) : NEW_PTN Item_func_database(@2);
            if (!v || !schema || sdp_ai_pair(YYTHD,@$,v,"kind",sdp_ai_text(YYTHD,@1,$1.str,$1.length)) ||
                sdp_ai_pair(YYTHD,@$,v,"schema_name",schema) ||
                sdp_ai_pair(YYTHD,@$,v,"name",sdp_ai_text(YYTHD,@2,$2->table.str,$2->table.length))) MYSQL_YYABORT;
            $$=sdp_ai_object(YYTHD,@$,v);if (!$$) MYSQL_YYABORT;
          }
        ;
sdp_ai_ticket:
''')
    return source


def lexer(source):
    return once(source,'    {SYM("METADATA", METADATA_SYM)},','    {SYM("METADATA", METADATA_SYM)},\n    {SYM("TOOL", TOOL_SYM)},')


def factory(source):
    source=once(source,'#include "sql/sdp_ai_knowledge.h"','#include "sql/sdp_ai_knowledge.h"\n#include "sql/sdp_ai_tool_registry.h"')
    return once(source,'    {"STARDATAPARADISE_KNOWLEDGE_VALIDATE", SQL_FN(Item_func_sdp_knowledge_validate, 1)},',
        '''    {"STARDATAPARADISE_KNOWLEDGE_VALIDATE", SQL_FN(Item_func_sdp_knowledge_validate, 1)},
    {"STARDATAPARADISE_TOOLS_VALIDATE", SQL_FN(Item_func_sdp_tools_validate, 1)},
    {"STARDATAPARADISE_PRINCIPALS", SQL_FN(Item_func_sdp_principals, 0)},
    {"STARDATAPARADISE_TOOL_EXECUTION_ACTIVE", SQL_FN(Item_func_sdp_tool_execution_active, 0)},
    {"STARDATAPARADISE_TOOL_DISPATCH_ACTIVE", SQL_FN(Item_func_sdp_tool_dispatch_active, 1)},
    {"STARDATAPARADISE_TOOL_PUBLICATION_ACTIVE", SQL_FN(Item_func_sdp_tool_publication_active, 1)},
    {"STARDATAPARADISE_ACTION_DISPATCH_ACTIVE", SQL_FN(Item_func_sdp_action_dispatch_active, 1)},''')


def actor(source):
    source=once(source,'#include "sql/item_strfunc.h"','#include "sql/item_strfunc.h"\n#include "sql/sdp_ai_invoker.h"')
    return once(source,'  const auto &ctx = current_thd->m_main_security_ctx;',
        '  const auto &ctx = sdp_ai_invoker::actor_context(current_thd);')


def main():
    (OUT/'sql').mkdir(parents=True,exist_ok=True)
    patch=[]
    for name,transform in [('sql_yacc.yy',grammar),('lex.h',lexer),('item_create.cc',factory),('item_strfunc.cc',actor)]:
        source=(SOURCE/'sql'/name).read_text(encoding='utf-8');updated=transform(source)
        (OUT/'sql'/name).write_text(updated,encoding='utf-8',newline='\n')
        patch.append(''.join(difflib.unified_diff(source.splitlines(True),updated.splitlines(True),fromfile='a/sql/'+name,tofile='b/sql/'+name)))
    for name in ('sdp_ai_knowledge.h','sdp_knowledge_source.h','sdp_ai_tool.h','sdp_ai_routine.h','sdp_ai_tool_registry.h','sdp_ai_invoker.h','sdp_ai_tool_policy.h','sdp_routine_guard.h'):
        shutil.copyfile(HERE/name,OUT/'sql'/name)
    for name in ('sdp_ai_action.h','sdp_ai_principal.h','sdp_ai_identity.h'):
        shutil.copyfile(ROOT/'out/ai-action-next13/sql'/name,OUT/'sql'/name)
    shutil.copyfile(OUT/'sdp_ai_catalog_guard.h',OUT/'sql/sdp_ai_catalog_guard.h')
    # The runtime policy overlay adds readonly IR accessors used by validation.
    for name in ('sp_instr.h','sql_signal.h'):
        shutil.copyfile(ROOT/'out/ai-tool-policy-next/sql'/name,OUT/'sql'/name)
    (OUT/'ai-tools-native-next.patch').write_text(''.join(patch),encoding='utf-8',newline='\n')
    print('Isolated native13 explicit tools:',OUT)


if __name__=='__main__':main()
