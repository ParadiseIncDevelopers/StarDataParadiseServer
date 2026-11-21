        | ALTER AI_SYM CONVERSATION_SYM table_ident WITH '(' sdp_ai_option_list ')'
          { $$=sdp_ai_call(YYTHD,@$,"alter","conversation",$4,sdp_ai_object(YYTHD,@$,$7)); if (!$$) MYSQL_YYABORT; }
        | ALTER AI_SYM CONVERSATION_SYM table_ident RENAME TO_SYM ident
          {
            auto *values=NEW_PTN PT_item_list(@$);
            if (!values || sdp_ai_pair(YYTHD,@$,values,"name",sdp_ai_text(YYTHD,@7,$7.str,$7.length))) MYSQL_YYABORT;
            $$=sdp_ai_call(YYTHD,@$,"rename","conversation",$4,sdp_ai_object(YYTHD,@$,values)); if (!$$) MYSQL_YYABORT;
          }
        | ALTER AI_SYM CONVERSATION_SYM table_ident ident
          {
            const char *operation=is_identifier($5,"ARCHIVE") ? "archive" : is_identifier($5,"UNARCHIVE") ? "unarchive" : nullptr;
            if (!operation) MYSQL_YYABORT_ERROR(ER_PARSE_ERROR,MYF(0),"Expected ARCHIVE or UNARCHIVE");
            $$=sdp_ai_call(YYTHD,@$,operation,"conversation",$4,nullptr); if (!$$) MYSQL_YYABORT;
          }
        | APPEND_SYM CONTEXT_SYM AS ident FROM KNOWLEDGE_SYM table_ident '(' opt_expr_list ')'
          { $$=NEW_PTN PT_sdp_append_knowledge(@$,$7,$9,$4); if (!$$) MYSQL_YYABORT; }
