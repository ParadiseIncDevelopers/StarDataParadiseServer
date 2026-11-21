/* Native Knowledge AST declarations; incorporated by the isolated overlay. */
sdp_knowledge_operation:
          CREATE { $$= {STRING_WITH_LEN("create")}; }
        | ALTER { $$= {STRING_WITH_LEN("alter")}; }
        ;
sdp_knowledge_opt_columns:
          %empty { $$= NEW_PTN PT_item_list(@$); if (!$$) MYSQL_YYABORT; }
        | sdp_knowledge_columns { $$=$1; }
        ;
sdp_knowledge_columns:
          ident type opt_collate sdp_knowledge_nullable
          {
            auto *column= NEW_PTN PT_item_list(@$);
            $$= NEW_PTN PT_item_list(@$);
            if (!column || !$$ || sdp_ai_pair(YYTHD,@$,column,"name",sdp_ai_text(YYTHD,@1,$1.str,$1.length)) ||
                sdp_ai_pair(YYTHD,@$,column,"type",sdp_ai_text(YYTHD,@2,@2.raw.start,static_cast<size_t>(@3.raw.end-@2.raw.start))) ||
                sdp_ai_pair(YYTHD,@$,column,"nullable",$4) || $$->push_back(sdp_ai_object(YYTHD,@$,column))) MYSQL_YYABORT;
          }
        | sdp_knowledge_columns ',' ident type opt_collate sdp_knowledge_nullable
          {
            auto *column= NEW_PTN PT_item_list(@$); $$=$1;
            if (!column || sdp_ai_pair(YYTHD,@$,column,"name",sdp_ai_text(YYTHD,@3,$3.str,$3.length)) ||
                sdp_ai_pair(YYTHD,@$,column,"type",sdp_ai_text(YYTHD,@4,@4.raw.start,static_cast<size_t>(@5.raw.end-@4.raw.start))) ||
                sdp_ai_pair(YYTHD,@$,column,"nullable",$6) || $$->push_back(sdp_ai_object(YYTHD,@$,column))) MYSQL_YYABORT;
          }
        ;
sdp_knowledge_nullable:
          %empty { $$= NEW_PTN Item_func_false(@$); }
        | NULL_SYM { $$= NEW_PTN Item_func_true(@$); }
        | NOT_SYM NULL_SYM { $$= NEW_PTN Item_func_false(@$); }
        ;
