/* Response and memory native AST extensions. No provider I/O in parsing. */
sdp_ai_response_format:
          TEXT_SYM { $$=sdp_ai_text(YYTHD,@$,"TEXT"); }
        | JSON_SYM { $$=sdp_ai_text(YYTHD,@$,"JSON"); }
        ;
sdp_ai_response_format_opt:
          %empty { $$=nullptr; }
        | AS sdp_ai_response_format { $$=$2; }
        ;
sdp_ai_response_output:
          INTO '@' ident_or_text { $$=sdp_ai_user_target(YYTHD,@$,$3); if (!$$) MYSQL_YYABORT; }
        ;
sdp_ai_response_metadata:
          %empty { $$=nullptr; }
        | RETURNING_SYM METADATA_SYM INTO '@' ident_or_text
          { $$=sdp_ai_user_target(YYTHD,@$,$5); if (!$$) MYSQL_YYABORT; }
        ;
sdp_ai_memory_lifetime:
          PERSISTENT_SYM { $$=NEW_PTN Item_func_true(@$); }
        | TEMPORARY { $$=NEW_PTN Item_func_false(@$); }
        ;
sdp_ai_response_request:
          %empty
          {
            auto *values=NEW_PTN PT_item_list(@$);
            if (!values) MYSQL_YYABORT;
            $$=sdp_ai_response_payload(YYTHD,@$,nullptr,sdp_ai_object(YYTHD,@$,values),nullptr,nullptr);
            if (!$$) MYSQL_YYABORT;
          }
        | sdp_ai_response_output sdp_ai_response_format_opt sdp_ai_options sdp_ai_response_metadata
          { $$=sdp_ai_response_payload(YYTHD,@$,$2,$3,$1,$4); if (!$$) MYSQL_YYABORT; }
        | AS sdp_ai_response_format sdp_ai_options sdp_ai_response_metadata
          { $$=sdp_ai_response_payload(YYTHD,@$,$2,$3,nullptr,$4); if (!$$) MYSQL_YYABORT; }
        | WITH '(' sdp_ai_option_list ')' sdp_ai_response_metadata
          { $$=sdp_ai_response_payload(YYTHD,@$,nullptr,sdp_ai_object(YYTHD,@$,$3),nullptr,$5); if (!$$) MYSQL_YYABORT; }
        | RETURNING_SYM METADATA_SYM INTO '@' ident_or_text
          {
            auto *values=NEW_PTN PT_item_list(@$);
            auto *target=sdp_ai_user_target(YYTHD,@$,$5);
            if (!values || !target) MYSQL_YYABORT;
            $$=sdp_ai_response_payload(YYTHD,@$,nullptr,sdp_ai_object(YYTHD,@$,values),nullptr,target);
            if (!$$) MYSQL_YYABORT;
          }
        ;
