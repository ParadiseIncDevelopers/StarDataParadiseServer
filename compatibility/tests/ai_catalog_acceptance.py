"""Durable AI catalog/queue acceptance on a disposable TLS Server.

--routines-only tests SQL contracts with explicitly labelled test identity
shims on the old server. It does not claim native parser/transaction acceptance.
The default uses the native actor, transaction flag and SQL command grammar.
"""
import argparse
import json
from pathlib import Path

from .fixture import BASEDIR, BIN, MIGRATION, REPOSITORY, SHELL, Fixture, query, require, run
from .workbench_native import backend_certificate


def literal(value):
    return "CONVERT(X'%s' USING utf8mb4)" % value.encode('utf-8').hex()


def execute(instance, source, error=None):
    env, command, _ = instance
    result = run(command, env, 60, script=source)
    if error is None:
        require(result.returncode == 0, result.stdout + result.stderr)
    else:
        require(result.returncode != 0 and str(error) in result.stderr, result.stdout + result.stderr)
    return result.stdout.strip()


def call(operation, kind, name='', payload=None):
    return 'CALL sdp_ai.dispatch(%s,%s,%s,%s,CAST(%s AS JSON),@sdp_ai_test_output);' % tuple(literal(value) for value in
        (operation, kind, 'fixture', name, json.dumps(payload if payload is not None else {}, separators=(',', ':'))))


def catalog_source(routines_only):
    source=(REPOSITORY/'compatibility/ai/catalog.sql').read_text(encoding='utf-8')
    if routines_only:
        # Test-only identity/state shims must remain explicit even when the
        # selected binary also contains native builtins with the same names.
        for name in ('ACTOR','TRANSACTION_ACTIVE','TRANSACTION_DIRTY'):
            builtin='STARDATAPARADISE_'+name+'()'
            source=source.replace(builtin,'sdp_ai.'+builtin)
        source=source.replace("Routine_type='FUNCTION' AND Routine_name IN (",
            "Routine_type='FUNCTION' AND Routine_name IN ('stardataparadise_actor','stardataparadise_transaction_active','stardataparadise_transaction_dirty',")
        for name in ('ACTOR','TRANSACTION_ACTIVE','TRANSACTION_DIRTY'):
            source+="\nGRANT EXECUTE ON FUNCTION sdp_ai.STARDATAPARADISE_%s TO 'sdp_ai_internal'@'localhost';\n"%name
    return source


def install(instance, routines_only):
    execute(instance, """CREATE SCHEMA sdp_jobs;
      CREATE TABLE sdp_jobs.compute_jobs (
        sequence BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,job_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL UNIQUE,
        owner VARCHAR(576) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
        request_token CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,iterations INT UNSIGNED NOT NULL,timeout_ms INT UNSIGNED NOT NULL,
        state ENUM('queued','running','succeeded','failed','cancelled','interrupted') NOT NULL DEFAULT 'queued',cancel_requested BOOLEAN NOT NULL DEFAULT FALSE,
        checksum VARCHAR(20),error_code VARCHAR(32),attempt INT UNSIGNED NOT NULL DEFAULT 0,events JSON NOT NULL,
        job_kind VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT 'compute',payload JSON,
        UNIQUE KEY(owner,request_token),KEY(state,sequence)) ENGINE=InnoDB;
      CREATE SCHEMA fixture;CREATE TABLE fixture.ledger(id INT PRIMARY KEY) ENGINE=InnoDB;
    """)
    if routines_only:
        execute(instance, """CREATE SCHEMA sdp_ai;
          CREATE FUNCTION sdp_ai.STARDATAPARADISE_ACTOR() RETURNS VARCHAR(576) SQL SECURITY INVOKER NO SQL RETURN USER();
          CREATE FUNCTION sdp_ai.STARDATAPARADISE_TRANSACTION_ACTIVE() RETURNS BOOLEAN SQL SECURITY INVOKER NO SQL RETURN COALESCE(@fixture_transaction,0);
          CREATE FUNCTION sdp_ai.STARDATAPARADISE_TRANSACTION_DIRTY() RETURNS BOOLEAN SQL SECURITY INVOKER NO SQL RETURN COALESCE(@fixture_dirty,0);
        """)
    execute(instance,catalog_source(routines_only))
    execute(instance, "INSERT INTO sdp_ai.worker_accounts(actor) VALUES(sdp_ai.STARDATAPARADISE_ACTOR())" if routines_only else
            "INSERT INTO sdp_ai.worker_accounts(actor) VALUES(STARDATAPARADISE_ACTOR())")


