"""Durable tool batch SQL gates; native registration/execution is a separate gate."""
import argparse
import json
from pathlib import Path
from .ai_catalog_acceptance import call,execute,install,literal,catalog_source
from .fixture import BASEDIR,BIN,MIGRATION,REPOSITORY,SHELL,Fixture,query,require
from .workbench_native import backend_certificate


def exercise(instance,routines_only):
    install(instance,routines_only)
    source=(REPOSITORY/'compatibility/ai/tool_catalog.sql').read_text(encoding='utf-8')
    if routines_only:
        source=source.replace("Routine_type='FUNCTION' AND Routine_name IN (", "Routine_type='FUNCTION' AND Routine_name IN ('stardataparadise_actor','stardataparadise_transaction_active','stardataparadise_transaction_dirty',")
    execute(instance,source)
    execute(instance,"""CREATE USER tool_grant_app@localhost IDENTIFIED BY '' REQUIRE SSL;
      CREATE USER tool_grant_worker@localhost IDENTIFIED BY '' REQUIRE SSL;
      GRANT EXECUTE ON PROCEDURE sdp_ai.publish_tool_requests TO tool_grant_app@localhost;
      GRANT EXECUTE,ALTER ROUTINE ON PROCEDURE sdp_ai.claim_run TO tool_grant_worker@localhost WITH GRANT OPTION;
      GRANT EXECUTE ON PROCEDURE sdp_ai.publish_tool_requests TO tool_grant_worker@localhost;
      INSERT INTO sdp_ai.worker_accounts(actor) VALUES('tool_grant_worker@localhost');""")
    fingerprint="SELECT GROUP_CONCAT(CONCAT(User,'@',Host,':',Routine_name,':',Proc_priv) ORDER BY User,Host,Routine_name SEPARATOR '|') FROM mysql.procs_priv WHERE Db='sdp_ai' AND User IN ('tool_grant_app','tool_grant_worker')"
    before=query(instance,fingerprint)
    execute(instance,source)
    require(query(instance,fingerprint)==before,'optional tool reinstall removed or widened bounded routine rights')
    execute(instance,catalog_source(routines_only))
    execute(instance,source)
    require(query(instance,fingerprint)==before,'base+tools upgrade removed or widened bounded routine rights')
    env,command,cli=instance
    worker_instance=env,['--user=tool_grant_worker' if v=='--user=root' else v for v in command],cli
    execute(worker_instance,"CALL sdp_ai.claim_run(REPEAT('f',32),3720);")
    require(query(instance,"SELECT CONCAT(Super_priv,Grant_priv,File_priv) FROM mysql.user WHERE User='tool_grant_worker' AND Host='localhost'")=='NNN','tool upgrade widened worker global privileges')
    print('PASS tools and base+tools reinstall preserves exact application/worker EXECUTE, ALTER ROUTINE and per-routine grant option')
    execute(instance,call('create','model','m',dict(provider='OPENAI',remote_model='fixture',settings=dict(secret_env='FIXTURE_KEY'))))
    execute(instance,call('create','conversation','c',dict(model_schema='fixture',model_name='m',auto_append=True)))
    start=call('start','conversation','c') if routines_only else 'START AI CONVERSATION fixture.c;'
    commit='SET @fixture_transaction=1;START TRANSACTION;CALL sdp_ai.before_commit();COMMIT;CALL sdp_ai.after_commit();SET @fixture_transaction=0;' if routines_only else 'COMMIT ASYNC;'
    execute(instance,start+call('append_prompt','conversation',payload=dict(value='typed tools'))+commit)
    run_id=query(instance,'SELECT run_id FROM sdp_ai.runs')
    worker='7'*32
    snapshot=[dict(tool_id='a'*32,provider_name='tool_'+'a'*32,kind='knowledge',qualified_name='fixture.lookup',version=1,
                   definition_hash='b'*64,parameters=dict(type='object',properties=dict(id=dict(type='integer')),required=['id'],additionalProperties=False),approval_required=False),
              dict(tool_id='c'*32,provider_name='tool_'+'c'*32,kind='procedure',qualified_name='fixture.cancel',version=1,
                   definition_hash='d'*64,parameters=dict(type='object',properties=dict(id=dict(type='integer')),required=['id'],additionalProperties=False),approval_required=True)]
    # This is a protected admin fixture snapshot, not native DD/ACL acceptance.
    execute(instance,'INSERT INTO sdp_ai.run_tools(run_id,tools) VALUES(%s,CAST(%s AS JSON));'%(literal(run_id),literal(json.dumps(snapshot))))
    query(instance,"CALL sdp_ai.claim_run('%s',3720)"%worker)
    def publish(round_id,calls,metadata=None,error=None):
        source='CALL sdp_ai.publish_tool_requests(%s,%s,%s,%s,CAST(%s AS JSON),CAST(%s AS JSON));'%tuple(literal(v) for v in
            (run_id,worker,round_id,'assistant tools',json.dumps(calls),json.dumps(metadata or {})))
        return execute(instance,source,error)
    calls=[dict(provider_call_id='provider_call_a',provider_name=snapshot[0]['provider_name'],arguments=dict(id=7)),
           dict(provider_call_id='provider_call_b',provider_name=snapshot[1]['provider_name'],arguments=dict(id=8))]
    publish('1'*32,[dict(calls[0],provider_name='fixture.cancel')],error=61082)
    publish('2'*32,[calls[0],dict(calls[1],provider_name='tool_'+'e'*32)],error=61081)
    publish('3'*32,[calls[0],dict(calls[1],arguments=dict(id='bad'))],error=61023)
    require(query(instance,'SELECT COUNT(*) FROM sdp_ai.tool_calls')=='0','invalid sibling committed a partial tool batch')
    require(query(instance,'SELECT COUNT(*) FROM sdp_ai.tool_rounds')=='0','invalid batch left a round receipt')
    document=json.loads(publish('4'*32,calls,dict(provider_continuation=[dict(type='reasoning',encrypted_content='bounded-fixture')])) )
    require(document['state']=='waiting_approval' and len(document['tool_calls'])==2,'batch did not publish both durable tool requests')
    require(query(instance,'SELECT COUNT(*) FROM sdp_ai.tool_calls')=='2','atomic batch omitted a sibling')
    require(query(instance,"SELECT GROUP_CONCAT(state ORDER BY ordinal) FROM sdp_ai.tool_calls")=='ready,waiting_approval','approval was not kept separate from tool readiness')
    transcript=json.loads(query(instance,"SELECT JSON_EXTRACT(messages,'$[last]') FROM sdp_ai.runs"))
    require(transcript['role']=='assistant' and [c['id'] for c in transcript['tool_calls']]==['provider_call_a','provider_call_b'],
            'provider call identities were lost from the typed transcript')
    require(transcript['provider_continuation'][0]['encrypted_content']=='bounded-fixture','provider continuation was lost')
    require(json.loads(publish('4'*32,calls,dict(provider_continuation=[dict(type='reasoning',encrypted_content='bounded-fixture')])) )['replayed'],
            'identical batch retry did not return its durable receipt')
    publish('4'*32,[calls[0]],error=61091)
    require(query(instance,'SELECT COUNT(*) FROM sdp_ai.tool_calls')=='2','replay created duplicate calls')
    # Explicit administrator fixture releases the approval wait. This tests
    # storage/worker protocol only, not the future public APPROVE ACTION gate.
    execute(instance,"UPDATE sdp_ai.tool_calls SET state='ready' WHERE state='waiting_approval';UPDATE sdp_ai.run_tools SET phase='tools';")
    phase=json.loads(query(instance,"CALL sdp_ai.claim_run('%s',3720)"%worker))
    ids=[value['call_id'] for value in sorted(phase['tool_calls_to_execute'],key=lambda value:value['ordinal'])]
    require(len(ids)==2,'tool continuation lost sibling IDs')
    for value in ids:
        status=json.loads(query(instance,'CALL sdp_ai.tool_call_status(%s)'%literal(value)))
        require(status['action_commit_state']=='unknown' and status['replayed'] is False,'MVCC ready state falsely proved rollback')
    # An expired SQL-tool lease can recover the same protected call IDs. The
    # provider phase retains its separate no-blind-external-retry policy.
    execute(instance,"UPDATE sdp_ai.runs SET lease_until=TIMESTAMPADD(SECOND,-1,CURRENT_TIMESTAMP(6)) WHERE run_id=%s;"%literal(run_id))
    worker='8'*32
    recovered=json.loads(query(instance,"CALL sdp_ai.claim_run('%s',3720)"%worker))
    require(recovered['phase']=='tools' and [value['call_id'] for value in sorted(recovered['tool_calls_to_execute'],key=lambda value:value['ordinal'])]==ids,
            'SQL tool recovery replaced IDs or failed the run as an ambiguous provider call')
    def failure(call_id,document,error=None):
        return execute(instance,'CALL sdp_ai.publish_tool_failure(%s,%s,%s,CAST(%s AS JSON));'%
          (literal(run_id),literal(worker),literal(call_id),literal(json.dumps(document))),error)
    proof=dict(code=510005,native_errno=61085,phase='tool',run_id=run_id,call_id=ids[0],commit_state='committed',run_state='running',
               action_commit_state='rolled_back',unknown_outcome=False,cause_native_errno=3024,cause_sqlstate='HY000',message='Tool transaction rolled back.')
    failure(ids[0],dict(proof,unknown_outcome=True),61082)
    failure(ids[0],{key:value for key,value in proof.items() if key!='action_commit_state'},61082)
    failed=json.loads(failure(ids[0],proof))
    require(failed['state']=='running' and failed['action_commit_state']=='rolled_back','known rollback did not become a durable failed call receipt')
    count=query(instance,'SELECT JSON_LENGTH(messages) FROM sdp_ai.runs')
    require(json.loads(failure(ids[0],proof))['replayed'] is True,'failed call publication was not idempotent')
    require(query(instance,'SELECT JSON_LENGTH(messages) FROM sdp_ai.runs')==count,'failed receipt replay duplicated provider transcript')
    status=json.loads(query(instance,'CALL sdp_ai.tool_call_status(%s)'%literal(ids[0])))
    require(status['tool_state']=='failed' and status['action_commit_state']=='rolled_back' and status['error']['native_errno']==61085,'status lost durable rollback proof')
    second=json.loads(failure(ids[1],dict(proof,call_id=ids[1])))
    require(second['state']=='queued','completed tool error siblings did not resume inference')
    continuation=json.loads(query(instance,"CALL sdp_ai.claim_run('%s',3720)"%worker))
    require(continuation['phase']=='inference' and len([value for value in continuation['messages'] if value['role']=='tool'])==2,'continuation lost durable tool errors')
    require(query(instance,'SELECT COUNT(*) FROM fixture.ledger')=='0','storage fixture unexpectedly executed application SQL')
    print('PASS atomic tool request batch: pinned provider-name mapping, typed arguments, all-sibling rollback, approval state and replay')
    print('PASS durable assistant/tool-request transcript preserves provider call IDs and bounded continuation')
    print('PASS SQL tool receipt/status, strict rollback proof, idempotent failure publication and same-call-ID lease recovery')
    print('Native tool registration/execution, ACL, approval execution and Decision: NOT_RUN (SQL storage fixture only)')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--routines-only',action='store_true')
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--openssl',type=Path,default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-tools') as root:
        certificates=backend_certificate(args.openssl.resolve(strict=True),root,'cert')
        with fixture.disposable(root/'server',tls_dir=certificates,server_options=() if args.routines_only else ('--sdp-ai-sql=ON',)) as instance:
            exercise(instance,args.routines_only)


if __name__=='__main__':main()
