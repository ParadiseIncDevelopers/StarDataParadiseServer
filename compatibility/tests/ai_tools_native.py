"""Actual native typed tool execution with fixture-only provider publication.

No provider API key or model-authored SQL is used. Native parser, owner ACL,
typed arguments, durable calls and data/receipt transactions are exercised.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import sys
from .ai_catalog_acceptance import execute, install
from .ai_commit_guard_native import connection, drain, statement
from .fixture import BASEDIR, BIN, MIGRATION, REPOSITORY, SHELL, Fixture, query, require, run
from .workbench_native import backend_certificate
from compatibility.decision.provision import parse_rules, provision

WORKER_PASSWORD=secrets.token_hex(24)


def document(cursor, source, parameters=None):
    rows=statement(cursor,source,parameters)
    require(len(rows)==1 and len(rows[0])==1,'expected one protected document: '+repr(rows))
    value=rows[0][0]
    return json.loads(value) if isinstance(value,(str,bytes,bytearray)) else value


def user_connection(instance,user):
    import mysql.connector
    options={v[2:].split('=',1)[0]:v.split('=',1)[1] for v in instance[1] if v.startswith('--') and '=' in v}
    return mysql.connector.connect(host='127.0.0.1',port=int(options['port']),user=user,
        password=WORKER_PASSWORD if user=='tool_worker' else '',
        ssl_ca=options['ssl-ca'],ssl_verify_cert=True,ssl_verify_identity=True,autocommit=True,use_pure=True)


def bootstrap(instance,catalog,actions):
    install(instance,False)
    execute(instance,(catalog/'catalog.sql').read_text(encoding='utf-8'))
    execute(instance,(catalog/'tool_catalog.sql').read_text(encoding='utf-8'))
    execute(instance,actions.read_text(encoding='utf-8'))
    execute(instance,"""CREATE USER tool_owner@localhost IDENTIFIED BY '' REQUIRE SSL;
      CREATE USER tool_worker@localhost IDENTIFIED BY 'WORKER_FIXTURE_PASSWORD' REQUIRE SSL;
      CREATE ROLE tool_case_reader;
      CREATE TABLE fixture.tool_rows(id BIGINT UNSIGNED PRIMARY KEY,quantity BIGINT NOT NULL,amount DECIMAL(38,9),label VARCHAR(60) CHARACTER SET utf8mb4) ENGINE=InnoDB;
      INSERT INTO fixture.tool_rows VALUES(18446744073709551615,1,12345678901234567890123456789.123456789,'Türkçe');
      CREATE TABLE fixture.tool_clock(id INT PRIMARY KEY,supplied TIMESTAMP(6) NULL,observed DATETIME(6) NOT NULL) ENGINE=InnoDB;
      CREATE TABLE fixture.tool_decision_rows(id INT PRIMARY KEY,quantity BIGINT NOT NULL,total BIGINT NOT NULL) ENGINE=InnoDB;
      INSERT INTO fixture.tool_decision_rows VALUES(1,1,2);
      CREATE TABLE fixture.tool_trigger_rows(id INT PRIMARY KEY,quantity BIGINT NOT NULL,total BIGINT NOT NULL) ENGINE=InnoDB;
      INSERT INTO fixture.tool_trigger_rows VALUES(1,1,3);
      CREATE USER tool_trigger@localhost IDENTIFIED BY '' ACCOUNT LOCK;
      GRANT TRIGGER,UPDATE ON fixture.tool_trigger_rows TO tool_trigger@localhost;
      GRANT SELECT,UPDATE ON fixture.tool_rows TO tool_owner@localhost;
      GRANT SELECT,INSERT ON fixture.tool_clock TO tool_owner@localhost;
      GRANT SELECT,UPDATE ON fixture.tool_decision_rows TO tool_owner@localhost;
      GRANT SELECT,UPDATE ON fixture.tool_trigger_rows TO tool_owner@localhost;
      GRANT INSERT ON fixture.ledger TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.dispatch TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.before_commit TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.after_commit TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.poll_commit TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.rollback_turn TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.knowledge_document TO tool_owner@localhost;
      GRANT EXECUTE ON FUNCTION sdp_ai.knowledge_value TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.claim_run TO tool_worker@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.renew_lease TO tool_worker@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.publish_run TO tool_worker@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.publish_tool_requests TO tool_worker@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.publish_tool_failure TO tool_worker@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.tool_call_status TO tool_worker@localhost;
      DELIMITER $$
      CREATE FUNCTION fixture.tool_decimal(p DECIMAL(38,9)) RETURNS DECIMAL(38,9) SQL SECURITY INVOKER DETERMINISTIC RETURN p$$
      CREATE FUNCTION fixture.tool_decimal_probe(p DECIMAL(38,9)) RETURNS DECIMAL(38,9) SQL SECURITY INVOKER DETERMINISTIC
      BEGIN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='fixture_decimal_body_entered';RETURN p;END$$
      CREATE PROCEDURE fixture.tool_update(IN p_id BIGINT UNSIGNED,IN p_quantity BIGINT) SQL SECURITY INVOKER MODIFIES SQL DATA
      BEGIN UPDATE fixture.tool_rows SET quantity=p_quantity WHERE id=p_id;
        SELECT id,quantity,STARDATAPARADISE_ACTOR() AS actor,CURRENT_USER() AS effective_actor FROM fixture.tool_rows WHERE id=p_id;
      END$$
      CREATE PROCEDURE fixture.tool_commit_escape(IN p INT) SQL SECURITY INVOKER MODIFIES SQL DATA
      BEGIN INSERT INTO fixture.ledger VALUES(p);COMMIT;END$$
      CREATE PROCEDURE fixture.tool_ddl_escape() SQL SECURITY INVOKER
      BEGIN CREATE TABLE fixture.forbidden_tool_table(id INT);END$$
      CREATE PROCEDURE fixture.tool_timeout(IN p INT) SQL SECURITY INVOKER MODIFIES SQL DATA
      BEGIN DECLARE counter_value BIGINT DEFAULT 0;INSERT INTO fixture.ledger VALUES(p);
        WHILE counter_value<100000000 DO SET counter_value=counter_value+1;END WHILE;
      END$$
      CREATE PROCEDURE fixture.tool_time(IN p TIMESTAMP(6)) SQL SECURITY INVOKER MODIFIES SQL DATA
      BEGIN INSERT INTO fixture.tool_clock VALUES(1,p,CURRENT_TIMESTAMP(6));
        SELECT supplied,observed FROM fixture.tool_clock WHERE id=1;
      END$$
      CREATE PROCEDURE fixture.tool_decision(IN p BIGINT) SQL SECURITY INVOKER MODIFIES SQL DATA
      BEGIN UPDATE fixture.tool_decision_rows SET quantity=p WHERE id=1;
        SELECT quantity,total FROM fixture.tool_decision_rows WHERE id=1;
      END$$
      CREATE DEFINER=tool_trigger@localhost TRIGGER fixture.tool_restricted_trigger BEFORE UPDATE ON fixture.tool_trigger_rows
        FOR EACH ROW SET NEW.total=NEW.quantity*3$$
      CREATE PROCEDURE fixture.tool_trigger_update(IN p BIGINT) SQL SECURITY INVOKER MODIFIES SQL DATA
      BEGIN UPDATE fixture.tool_trigger_rows SET quantity=p WHERE id=1;
        SELECT quantity,total FROM fixture.tool_trigger_rows WHERE id=1;
      END$$
      DELIMITER ;
      GRANT EXECUTE ON FUNCTION fixture.tool_decimal TO tool_owner@localhost;
      GRANT EXECUTE ON FUNCTION fixture.tool_decimal_probe TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE fixture.tool_update TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE fixture.tool_commit_escape TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE fixture.tool_ddl_escape TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE fixture.tool_timeout TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE fixture.tool_trigger_update TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE fixture.tool_time TO tool_owner@localhost;""".replace('WORKER_FIXTURE_PASSWORD',WORKER_PASSWORD))
    execute(instance, 'GRANT EXECUTE ON PROCEDURE fixture.tool_decision TO tool_owner@localhost;')
    # Reuse the existing Decision compiler and its native trigger semantics.
    # An AI action must observe the same DENY and SET behavior as ordinary SQL.
    execute(instance, provision(parse_rules('''
      CREATE DECISION RULE tool_negative ON fixture.tool_decision_rows FOR UPDATE PRIORITY 200 WHEN (NEW.quantity < 0) THEN DENY;
      CREATE DECISION RULE tool_calculate ON fixture.tool_decision_rows FOR UPDATE PRIORITY 100 WHEN (NEW.quantity >= 0) THEN SET total = NEW.quantity * 2;
      CREATE DECISION RULE tool_allow ON fixture.tool_decision_rows FOR UPDATE PRIORITY 0 WHEN (NEW.quantity >= 0) THEN ALLOW;
    ''')))
    worker=user_connection(instance,'tool_worker')
    try:
        cursor=worker.cursor()
        actor=statement(cursor,'SELECT STARDATAPARADISE_ACTOR()')[0][0]
        execute(instance,"INSERT INTO sdp_ai.worker_accounts(actor) VALUES('"+actor.replace("'","''")+"');")
        cursor.close()
    finally:worker.close()


def installed_sdk_deadline(instance, root, python, call_id, run_id):
    from .dotnet_native_acceptance import native_bridge
    front=backend_certificate(MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe',root,'tool-sdk-front')
    options={v[2:].split('=',1)[0]:v.split('=',1)[1] for v in instance[1] if v.startswith('--') and '=' in v}
    uuid,version=query(instance,'SELECT @@server_uuid,@@version').split('\t')
    command=[sys.executable,'-B',str(REPOSITORY/'compatibility/wire_v1/service.py'),'--port','0',
        '--cert',str(front/'server.pem'),'--key',str(front/'server.key'),'--mysql-port',options['port'],
        '--mysql-ca',options['ssl-ca'],'--mysql-uuid',uuid,'--product-version',version.removesuffix('-StarDataParadise'),
        '--schema','fixture','--native-sql']
    environment=instance[0].copy()
    environment['PYTHONPATH']=str(MIGRATION/'wire-v1/dependencies')+os.pathsep+str(REPOSITORY)
    with native_bridge(command,environment,root) as port:
        settings=dict(port=port,user='tool_worker',password=WORKER_PASSWORD,ca_file=str(front/'ca.pem'),call_id=call_id,run_id=run_id)
        environment['SDP_NATIVE_TOOL_SETTINGS']=json.dumps(settings)
        # Isolated mode ignores PYTHONPATH/source trees for the consumer. The
        # gateway separately uses the repository's current transport backend.
        result=run([str(python),'-I','-B',str(REPOSITORY/'compatibility/tests/native_tool_error_consumer.py')],environment,60)
        require(result.returncode==0,'installed SDK tool rollback consumer failed: '+result.stdout+result.stderr)
        value=json.loads(result.stdout)
        require(value['proof']['call_id']==call_id and value['proof']['run_id']==run_id,'SDK consumer returned another call proof')
        print('PASS real native61085 -> gateway TLS -> installed PythonSDK'+value['sdk_version']+' rollback ABI: '+value['sdk_file'],flush=True)
        print('Native SDK rollback proof: '+json.dumps(value['proof'],sort_keys=True),flush=True)
        return value['proof']


def catalog_case_vector(app, instance):
    import mysql.connector
    canonical_model=document(app,'DESCRIBE MODEL fixture.tool_model')
    for target in ('FiXtUrE.tool_model','fixture.TOOL_MODEL'):
        try:document(app,'DESCRIBE MODEL '+target)
        except mysql.connector.Error as error:require(error.errno==61041,'custom catalog case mismatch returned unexpected error: '+repr(error))
        else:raise RuntimeError('custom AI namespace silently normalized a distinct catalog spelling')
        try:statement(app,'GRANT USE ON MODEL '+target+' TO ROLE tool_case_reader')
        except mysql.connector.Error as error:require(error.errno==61081,'custom grant case mismatch returned unexpected error: '+repr(error))
        else:raise RuntimeError('custom grant resolved a different catalog spelling')
    statement(app,'GRANT USE ON MODEL fixture.tool_model TO ROLE tool_case_reader')
    require(query(instance,"SELECT COUNT(*) FROM sdp_ai.object_grants WHERE object_id='"+canonical_model['id']+"' AND privilege_name='USE'")=='1',
        'exact catalog GRANT did not bind the discovered model id')
    statement(app,'REVOKE USE ON MODEL fixture.tool_model FROM ROLE tool_case_reader')


def duplicate_tool_vector(app):
    import mysql.connector
    try:
        statement(app,'CREATE AI CONVERSATION fixture.duplicate_tool USING MODEL fixture.tool_model WITH FUNCTIONS (FUNCTION fixture.tool_decimal, FUNCTION fixture.TOOL_DECIMAL)')
    except mysql.connector.Error as error:
        require(error.errno==61081,'duplicate DD tool identity had the wrong error: '+repr(error))
    else:
        raise RuntimeError('case-equivalent tool registrations created duplicate provider identities')


def ordinary_decision_vector(app):
    import mysql.connector
    statement(app,'UPDATE fixture.tool_decision_rows SET quantity=4 WHERE id=1')
    require(statement(app,'SELECT quantity,total FROM fixture.tool_decision_rows')==[(4,8)],
        'ordinary owner UPDATE did not apply the existing Decision SET')
    require(statement(app,'CALL fixture.tool_decision(5)')==[(5,10)],
        'ordinary owner CALL did not apply the existing Decision SET')
    try:statement(app,'UPDATE fixture.tool_decision_rows SET quantity=-1 WHERE id=1')
    except mysql.connector.Error as error:require(error.errno==1644 and 'decision_denied' in error.msg,
        'ordinary Decision DENY returned the wrong condition: '+repr(error))
    else:raise RuntimeError('ordinary Decision DENY permitted its application write')
    require(statement(app,'SELECT quantity,total FROM fixture.tool_decision_rows')==[(5,10)],
        'ordinary Decision DENY changed the row')
    statement(app,'UPDATE fixture.tool_decision_rows SET quantity=6 WHERE id=1')


def ordinary_restricted_trigger_vector(app):
    import mysql.connector
    try:statement(app,'UPDATE fixture.tool_trigger_rows SET quantity=4 WHERE id=1')
    except mysql.connector.Error as error:require(error.errno==1143,
        'restricted trigger definer did not retain its own column ACL: '+repr(error))
    else:raise RuntimeError('ordinary trigger inherited another invoker table grant cache')


def exercise(instance, sdk_python=None, root=None):
    import mysql.connector
    owner,worker=user_connection(instance,'tool_owner'),user_connection(instance,'tool_worker')
    try:
        app,agent=owner.cursor(),worker.cursor()
        tool_statement=worker.cursor(prepared=True)
        try:
            owner_actor=statement(app,'SELECT STARDATAPARADISE_ACTOR()')[0][0]
            worker_actor=statement(agent,'SELECT STARDATAPARADISE_ACTOR()')[0][0]
            statement(agent,'SET SESSION sdp_ai_worker_rpc_guard=ON')
            statement(agent,"SET SESSION time_zone='+00:00'")
            statement(app,"SET SESSION time_zone='+05:30'")
            statement(app,"CREATE MODEL fixture.tool_model TYPE GENERATIVE PROVIDER OPENAI MODEL 'fixture-model' WITH(secret_env='UNUSED_FIXTURE_KEY',capabilities=JSON_ARRAY('tools'))")
            catalog_case_vector(app,instance)
            statement(app,"""CREATE KNOWLEDGE fixture.tool_knowledge(p_id BIGINT UNSIGNED NOT NULL)
              RETURNS TABLE(id BIGINT UNSIGNED NOT NULL,amount DECIMAL(38,9) NULL,label VARCHAR(60) NULL,actor VARCHAR(576) NOT NULL)
              READ ONLY SQL SECURITY INVOKER AS SELECT id,amount,label,STARDATAPARADISE_ACTOR() FROM fixture.tool_rows WHERE id=:p_id""")

            worker_id='a'*32
            # Bare WITH FUNCTIONS is an empty allowlist. It cannot require a
            # tools-capable model or silently discover executable routines.
            statement(app,"CREATE MODEL fixture.bare_model TYPE GENERATIVE PROVIDER OPENAI MODEL 'fixture-bare' WITH(secret_env='UNUSED_FIXTURE_KEY')")
            statement(app,'CREATE AI CONVERSATION fixture.bare_turn USING MODEL fixture.bare_model WITH FUNCTIONS')
            statement(app,'START AI CONVERSATION fixture.bare_turn')
            statement(app,"APPEND PROMPT 'No implicit tools'")
            statement(app,'APPEND RESPONSE AS TEXT')
            bare_receipt=document(app,'COMMIT ASYNC RETURNING RUN INTO @bare_run')
            statement(app,'FINISH')
            bare_claim=document(agent,'CALL sdp_ai.claim_run(%s,3720)',(worker_id,))
            require(bare_claim['run_id']==bare_receipt['run_id'] and bare_claim.get('tools',[])==[],
                'bare WITH FUNCTIONS discovered an implicit tool')
            document(agent,'CALL sdp_ai.publish_run(%s,%s,CAST(%s AS JSON),CAST(%s AS JSON),CAST(%s AS JSON))',
                (bare_receipt['run_id'],worker_id,json.dumps(dict(format='TEXT',value='No tools requested')),'{}','null'))
            require(document(app,'DESCRIBE CONVERSATION fixture.bare_turn')['tool_calls']==[],
                'empty allowlist created a tool summary')

            def invoke(name,reference,arguments,*,timeout=False,expected_failure=False,before_execute=None):
                statement(app,'CREATE AI CONVERSATION fixture.'+name+' USING MODEL fixture.tool_model WITH FUNCTIONS ('+reference+')')
                statement(app,'START AI CONVERSATION fixture.'+name)
                statement(app,"APPEND PROMPT 'Fixture tool request - no external provider'")
                if timeout:statement(app,'APPEND RESPONSE AS TEXT WITH(provider_timeout_ms=1000,run_timeout_ms=1000)')
                else:statement(app,'APPEND RESPONSE AS TEXT')
                try:
                    receipt=document(app,'COMMIT ASYNC RETURNING RUN INTO @fixture_run')
                except mysql.connector.Error as error:
                    if error.errno==61084:
                        try:
                            pinned=document(app,'DESCRIBE CONVERSATION fixture.'+name)['settings'].get('tool_bindings')
                            kind,qualified=reference.split(' ',1);schema,target=qualified.split('.',1)
                            requested=[dict(kind=kind.lower(),schema_name=schema,name=target)]
                            fresh=document(app,'SELECT CAST(STARDATAPARADISE_TOOLS_VALIDATE(CAST(%s AS JSON)) AS JSON)',(json.dumps(requested),))
                            print('COMMIT descriptor drift pinned: '+json.dumps(pinned,sort_keys=True,ensure_ascii=False),flush=True)
                            print('COMMIT descriptor drift fresh: '+json.dumps(fresh,sort_keys=True,ensure_ascii=False),flush=True)
                            observer=user_connection(instance,'tool_owner')
                            try:
                                check=observer.cursor()
                                statement(check,'CREATE AI CONVERSATION fixture.fresh_validation_diagnostic USING MODEL fixture.tool_model WITH FUNCTIONS ('+reference+')')
                                other=document(check,'DESCRIBE CONVERSATION fixture.fresh_validation_diagnostic')['settings'].get('tool_bindings')
                                print('COMMIT descriptor fresh-session pin: '+json.dumps(other,sort_keys=True,ensure_ascii=False),flush=True)
                                check.close()
                            finally:observer.close()
                        except Exception as diagnostic:
                            print('Descriptor diagnostic failed; original commit error preserved: '+str(diagnostic),flush=True)
                    raise
                statement(app,'FINISH')
                run_id=receipt['run_id']
                claim=document(agent,"CALL sdp_ai.claim_run(%s,3720)",(worker_id,))
                require(claim['run_id']==run_id and len(claim['tools'])==1,'actual native allowlist snapshot was not queued')
                require(claim['tools'][0]['schema_name']=='fixture' and claim['tools'][0]['name']==reference.split('.')[-1].lower(),
                    'tool descriptor did not preserve canonical DD identity')
                document(agent,'CALL sdp_ai.renew_lease(%s,%s,3720)',(run_id,worker_id))
                descriptor=claim['tools'][0]
                calls=[dict(provider_call_id='fixture-call-1',provider_name=descriptor['provider_name'],arguments=arguments)]
                publication=document(agent,'CALL sdp_ai.publish_tool_requests(%s,%s,%s,%s,CAST(%s AS JSON),CAST(%s AS JSON))',
                    (run_id,worker_id,'b'*32,'Fixture tool selection',json.dumps(calls,ensure_ascii=False),'{}'))
                require(publication['state']=='waiting_tool','unapproved fixture tool unexpectedly requested approval')
                claimed=document(agent,'CALL sdp_ai.claim_run(%s,3720)',(worker_id,))
                require(claimed['phase']=='tools' and len(claimed['tool_calls_to_execute'])==1,'native tool execution phase was not claimable')
                call_id=claimed['tool_calls_to_execute'][0]['call_id']
                pending_summary=document(app,'DESCRIBE CONVERSATION fixture.'+name)['tool_calls']
                require(len(pending_summary)==1 and pending_summary[0]['call_id']==call_id and pending_summary[0]['run_id']==run_id,
                    'conversation did not expose its authorized bounded tool summary')
                require(not any(key in pending_summary[0] for key in ('arguments','result','snapshot','execution_context')),
                    'conversation tool summary exposed protected payloads')
                if before_execute is not None:before_execute()
                if timeout:
                    if sdk_python:
                        proof=installed_sdk_deadline(instance,root,sdk_python,call_id,run_id)
                    else:
                        try:document(agent,'EXECUTE AI TOOL CALL %s',(call_id,))
                        except mysql.connector.Error as error:
                            require(error.errno==61085 and error.msg.startswith('510005:'),'timeout lacked exact native rollback proof ABI: '+repr(error))
                            proof=json.loads(error.msg.partition(':')[2])
                            require(proof.get('run_id')==run_id and proof.get('call_id')==call_id and proof.get('action_commit_state')=='rolled_back' and proof.get('unknown_outcome') is False,
                                'timeout did not prove this specific action transaction rolled back: '+repr(proof))
                            require(proof.get('cause_native_errno')==3024,'timeout cause was not kept separate from native tool errno')
                        else:raise RuntimeError('bounded tool CPU loop did not time out')
                    proof.update(code=510005,native_errno=61085)
                    result=document(agent,'CALL sdp_ai.publish_tool_failure(%s,%s,%s,CAST(%s AS JSON))',(run_id,worker_id,call_id,json.dumps(proof)))
                    require(result['tool_state']=='failed','exact rollback proof was not durably published')
                else:
                    result=document(tool_statement,'EXECUTE AI TOOL CALL ?',(call_id,))
                    if result['tool_state']!=('failed' if expected_failure else 'succeeded'):
                        detail=document(agent,'CALL sdp_ai.tool_call_status(%s)',(call_id,))
                        raise RuntimeError('native typed tool had the wrong outcome: '+repr(detail))
                status=document(agent,'CALL sdp_ai.tool_call_status(%s)',(call_id,))
                run_summary=document(app,'DESCRIBE RUN %s',(run_id,))['tool_calls']
                require(len(run_summary)==1 and run_summary[0]['call_id']==call_id and run_summary[0]['state']==status['tool_state'],
                    'run tool summary disagreed with durable receipt')
                require(status['run_id']==run_id and status['replayed'] is True,'native action receipt was not durable')
                require(statement(agent,'SELECT STARDATAPARADISE_ACTOR()')[0][0]==worker_actor,'logical owner leaked back into pooled worker connection')
                require(statement(agent,'SELECT CURRENT_USER()')[0][0]==worker_actor,'effective application/private security context leaked into worker connection')
                require(statement(agent,'SELECT @@session.time_zone')[0][0]=='+00:00','owner timezone leaked back into pooled worker connection')
                try:statement(agent,'SELECT label FROM fixture.tool_rows')
                except mysql.connector.Error as error:require(error.errno==1142,'unexpected ordinary worker ACL result after tool: '+repr(error))
                else:raise RuntimeError('native invocation left application table privileges on the worker connection')
                continuation=document(agent,'CALL sdp_ai.claim_run(%s,3720)',(worker_id,))
                require(continuation['phase']=='inference' and continuation['messages'][-1]['role']=='tool','native result did not produce a typed provider continuation')
                document(agent,'CALL sdp_ai.publish_run(%s,%s,CAST(%s AS JSON),CAST(%s AS JSON),CAST(%s AS JSON))',
                    (run_id,worker_id,json.dumps(dict(format='TEXT',value='Fixture response - no external provider')),'{}','null'))
                return status,call_id

            knowledge,_=invoke('knowledge_turn','KNOWLEDGE fixture.tool_knowledge',dict(p_id=18446744073709551615))
            require(knowledge['result']['rows']==[dict(id=18446744073709551615,amount='12345678901234567890123456789.123456789',label='Türkçe',actor=owner_actor)],
                'native Knowledge tool lost exact values or logical owner: '+repr(knowledge))
            function,_=invoke('function_turn','FUNCTION fixture.TOOL_DECIMAL',dict(p='12345678901234567890123456789.123456789'))
            require(function['result']['value']=='12345678901234567890123456789.123456789' and function['result']['columns'][0]['type']=='DECIMAL(38,9)',
                'native function tool lost exact typed Decimal result')
            duplicate_tool_vector(app)
            invalid,_=invoke('function_scale_rejection','FUNCTION fixture.tool_decimal',dict(p='1.1234567891'),expected_failure=True)
            require(invalid['result'] is None and invalid['error'] is not None,'fractional precision was silently lost before function execution')
            probe,_=invoke('function_prebody_rejection','FUNCTION fixture.tool_decimal_probe',dict(p='1.1234567891'),expected_failure=True)
            require(probe['error']['native_errno']==61042,'lossy DECIMAL reached its signaling function body before native parameter validation')
            procedure,call_id=invoke('procedure_turn','PROCEDURE fixture.tool_update',dict(p_id=18446744073709551615,p_quantity=7))
            values=procedure['result']['result_sets'][0]['rows']
            require(values==[[18446744073709551615,7,owner_actor,owner_actor]],'routine executed with a worker/definer identity or wrong result: '+repr(values))
            ordinary_decision_vector(app)
            decision,_=invoke('decision_set_turn','PROCEDURE fixture.tool_decision',dict(p=6))
            require(decision['result']['result_sets'][0]['rows']==[[6,12]],'actual Decision SET effect was absent from typed tool result')
            ordinary_decision_vector(app)
            denied,_=invoke('decision_deny_turn','PROCEDURE fixture.tool_decision',dict(p=-1),expected_failure=True)
            require(denied['result'] is None and query(instance,'SELECT CONCAT(quantity,\':\',total) FROM fixture.tool_decision_rows')=='6:12',
                'Decision DENY did not roll back the native tool application write')
            ordinary_restricted_trigger_vector(app)
            trigger,_=invoke('trigger_owner_turn','PROCEDURE fixture.tool_trigger_update',dict(p=5))
            require(trigger['result']['result_sets'][0]['rows']==[[5,15]],
                'verified logical owner did not use its current trigger field privileges')
            ordinary_restricted_trigger_vector(app)
            try:
                revoked,_=invoke('trigger_owner_revoked','PROCEDURE fixture.tool_trigger_update',dict(p=8),expected_failure=True,
                    before_execute=lambda:execute(instance,'REVOKE SELECT ON fixture.tool_trigger_rows FROM tool_owner@localhost;'))
                require(revoked['error']['native_errno'] in (1142,1143) and
                        query(instance,"SELECT CONCAT(quantity,':',total) FROM fixture.tool_trigger_rows")=='5:15',
                    'revoked owner SELECT permitted trigger execution or leaked a partial write: '+repr(revoked))
            finally:
                execute(instance,'GRANT SELECT ON fixture.tool_trigger_rows TO tool_owner@localhost;')
            ordinary_restricted_trigger_vector(app)
            replay=document(agent,'EXECUTE AI TOOL CALL %s',(call_id,))
            require(replay['replayed'] is True and query(instance,'SELECT quantity FROM fixture.tool_rows')=='7','receipt replay reexecuted application SQL')
            # A worker with no application grants must still be unable to read
            # the table after an owner-scoped successful or failed invocation.
            try:statement(agent,'SELECT label FROM fixture.tool_rows')
            except mysql.connector.Error as error:require(error.errno==1142,'unexpected ordinary worker ACL error: '+str(error))
            else:raise RuntimeError('native tool left application privileges on the worker connection')
            clock,_=invoke('timezone_turn','PROCEDURE fixture.tool_time',dict(p='2026-01-01 12:00:00.123456'))
            require(clock['result']['result_sets'][0]['rows'][0][0]=='2026-01-01 12:00:00.123456','TIMESTAMP was bound/formatted under worker timezone')
            observer=connection(instance)
            try:
                check=observer.cursor();statement(check,"SET SESSION time_zone='+00:00'")
                observed=statement(check,"SELECT CAST(supplied AS CHAR),TIMESTAMPDIFF(SECOND,UTC_TIMESTAMP(6),observed) FROM fixture.tool_clock")
                require(observed[0][0]=='2026-01-01 06:30:00.123456' and abs(observed[0][1]-19800)<=30,
                    'TIMESTAMP or CURRENT_TIMESTAMP did not preserve initiating +05:30 session semantics: '+repr(observed))
                check.close()
            finally:observer.close()
            for name,reference in [('commit_escape','fixture.tool_commit_escape'),('ddl_escape','fixture.tool_ddl_escape')]:
                try:statement(app,'CREATE AI CONVERSATION fixture.'+name+' USING MODEL fixture.tool_model WITH FUNCTIONS (PROCEDURE '+reference+')')
                except mysql.connector.Error as error:require(error.errno==61081,'unsafe routine registration had the wrong boundary: '+str(error))
                else:raise RuntimeError('native tool registry accepted an internal transaction/implicit DDL escape')
            invoke('timeout_turn','PROCEDURE fixture.tool_timeout',dict(p=990),timeout=True)
            require(query(instance,'SELECT COUNT(*) FROM fixture.ledger')=='0','failed native tool committed its partial application write')
            require(statement(agent,'SELECT STARDATAPARADISE_ACTOR()')[0][0]==worker_actor,'timeout leaked logical owner state')
            require(statement(agent,'SELECT @@session.time_zone')[0][0]=='+00:00','timeout leaked owner timezone')
        finally:
            app.close();agent.close();tool_statement.close()
    finally:
        owner.close();worker.close()
    print('PASS actual native tools: typed Knowledge/function/procedure, exact Decimal/uint64, logical owner ACL, data+receipt and idempotent replay')
    print('PASS actual native tool prepared statement: one statement rebound across distinct Knowledge/function/procedure calls')
    print('PASS native atomicity policy: transaction/implicit-DDL registration rejection and real CPU deadline rollback proof')
    print('PASS existing Decision trigger integration: SET appears in typed result and DENY rolls back the action')
    print('PASS trigger ACL cache: ordinary definer and logical owner stay isolated across success, failure and grant revocation')
    print('PASS owner +05:30 / worker UTC: typed TIMESTAMP, CURRENT_TIMESTAMP, lease renewal and timezone restoration after success/failure/timeout')
    print('External provider: NOT_RUN (durable provider request/publication fixture only)')


def exercise_approval(instance):
    """Real protected provider requests use the same native approval records."""
    import mysql.connector
    execute(instance,"""CREATE USER tool_approver@localhost IDENTIFIED BY '' REQUIRE SSL;
      CREATE ROLE tool_approve_role;
      GRANT tool_approve_role TO tool_approver@localhost;
      CREATE TABLE fixture.tool_approval_rows(id INT PRIMARY KEY,quantity INT NOT NULL,effects INT NOT NULL) ENGINE=InnoDB;
      INSERT INTO fixture.tool_approval_rows VALUES(1,1,0);
      DELIMITER $$
      CREATE PROCEDURE fixture.tool_approved_update(IN p INT) SQL SECURITY INVOKER MODIFIES SQL DATA
      BEGIN UPDATE fixture.tool_approval_rows SET quantity=p,effects=effects+1 WHERE id=1;
        SELECT quantity,effects FROM fixture.tool_approval_rows WHERE id=1;
      END$$
      DELIMITER ;
      GRANT SELECT,UPDATE ON fixture.tool_approval_rows TO tool_owner@localhost;
      GRANT EXECUTE ON PROCEDURE fixture.tool_approved_update TO tool_owner@localhost;
      REQUIRE APPROVAL ON PROCEDURE fixture.tool_approved_update;
      GRANT APPROVE ON PROCEDURE fixture.tool_approved_update TO ROLE tool_approve_role;""")
    owner,worker,approver=(user_connection(instance,name) for name in ('tool_owner','tool_worker','tool_approver'))
    try:
        app,agent,review=owner.cursor(),worker.cursor(),approver.cursor()
        prepared=worker.cursor(prepared=True)
        try:
            statement(app,"SET SESSION time_zone='+05:30'")
            statement(agent,'SET SESSION sdp_ai_worker_rpc_guard=ON')
            statement(agent,"SET SESSION time_zone='+00:00'")
            statement(review,'SET ROLE tool_approve_role')
            try:statement(app,'CALL fixture.tool_approved_update(99)')
            except mysql.connector.Error as error:require(error.errno==61083,'ordinary CALL did not enforce approval: '+repr(error))
            else:raise RuntimeError('ordinary CALL bypassed the required native action approval')
            worker_id='c'*32
            for operation,value in (('approve',2),('deny',9)):
                name='tool_'+operation+'_turn'
                statement(app,'CREATE AI CONVERSATION fixture.'+name+' USING MODEL fixture.tool_model WITH FUNCTIONS (PROCEDURE fixture.tool_approved_update)')
                statement(app,'START AI CONVERSATION fixture.'+name)
                statement(app,"APPEND PROMPT 'Fixture approval selection - no external provider'")
                statement(app,'APPEND RESPONSE AS TEXT')
                receipt=document(app,'COMMIT ASYNC RETURNING RUN INTO @approval_run')
                statement(app,'FINISH')
                run_id=receipt['run_id']
                claim=document(agent,'CALL sdp_ai.claim_run(%s,3720)',(worker_id,))
                require(claim['run_id']==run_id and len(claim['tools'])==1 and claim['tools'][0]['approval_required'] is True,
                    'required approval was absent from the actual pinned tool allowlist')
                descriptor=claim['tools'][0]
                calls=[dict(provider_call_id='approval-provider-call',provider_name=descriptor['provider_name'],arguments=dict(p=value))]
                publication=document(agent,'CALL sdp_ai.publish_tool_requests(%s,%s,%s,%s,CAST(%s AS JSON),CAST(%s AS JSON))',
                    (run_id,worker_id,'d'*32,'Fixture requested action',json.dumps(calls),'{}'))
                require(publication['state']=='waiting_approval' and len(publication['tool_calls'])==1,
                    'provider-published tool request skipped the durable approval wait')
                call_id=publication['tool_calls'][0]['call_id']
                require(publication['tool_calls'][0]['state']=='waiting_approval' and
                        statement(agent,'CALL sdp_ai.claim_run(%s,3720)',(worker_id,))==[],
                    'worker claimed an unresolved approval as executable inference or SQL')
                print('PASS protected tool approval request: durable waiting state and no eligible worker claim',flush=True)
                action=document(app,'PREPARE ACTION FOR TOOL CALL %s RETURNING ID INTO @tool_action',(call_id,))
                action_id=action['action_id']
                require(action['state']=='prepared' and action['resource_scope']=='all_dependency_tables' and
                        statement(app,'SELECT @tool_action')==[(action_id,)],
                    'native tool action did not prepare its bounded resource snapshot on the owner session')
                description=document(app,'DESCRIBE ACTION %s',(action_id,))
                require(description['tool_call_id']==call_id and description['arguments']==dict(p=value),
                    'action preview did not bind the actual protected call arguments')
                try:document(app,'APPROVE ACTION %s',(action_id,))
                except mysql.connector.Error as error:require(error.errno==61081,'owner self-approval without the active approval role returned the wrong error')
                else:raise RuntimeError('tool owner approved its request without approval authority')
                resolved=document(review,operation.upper()+' ACTION %s',(action_id,))
                require(resolved==dict(action_id=action_id,state='approved' if operation=='approve' else 'denied'),
                    'native approver did not resolve the exact protected action')
                if operation=='approve':
                    tools=document(agent,'CALL sdp_ai.claim_run(%s,3720)',(worker_id,))
                    require(tools['run_id']==run_id and tools['phase']=='tools' and
                            tools['tool_calls_to_execute']==[dict(call_id=call_id,state='ready',ordinal=0)],
                        'approved action did not resume its original protected tool call')
                    document(prepared,'EXECUTE AI TOOL CALL ?',(call_id,))
                    status=document(agent,'CALL sdp_ai.tool_call_status(%s)',(call_id,))
                    require(status['tool_state']=='succeeded' and status['result']['result_sets'][0]['rows']==[[2,1]],
                        'approved native tool did not atomically publish its application result')
                    require(document(app,'DESCRIBE ACTION %s',(action_id,))['state']=='succeeded',
                        'approved action receipt was not committed with its tool result')
                    replay=document(prepared,'EXECUTE AI TOOL CALL ?',(call_id,))
                    require(replay['replayed'] is True,'approved action receipt was not idempotent')
                else:
                    status=document(agent,'CALL sdp_ai.tool_call_status(%s)',(call_id,))
                    require(status['tool_state']=='denied' and status['result'] is None,
                        'denied native tool was not terminal without application execution')
                require(query(instance,"SELECT CONCAT(quantity,':',effects) FROM fixture.tool_approval_rows")=='2:1',
                    'approval replay or denial executed application writes again')
                continuation=document(agent,'CALL sdp_ai.claim_run(%s,3720)',(worker_id,))
                require(continuation['run_id']==run_id and continuation['phase']=='inference' and
                        continuation['messages'][-1]['role']=='tool' and
                        continuation['messages'][-1]['tool_call_id']=='approval-provider-call',
                    'resolved tool approval did not produce the original typed provider continuation')
                document(agent,'CALL sdp_ai.publish_run(%s,%s,CAST(%s AS JSON),CAST(%s AS JSON),CAST(%s AS JSON))',
                    (run_id,worker_id,json.dumps(dict(format='TEXT',value='Fixture approval result')),'{}','null'))
                summary=document(app,'DESCRIBE CONVERSATION fixture.'+name)['tool_calls']
                require(len(summary)==1 and summary[0]['call_id']==call_id and summary[0]['state']==status['tool_state'],
                    'conversation approval summary did not match its terminal protected call')
                require(statement(agent,'SELECT STARDATAPARADISE_ACTOR(),CURRENT_USER(),@@session.time_zone')[0]==
                        ('tool_worker@localhost','tool_worker@localhost','+00:00'),
                    'approved action leaked owner identity or timezone into the worker session')
        finally:
            app.close();agent.close();review.close();prepared.close()
    finally:
        owner.close();worker.close();approver.close()
    print('PASS genuine tool approval: provider publication -> waiting -> owner preview -> active-role approve/deny -> native tool and atomic receipt')
    print('PASS tool approval continuation: no unresolved claims, no self-approval, idempotent writes, terminal denial and bounded conversation summary')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--catalog',type=Path,default=REPOSITORY/'compatibility/ai')
    parser.add_argument('--actions',type=Path)
    parser.add_argument('--sdk-python',type=Path,help='Optional installed SDK0.3.0 venv Python; real timeout call goes through gateway TLS.')
    args=parser.parse_args()
    catalog=args.catalog.resolve(strict=True);actions=(args.actions or catalog/'action_catalog.sql').resolve(strict=True)
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-tools-native') as root:
        cert=backend_certificate(MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe',root,'cert')
        with fixture.disposable(root/'server',tls_dir=cert,bootstrap=lambda instance:bootstrap(instance,catalog,actions),
                bootstrap_options=('--sdp-ai-sql=ON','--sdp-ai-actions=OFF'),
                server_options=('--sdp-ai-sql=ON','--sdp-ai-actions=ON')) as instance:
            exercise(instance,args.sdk_python.resolve(strict=True) if args.sdk_python else None,root)
            exercise_approval(instance)


if __name__=='__main__':main()