def exercise(instance, routines_only):
    def ledger(expected,reason):
        observed=run(instance[1],instance[0],30,script='SELECT COUNT(*),COALESCE(GROUP_CONCAT(id ORDER BY id),\'empty\') FROM fixture.ledger;')
        actual=observed.stdout.strip()
        diagnostic={'returncode':observed.returncode,'stdout':observed.stdout,'stderr':observed.stderr}
        if observed.returncode!=0 or not actual.startswith(str(expected)+'\t'):
            # Independent read-only diagnostic: never repeat a mutating CALL or
            # turn a transport failure into a passing test through retry.
            try:
                import mysql.connector
                options={v[2:].split('=',1)[0]:v.split('=',1)[1] for v in instance[1] if v.startswith('--') and '=' in v}
                connection=mysql.connector.connect(host='127.0.0.1',port=int(options['port']),user='root',password='',
                  ssl_ca=options['ssl-ca'],ssl_verify_cert=True,ssl_verify_identity=True,autocommit=True,use_pure=True)
                try:
                    cursor=connection.cursor();cursor.execute('SELECT COUNT(*),COALESCE(GROUP_CONCAT(id ORDER BY id),\'empty\') FROM fixture.ledger')
                    diagnostic['connector_snapshot']=cursor.fetchall();cursor.close()
                finally:connection.close()
            except Exception as error:
                diagnostic['connector_diagnostic_error']=str(error)
            raise RuntimeError(reason+': '+repr(diagnostic))
    install(instance, routines_only)
    require(query(instance,"SELECT account_locked FROM mysql.user WHERE User='sdp_ai_internal' AND Host='localhost'")=='Y',
            'internal AI definer account can authenticate')
    require(query(instance,"SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA='sdp_ai' AND ROUTINE_NAME NOT LIKE 'STARDATAPARADISE_%' AND DEFINER<>'sdp_ai_internal@localhost'")=='0',
            'AI routine inherited the installer definer authority')
    require(query(instance,"SELECT CONCAT(Create_routine_priv,Grant_priv,Super_priv,File_priv,Select_priv) FROM mysql.user WHERE User='sdp_ai_internal' AND Host='localhost'")=='NNNNN',
            'internal AI account has global/root authority')
    require(query(instance,"SELECT COUNT(*) FROM mysql.global_grants WHERE USER='sdp_ai_internal' AND HOST='localhost'")=='0',
            'internal AI account has dynamic global privileges')
    require(query(instance,"SELECT sdp_ai.boolean_value(CAST('true' AS JSON),FALSE),sdp_ai.boolean_value(CAST('false' AS JSON),TRUE),sdp_ai.boolean_value(CAST('1' AS JSON),FALSE),sdp_ai.boolean_value(CAST('0' AS JSON),TRUE),sdp_ai.boolean_value(NULL,TRUE),sdp_ai.boolean_value(CAST('null' AS JSON),FALSE)")=='1\t0\t1\t0\t1\t0',
            'JSON booleans, legacy 0/1 or missing defaults changed')
    for boolean,error in [('"true"',61003),('2',61004),('[]',61003),('{}',61003)]:
        execute(instance,'SELECT sdp_ai.boolean_value(CAST(%s AS JSON),FALSE);'%literal(boolean),error)
    execute(instance, call('create','model','provider_model', dict(provider='OPENAI',remote_model='fixture-model',settings=dict(secret_env='FIXTURE_PROVIDER_KEY'))))
    execute(instance,call('create','model','claude_model',dict(provider='ANTHROPIC',remote_model='claude-fixture',settings=dict(secret_env='FIXTURE_CLAUDE_KEY',capabilities=['stop','tools']))))
    for removed in ('temperature','top_p'):
        execute(instance,call('create','model','invalid_claude_'+removed,dict(provider='ANTHROPIC',remote_model='claude-fixture',settings=dict(secret_env='FIXTURE_CLAUDE_KEY',capabilities=[removed]))),61072)
    execute(instance, call('create','conversation','conversation',dict(model_schema='fixture',model_name='provider_model',functions=0,auto_append=1,persistent=1)))
    negative = call('create','response_schema','unsafe_schema',dict(version=1,schema={'$ref':'https://example.invalid/remote'}))
    execute(instance, negative, 61006)
    execute(instance, call('create','response_schema','shape',dict(version=1,schema={'type':'object','properties':{'ok':{'type':'boolean'}},'required':['ok'],'additionalProperties':False})))
    if not routines_only:
        execute(instance,"""DELIMITER $$
          CREATE FUNCTION fixture.ai_argument_write(p INT) RETURNS VARCHAR(64) DETERMINISTIC MODIFIES SQL DATA
          BEGIN INSERT INTO fixture.ledger VALUES(p); RETURN CAST(p AS CHAR); END$$
          DELIMITER ;
        """)
        execute(instance,"CREATE MODEL fixture.argument_model TYPE GENERATIVE PROVIDER OPENAI MODEL fixture.ai_argument_write(900) WITH(secret_env='FIXTURE_PROVIDER_KEY');",61006)
        execute(instance,"IMPORT MODEL fixture.argument_import FROM FILE fixture.ai_argument_write(901) WITH(runtime='llama.cpp',sha256='%s');"%('c'*64),61006)
        ledger(0,'native catalog argument evaluated a writing function')
        staged="""START AI CONVERSATION fixture.conversation;APPEND PROMPT 'safe';
          START TRANSACTION;INSERT INTO fixture.ledger VALUES(903);
          COMMIT ASYNC WITH(request_key=fixture.ai_argument_write(902));
          SELECT COUNT(*) FROM fixture.ledger WHERE id=903;ROLLBACK;FINISH;"""
        rejected=run([*instance[1],'--force'],instance[0],30,script=staged)
        require('61006' in rejected.stderr and rejected.stdout.strip().endswith('1'),
                'native COMMIT option rejection evaluated an argument or lost existing user writes: stdout='+repr(rejected.stdout)+' stderr='+repr(rejected.stderr))
        ledger(0,'argument rejection committed business SQL')

    begin = call('start','conversation','conversation') + call('append_prompt','conversation',payload=dict(value="Türkçe ' prompt"))
    if routines_only:
        execute(instance, begin + 'SET @fixture_transaction=1;START TRANSACTION;INSERT INTO fixture.ledger VALUES(1);CALL sdp_ai.before_commit();ROLLBACK;CALL sdp_ai.rollback_turn();')
    else:
        execute(instance, "START AI CONVERSATION fixture.conversation;APPEND PROMPT 'Türkçe';START TRANSACTION;INSERT INTO fixture.ledger VALUES(1);ROLLBACK;FINISH;")
    require(query(instance,'SELECT COUNT(*) FROM sdp_ai.runs')=='0','rollback published an AI run')
    ledger(0,'rollback committed ordinary SQL')
    if routines_only:
        execute(instance, begin + 'SET @fixture_transaction=1;START TRANSACTION;INSERT INTO fixture.ledger VALUES(2);CALL sdp_ai.before_commit();COMMIT;CALL sdp_ai.after_commit();')
    else:
        execute(instance, "START AI CONVERSATION fixture.conversation;APPEND PROMPT 'Türkçe';START TRANSACTION;INSERT INTO fixture.ledger VALUES(2);COMMIT ASYNC;FINISH;")
    ledger(1,'ordinary SQL and AI commit were not atomic')
    require(query(instance,"SELECT COUNT(*) FROM sdp_jobs.compute_jobs WHERE job_kind='ai' AND state='queued'")=='1','AI did not use shared queue')
    worker='a'*32
    claim=json.loads(query(instance,"CALL sdp_ai.claim_run('%s',3720)" % worker))
    require(claim['model']['provider']=='openai' and claim['messages'][0]['role']=='user','provider snapshot differs')
    require(claim['model']['secret_env']=='FIXTURE_PROVIDER_KEY','credential reference was lost')
    require(query(instance,'SELECT @@autocommit')=='1','claim changed worker autocommit')
    response=dict(format='TEXT',value='Fixture response - no external request')
    source='CALL sdp_ai.publish_run(%s,%s,CAST(%s AS JSON),CAST(%s AS JSON),CAST(%s AS JSON));' % (
        literal(claim['run_id']),literal(worker),literal(json.dumps(response)),literal('{}'),literal('null'))
    published=json.loads(execute(instance,source)); require(published['state']=='succeeded','worker publication failed')
    replay=json.loads(execute(instance,source)); require(replay['replayed'] is True,'publication retry was not idempotent')
    require(query(instance,"SELECT COUNT(*) FROM sdp_ai.outbox WHERE event_kind='run_published'")=='1','publication replay duplicated outbox event')
    fetched=json.loads(execute(instance,call('fetch_response','run',payload=dict(value=claim['run_id']))))
    require(fetched['result']==response,'typed result was lost')
    wrong=source.replace(literal(worker),literal('b'*32));execute(instance,wrong,61092)
    bad_options=begin+call('append_response','conversation',payload=dict(value=dict(unknown=True)))+'SET @fixture_transaction=1;START TRANSACTION;CALL sdp_ai.before_commit();'
    execute(instance,bad_options,61001)
    run_count=query(instance,'SELECT COUNT(*) FROM sdp_ai.runs')
    require(run_count=='1','invalid option changed durable run count: '+repr(run_count))
    # Catalog revisions are immutable, and retired names can be created again.
    execute(instance,call('create','behavior','policy',dict(definition='İlk tanım')))
    execute(instance,call('alter','behavior','policy',dict(definition='İkinci tanım')))
    behavior=json.loads(execute(instance,call('describe','behavior','policy')))
    require(behavior['version']==2 and len(behavior['versions'])==2,'behavior version timeline was not durable')
    execute(instance,call('drop','behavior','policy'))
    execute(instance,call('create','behavior','policy',dict(definition='Yeni nesne')))
    execute(instance,call('create','model','missing_provider',dict(remote_model='fixture',settings=dict(secret_env='KEY'))),61072)
    execute(instance,call('create','model','raw_secret',dict(provider='OPENAI',remote_model='fixture',settings=dict(api_key='fixture-not-a-credential'))),61001)
    execute(instance,call('create','model','declared_caps',dict(provider='ANTHROPIC',remote_model='fixture',settings=dict(secret_env='KEY',capabilities=['stop','tools']))))
    caps=json.loads(execute(instance,call('describe','model','declared_caps')))['capabilities']
    require(caps==['stop','tools'],'declared provider capabilities were lost')
    execute(instance,call('create','model','bad_caps',dict(provider='OPENAI',remote_model='fixture',settings=dict(secret_env='KEY',capabilities=['seed']))),61072)
    execute(instance,call('create','model','duplicate_caps',dict(provider='OPENAI',remote_model='fixture',settings=dict(secret_env='KEY',capabilities=['tools','tools']))),61002)

    # The default DEFINER identity must never become the caller's authority.
    execute(instance,"CREATE USER ai_other@localhost IDENTIFIED BY '' REQUIRE SSL;GRANT EXECUTE ON sdp_ai.* TO ai_other@localhost;")
    env, command, cli=instance
    other=(env,['--user=ai_other' if value=='--user=root' else value for value in command],cli)
    execute(other,call('describe','model','provider_model'),61041)
    execute(other,"CALL sdp_ai.claim_run('%s',3720)" % worker,1142)
    require(execute(other,call('list','conversation'))=='','catalog leaked another authenticated actor')
    # A workflow/namespace routine author cannot replace internal code, choose
    # a privileged definer, read the private catalog or acquire a root context.
    execute(instance,'GRANT CREATE ROUTINE,ALTER ROUTINE ON fixture.* TO ai_other@localhost;')
    execute(other,"CREATE DEFINER='root'@'localhost' PROCEDURE fixture.forged_definer() SELECT 1;",1227)
    execute(other,'ALTER PROCEDURE sdp_ai.dispatch SQL SECURITY INVOKER;',1370)
    execute(other,'CREATE FUNCTION sdp_ai.injected() RETURNS INT DETERMINISTIC RETURN 1;',1044)
    execute(other,'SELECT document FROM sdp_ai.objects;',1142)
    execute(other,'CREATE PROCEDURE fixture.namespace_reader() SQL SECURITY DEFINER '+call('describe','model','provider_model'))
    execute(other,'CALL fixture.namespace_reader();',61041)
    # Rejection before a worker-owned transaction must not roll back the caller.
    reject_tx='SET @fixture_transaction=1;START TRANSACTION;INSERT INTO fixture.ledger VALUES(50);CALL sdp_ai.claim_run(%s,3720);SELECT COUNT(*) FROM fixture.ledger WHERE id=50;ROLLBACK;' % literal(worker)
    rejected=run([*command,'--force'],env,30,script=reject_tx)
    require('61013' in rejected.stderr and rejected.stdout.strip().endswith('1'),'worker precondition rejection rolled back caller business data')

    # Real worker import protocol: no artifact becomes active before a matching
    # trusted publication. This is metadata acceptance, not a file-import claim.
    sha='c'*64
    execute(instance,call('import','model','local_model',dict(artifact_source='fixture/model.gguf',settings=dict(runtime='llama.cpp',sha256=sha,context_tokens=4096,max_output_tokens=128))))
    local_claim=json.loads(query(instance,"CALL sdp_ai.claim_model_import('%s',3720)" % worker))
    renewed=json.loads(query(instance,"CALL sdp_ai.renew_model_import('%s','%s',3720)" % (local_claim['job_id'],worker)))
    require(renewed['cancel_requested'] is False,'import renewal cancellation flag is not typed boolean')
    require(local_claim['relative_source']=='fixture/model.gguf' and local_claim['sha256']==sha,'local import snapshot differs')
    require(type(local_claim['context_tokens']) is int and local_claim['context_tokens']==4096,'model import context_tokens lost its JSON integer type')
    require(local_claim['capabilities']==['temperature','top_p','seed','stop'],'verified local sampling defaults were missing from the import snapshot')
    require(query(instance,"SELECT state FROM sdp_ai.objects WHERE name='local_model'")=='pending_import','unverified local model was active')
    publication='CALL sdp_ai.publish_model_import(%s,%s,CAST(%s AS JSON),CAST(%s AS JSON));'
    bad_artifact=dict(artifact_id='d'*64,sha256='d'*64,bytes=24)
    execute(instance,publication % (literal(local_claim['job_id']),literal(worker),literal(json.dumps(bad_artifact)),literal('null')),61079)
    artifact=dict(artifact_id=sha,sha256=sha,bytes=24,gguf_version=3,tensor_count=0,metadata_count=0,runtime_verified=True)
    imported=json.loads(execute(instance,publication % (literal(local_claim['job_id']),literal(worker),literal(json.dumps(artifact)),literal('null'))))
    require(imported['state']=='succeeded','matching import publication failed')
    imported_again=json.loads(execute(instance,publication % (literal(local_claim['job_id']),literal(worker),literal(json.dumps(artifact)),literal('null'))))
    require(imported_again['replayed'] is True,'model import publication was not idempotent')
    local_document=json.loads(execute(instance,call('describe','model','local_model')))
    require(type(local_document['settings']['context_tokens']) is int and local_document['settings']['context_tokens']==4096 and
            type(local_document['settings']['max_output_tokens']) is int and local_document['settings']['max_output_tokens']==128,
            'published model numeric settings became JSON strings')

    # Lease recovery may report an unknown external outcome, never retry it.
    enqueue=begin+('SET @fixture_transaction=1;START TRANSACTION;CALL sdp_ai.before_commit();COMMIT;CALL sdp_ai.after_commit();' if routines_only else 'START TRANSACTION;COMMIT ASYNC;')
    execute(instance,enqueue)
    expired=json.loads(query(instance,"CALL sdp_ai.claim_run('%s',3720)" % worker))
    # Leases are UTC DATETIME values even when the administrator's session
    # uses a non-UTC timezone. Expire in that same clock domain.
    execute(instance,"SET SESSION time_zone='+05:30';UPDATE sdp_ai.runs SET lease_until=UTC_TIMESTAMP(6)-INTERVAL 1 SECOND WHERE run_id=%s" % literal(expired['run_id']))
    require(query(instance,"CALL sdp_ai.claim_run('%s',3720)" % worker)=='','expired run was silently requeued')
    require(query(instance,"SELECT JSON_UNQUOTE(JSON_EXTRACT(error,'$.unknown_outcome')) FROM sdp_ai.runs WHERE run_id=%s" % literal(expired['run_id']))=='true','lease recovery lost unknown-outcome marker')
    execute(instance,begin+call('commit_async','conversation',payload=dict(wait_timeout_ms=1000)),61005)
    execute(instance,begin+call('commit_async','conversation',payload=dict(request_key='invalid')),61004)
    execute(instance,begin+call('commit_sync','conversation',payload=dict(wait_timeout_ms=0)),61004)
    # Every staged turn component and dirty business writes must survive a
    # rejected FINISH/switch. Read-only manual transactions remain permitted.
    start=call('start','conversation','conversation')
    for pending in [call('request_response','conversation'),
                    call('append_context','conversation',payload=dict(value='policy')),
                    call('append_response','conversation',payload=dict(value=dict(format='JSON')))]:
        execute(instance,start+pending+call('finish','conversation'),61013)
        execute(instance,start+pending+start,61013)
    if routines_only:
        execute(instance,start+'SET @fixture_dirty=1;'+call('finish','conversation'),61013)
        execute(instance,start+'SET @fixture_dirty=1;'+start,61013)
        execute(instance,start+'SET @fixture_transaction=1;SET autocommit=0;SELECT 1;'+call('finish','conversation'))
    else:
        execute(instance,start+'START TRANSACTION;INSERT INTO fixture.ledger VALUES(70);'+call('finish','conversation'),61013)
        execute(instance,start+'START TRANSACTION;INSERT INTO fixture.ledger VALUES(71);'+start,61013)
        execute(instance,start+'SET autocommit=0;SELECT COUNT(*) FROM fixture.ledger;'+call('finish','conversation'))
    # JSON scalar capture remains JSON text, while TEXT capture remains raw text.
    json_begin=begin+call('append_response','conversation',payload=dict(value=dict(format='JSON')))
    json_commit='SET @fixture_transaction=1;START TRANSACTION;CALL sdp_ai.before_commit();COMMIT;CALL sdp_ai.after_commit();' if routines_only else 'START TRANSACTION;COMMIT ASYNC;'
    result_check='@sdp_ai_commit_result IS NULL' if routines_only else "JSON_VALID(@sdp_ai_commit_result) AND JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_commit_result,'$.state'))='queued' AND JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_commit_result,'$.run_id'))= @sdp_ai_committed_run_id"
    committed=execute(instance,json_begin+json_commit+"SELECT JSON_LENGTH(JSON_EXTRACT(@sdp_ai_session,'$.response')), "+result_check+';'+call('finish','conversation'))
    require(committed.splitlines()[-1]=='0\t1','committed response options leaked into the next turn or commit receipt was not initialized')
    json_claim=json.loads(query(instance,"CALL sdp_ai.claim_run('%s',3720)" % worker))
    json_result=dict(format='JSON',value='Türkçe scalar')
    execute(instance,'CALL sdp_ai.publish_run(%s,%s,CAST(%s AS JSON),CAST(%s AS JSON),CAST(%s AS JSON));' %
      (literal(json_claim['run_id']),literal(worker),literal(json.dumps(json_result)),literal('{}'),literal('null')))
    captured=execute(instance,call('fetch_response','run',payload=dict(value=json_claim['run_id']))+'SELECT JSON_QUOTE(@sdp_ai_test_output);').splitlines()[-1]
    require(json.loads(json.loads(captured))=='Türkçe scalar','JSON scalar OUT value was double encoded or unquoted')
    refusal_begin=begin+call('append_response','conversation',payload=dict(value=dict(format='JSON',schema='fixture.shape',schema_version=1)))
    execute(instance,refusal_begin+json_commit)
    refusal_claim=json.loads(query(instance,"CALL sdp_ai.claim_run('%s',3720)" % worker))
    messages_before=query(instance,"SELECT JSON_LENGTH(messages) FROM sdp_ai.conversations WHERE conversation_id=(SELECT conversation_id FROM sdp_ai.runs WHERE run_id=%s)" % literal(refusal_claim['run_id']))
    refused=json.loads(execute(instance,'CALL sdp_ai.publish_run(%s,%s,CAST(%s AS JSON),CAST(%s AS JSON),CAST(%s AS JSON));' %
      (literal(refusal_claim['run_id']),literal(worker),literal('{"format":"REFUSAL","value":null}'),literal('{}'),literal('null'))))
    require(refused['state']=='refused','provider refusal was reported as an ordinary successful response')
    require(query(instance,"SELECT JSON_LENGTH(messages) FROM sdp_ai.conversations WHERE conversation_id=(SELECT conversation_id FROM sdp_ai.runs WHERE run_id=%s)" % literal(refusal_claim['run_id']))==messages_before,'refusal appended an assistant answer')
    context_before=query(instance,"SELECT epoch,CAST(messages AS CHAR) FROM sdp_ai.conversations")
    runs_before_flush=query(instance,'SELECT COUNT(*) FROM sdp_ai.runs')
    flush_rollback=call('start','conversation','conversation')+call('flush','conversation')+(
        'SET @fixture_transaction=1;START TRANSACTION;CALL sdp_ai.before_commit();ROLLBACK;CALL sdp_ai.rollback_turn();'
        if routines_only else 'START TRANSACTION;ROLLBACK;')
    execute(instance,flush_rollback)
    require(query(instance,"SELECT epoch,CAST(messages AS CHAR) FROM sdp_ai.conversations")==context_before,
            'FLUSH rollback changed the durable epoch or provider context')
    memory=call('start','conversation','conversation')+call('flush','conversation')+json_commit+'SELECT @sdp_ai_last_run_id IS NULL,@sdp_ai_committed_run_id IS NULL;'
    require(execute(instance,memory).splitlines()[-1]=='1\t1','memory-only commit produced a JSON null string as a run ID')
    require(query(instance,'SELECT COUNT(*) FROM sdp_ai.runs')==runs_before_flush,
            'memory-only FLUSH created an inference run')
    require(query(instance,'SELECT MAX(JSON_LENGTH(messages)) FROM sdp_ai.conversations')=='0',
            'FLUSH MEMORY retained earlier messages in the active provider context')
    require(int(query(instance,'SELECT MAX(JSON_LENGTH(messages)) FROM sdp_ai.runs'))>0,
            'FLUSH MEMORY erased immutable run history')
    key='e'*32
    before_replay=int(query(instance,'SELECT COUNT(*) FROM sdp_ai.runs'))
    replay_commit=("SET @sdp_ai_session=JSON_SET(@sdp_ai_session,'$.request_key','%s');SET @fixture_transaction=1;START TRANSACTION;CALL sdp_ai.before_commit();COMMIT;CALL sdp_ai.after_commit();" % key) if routines_only else ("START TRANSACTION;COMMIT ASYNC WITH(request_key='%s');" % key)
    execute(instance,begin+replay_commit)
    execute(instance,begin+replay_commit)
    require(int(query(instance,'SELECT COUNT(*) FROM sdp_ai.runs'))==before_replay+1,'request key replay duplicated an AI run')
    post_flush=json.loads(query(instance,"CALL sdp_ai.claim_run('%s',3720)" % worker))
    require(len(post_flush['messages'])==1 and post_flush['messages'][0]['role']=='user',
            'post-FLUSH worker snapshot contained earlier turn messages')
    changed=call('start','conversation','conversation')+call('append_prompt','conversation',payload=dict(value='different prompt'))
    execute(instance,changed+replay_commit,61091)
    schema={'type':'object','properties':{'ok':{'type':'boolean'}},'required':['ok'],'additionalProperties':False}
    execute(instance,'CALL sdp_ai.validate_response(CAST(%s AS JSON),CAST(%s AS JSON));' % (literal('{"ok":true}'),literal(json.dumps(schema))))
    for value in [{'ok':'true'},{'ok':True,'extra':1},{}]:
        execute(instance,'CALL sdp_ai.validate_response(CAST(%s AS JSON),CAST(%s AS JSON));' % (literal(json.dumps(value)),literal(json.dumps(schema))),61023)
    execute(instance,'CALL sdp_ai.validate_response(CAST(%s AS JSON),CAST(%s AS JSON));' % (literal('1'),literal('{"const":true}')),61023)
    execute(instance,'CALL sdp_ai.validate_response(CAST(%s AS JSON),CAST(%s AS JSON));' % (literal('null'),literal('false')),61023)
    execute(instance,call('create','conversation','memory_scope',dict(model_schema='fixture',model_name='provider_model',functions=0,auto_append=1,persistent=1)))
    memory_start=call('start','conversation','memory_scope')
    persistent=call('set_memory','conversation',payload=dict(name='language',value='tr',persistent=True))
    temporary=call('set_memory','conversation',payload=dict(name='order_id',value=42,persistent=False))
    count_before_memory=query(instance,'SELECT COUNT(*) FROM sdp_ai.runs')
    memory_state=json.loads(execute(instance,memory_start+persistent+temporary+json_commit+call('memory','conversation')+call('finish','conversation')).splitlines()[-1])
    require(memory_state['memory']==dict(language='tr') and memory_state['temporary']==dict(order_id=42),
            'committed persistent/temporary memory scopes differ')
    require(query(instance,'SELECT COUNT(*) FROM sdp_ai.runs')==count_before_memory,'memory settings commit requested inference')
    restarted=json.loads(execute(instance,memory_start+call('memory','conversation')).splitlines()[-1])
    require(restarted['memory']==dict(language='tr') and restarted['temporary']=={},'temporary memory survived FINISH or persistent memory was lost')
    rolled=json.loads(execute(instance,memory_start+call('set_memory','conversation',payload=dict(name='language',value='wrong',persistent=True))+
        ('ROLLBACK;CALL sdp_ai.rollback_turn();' if routines_only else 'ROLLBACK;')+call('memory','conversation')).splitlines()[-1])
    require(rolled['memory']==dict(language='tr'),'rollback persisted memory changes')
    execute(instance,memory_start+call('append_prompt','conversation',payload=dict(value='read structured memory'))+json_commit)
    memory_claim=json.loads(query(instance,"CALL sdp_ai.claim_run('%s',3720)"%worker))
    require(memory_claim['instructions']=='' and '[Structured conversation memory data]' in memory_claim['messages'][0]['content'] and
            json.loads(memory_claim['messages'][0]['content'].split('\n',1)[1])['effective']==dict(language='tr'),
            'memory was missing from data context or became system instructions')
    previous=query(instance,'SELECT COUNT(*) FROM sdp_ai.objects')
    execute(instance,"""CREATE USER ai_upgrade_app@localhost IDENTIFIED BY '' REQUIRE SSL;
      CREATE USER ai_upgrade_worker@localhost IDENTIFIED BY '' REQUIRE SSL;
      GRANT EXECUTE ON PROCEDURE sdp_ai.dispatch TO ai_upgrade_app@localhost;
      GRANT EXECUTE,ALTER ROUTINE ON PROCEDURE sdp_ai.claim_run TO ai_upgrade_worker@localhost WITH GRANT OPTION;
      INSERT INTO sdp_ai.worker_accounts(actor) VALUES('ai_upgrade_worker@localhost');""")
    quoted_account="ai_upgrade_'\\"
    quoted_sql="'"+quoted_account.replace('\\','\\\\').replace("'","''")+"'@'localhost'"
    execute(instance,'CREATE USER '+quoted_sql+" IDENTIFIED BY '' REQUIRE SSL;GRANT EXECUTE ON PROCEDURE sdp_ai.dispatch TO "+quoted_sql+';')
    accounts="('ai_upgrade_app','ai_upgrade_worker',"+literal(quoted_account)+')'
    saved_grants=query(instance,'SELECT User,Host,Routine_name,Routine_type,Proc_priv FROM mysql.procs_priv WHERE User IN '+accounts+' ORDER BY User,Routine_name')
    execute(instance,catalog_source(routines_only))
    require(query(instance,'SELECT COUNT(*) FROM sdp_ai.objects')==previous,'catalog reinstall changed existing objects')
    require(query(instance,'SELECT User,Host,Routine_name,Routine_type,Proc_priv FROM mysql.procs_priv WHERE User IN '+accounts+' ORDER BY User,Routine_name')==saved_grants,
            'catalog reinstall removed or widened bounded application/worker routine grants')
    def as_account(account,source):
        command=[('--user='+account if value.startswith('--user=') else value) for value in instance[1]]
        result=run(command,instance[0],30,script=source)
        require(result.returncode==0,result.stdout+result.stderr)
    as_account('ai_upgrade_app',call('list','model'))
    as_account(quoted_account,call('list','model'))
    as_account('ai_upgrade_worker',"CALL sdp_ai.claim_run('%s',3720);"%('f'*32))
    require(query(instance,"SELECT CONCAT(Select_priv,Super_priv,Grant_priv) FROM mysql.user WHERE User='ai_upgrade_app' AND Host='localhost'")=='NNN',
            'routine grant restore widened application global authority')
    execute(instance,"SET sql_mode=CONCAT_WS(',',@@sql_mode,'NO_BACKSLASH_ESCAPES');"+catalog_source(routines_only))
    require(query(instance,'SELECT User,Host,Routine_name,Routine_type,Proc_priv FROM mysql.procs_priv WHERE User IN '+accounts+' ORDER BY User,Routine_name')==saved_grants,
            'NO_BACKSLASH_ESCAPES reinstall changed quoted account routine grants')
    as_account(quoted_account,call('list','model'))
    print('PASS catalog reinstall preserves exact bounded app/worker EXECUTE, ALTER ROUTINE and per-routine grant option')
    execute(instance,call('create','conversation','local_numeric',dict(model_schema='fixture',model_name='local_model',auto_append=True)))
    execute(instance,call('start','conversation','local_numeric')+call('append_prompt','conversation',payload=dict(value='typed numeric model settings'))+json_commit)
    numeric_claim=json.loads(query(instance,"CALL sdp_ai.claim_run('%s',3720)"%worker))
    require(type(numeric_claim['model']['context_tokens']) is int and numeric_claim['model']['context_tokens']==4096 and
            type(numeric_claim['model']['max_output_tokens']) is int and numeric_claim['model']['max_output_tokens']==128 and
            type(numeric_claim['options']['max_output_tokens']) is int and numeric_claim['options']['max_output_tokens']==128,
            'inference claim coerced numeric model settings/options into strings')
    print('PASS bounded SQL response schema validator: strict types, required/additional keys, const type and boolean schema')
    print('PASS AI SQL routines: durable queue, atomic rollback/commit, schema rejection, claim/publication CAS, replay and typed response')
    print('PASS AI SQL routines: immutable versions, authenticated ACL, deferred local import verification and no blind retry after lease expiry')
    print('Native grammar/transaction hooks: ' + ('NOT_RUN (explicit routines-only shims)' if routines_only else 'PASS'))
    print('External provider calls: NOT_RUN (SQL fixture publication only; no credentials)')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--routines-only',action='store_true')
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--openssl',type=Path,default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-catalog') as root:
        certificates=backend_certificate(args.openssl.resolve(strict=True),root,'cert')
        with fixture.disposable(root/'server',tls_dir=certificates,server_options=() if args.routines_only else ('--sdp-ai-sql=ON',)) as instance:
            exercise(instance,args.routines_only)


if __name__=='__main__':
    main()
