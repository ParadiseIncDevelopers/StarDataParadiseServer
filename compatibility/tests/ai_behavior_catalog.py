"""Pinned behavior staging, ordering, rollback, version and dependency gates."""
import argparse
import json
from pathlib import Path
from .ai_catalog_acceptance import call,execute,install,literal
from .fixture import BASEDIR,BIN,MIGRATION,SHELL,Fixture,query,require
from .workbench_native import backend_certificate


def exercise(instance,routines_only):
    install(instance,routines_only)
    execute(instance,call('create','model','m',dict(provider='OPENAI',remote_model='fixture',settings=dict(secret_env='FIXTURE_KEY'))))
    execute(instance,call('create','conversation','c',dict(model_schema='fixture',model_name='m',functions=0,auto_append=1,persistent=1)))
    execute(instance,call('create','behavior','b1',dict(definition='first-policy-original')))
    execute(instance,call('create','behavior','b2',dict(definition='second-policy')))
    execute(instance,call('alter','behavior','b1',dict(definition='first-policy-updated')))
    start=call('start','conversation','c') if routines_only else 'START AI CONVERSATION fixture.c;'
    commit='SET @fixture_transaction=1;START TRANSACTION;CALL sdp_ai.before_commit();COMMIT;CALL sdp_ai.after_commit();' if routines_only else 'COMMIT ASYNC;'
    bind1=call('bind_behavior','conversation',payload=dict(behavior_name='b1',version=1,priority=200))
    bind2=call('bind_behavior','conversation',payload=dict(behavior_name='b2',version=1,priority=100))
    execute(instance,start+bind1+call('finish','conversation'),61013)
    rollback='ROLLBACK;CALL sdp_ai.rollback_turn();' if routines_only else 'ROLLBACK;'
    execute(instance,start+bind1+rollback+call('finish','conversation'))
    require(query(instance,"SELECT COUNT(*) FROM sdp_ai.dependencies WHERE kind='behavior'")=='0','rolled-back behavior binding became durable')
    execute(instance,start+bind1+bind2+bind1+commit)
    require(query(instance,"SELECT COUNT(*) FROM sdp_ai.runs")=='0','behavior-only commit requested inference')
    document=json.loads(execute(instance,call('describe','conversation','c')))
    require(len(document['settings']['behaviors'])==2,'repeated insertion duplicated a behavior')
    require([b['priority'] for b in document['settings']['behaviors']]==[100,200],'behavior ordering was not normalized')
    require(document['version']==2 and len(document['versions'])==2,'binding did not create a durable object revision')
    execute(instance,call('drop','behavior','b1'),61041)
    execute(instance,start+call('append_prompt','conversation',payload=dict(value='use pinned policies'))+commit)
    worker='d'*32
    claim=json.loads(query(instance,"CALL sdp_ai.claim_run('%s',3720)"%worker))
    instructions=claim['instructions']
    require(instructions.index('second-policy')<instructions.index('first-policy-original') and 'first-policy-updated' not in instructions,
            'claim did not preserve pinned behavior version/priority')
    execute(instance,'CALL sdp_ai.publish_run(%s,%s,CAST(%s AS JSON),CAST(%s AS JSON),CAST(%s AS JSON));'%
      (literal(claim['run_id']),literal(worker),literal('{"format":"TEXT","value":"fixture answer"}'),literal('{}'),literal('null')))
    unbind=call('unbind_behavior','conversation',payload=dict(behavior_name='b1'))
    execute(instance,start+unbind+commit)
    execute(instance,call('drop','behavior','b1'))
    require(query(instance,"SELECT state FROM sdp_ai.objects WHERE kind='behavior' AND name='b1'")=='retired',
            'historical dependency incorrectly blocked removing a detached behavior')
    execute(instance,start+call('bind_behavior','conversation',payload=dict(behavior_name='b2',version=99,priority=1)),61051)
    execute(instance,start+call('bind_behavior','conversation',payload=dict(behavior_name='b2',version=1,priority='invalid')),61052)
    execute(instance,"CREATE USER behavior_other@localhost IDENTIFIED BY '' REQUIRE SSL;GRANT EXECUTE ON sdp_ai.* TO behavior_other@localhost;")
    env,command,cli=instance
    other=(env,['--user=behavior_other' if v=='--user=root' else v for v in command],cli)
    ticket=query(instance,"SELECT object_id FROM sdp_ai.objects WHERE kind='conversation' AND name='c'")
    spoof='SET @sdp_ai_session=JSON_OBJECT(\'conversation_id\',%s);'%literal(ticket)
    execute(other,spoof+bind2,61011)
    explicit=('SET @fixture_transaction=1;' if routines_only else '')+'START TRANSACTION;CALL sdp_ai.apply_behaviors(%s,JSON_ARRAY());'%literal(ticket)
    execute(other,explicit,61011)
    if routines_only:
        print('PASS behavior lifecycle: staged rollback, stable pins/priority, immutable metadata and authenticated owner boundary')
        print('Named metadata transactions: NOT_RUN (the old transaction shim cannot observe internal START TRANSACTION)')
        return
    # An explicitly named, inactive target is a short metadata transaction.
    execute(instance,call('create','conversation','named',dict(model_schema='fixture',model_name='m',auto_append=True)))
    named=call('bind_behavior','conversation','named',dict(behavior_name='b2',version=1,priority=15))
    result=json.loads(execute(instance,named))
    require(result['staged'] is False and result['behaviors'][0]['priority']==15,'named behavior binding was not durable')
    named_doc=json.loads(execute(instance,call('describe','conversation','named')))
    require(named_doc['version']==2 and len(named_doc['settings']['behaviors'])==1,'named binding lost its metadata version/dependency')
    execute(instance,call('unbind_behavior','conversation','named',dict(behavior_name='b2')))
    named_doc=json.loads(execute(instance,call('describe','conversation','named')))
    require(named_doc['version']==3 and named_doc['settings']['behaviors']==[],'named behavior removal was not durable')
    execute(instance,call('bind_behavior','conversation','named',dict(behavior_name='b2',version=99)),61051)
    require(json.loads(execute(instance,call('describe','conversation','named')))['version']==3,'failed named binding partially changed metadata')
    execute(other,named,61011)
    rejected=execute(instance,start+call('append_prompt','conversation',payload=dict(value='pending'))+named,61013)
    # The same active named target follows ordinary turn staging semantics.
    active_named=call('start','conversation','named')
    staged=json.loads(execute(instance,active_named+named).splitlines()[-1])
    require(staged['staged'] is True,'active named behavior target bypassed turn staging')
    print('PASS behavior lifecycle: staged rollback, stable pins/priority, idempotent membership, metadata versions and DROP dependency rules')
    print('PASS behavior worker snapshot and private-helper authenticated-owner boundary')
    print('PASS named behavior binding/removal, failure atomicity and active-target staging')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--routines-only',action='store_true')
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--openssl',type=Path,default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-behavior') as root:
        certificates=backend_certificate(args.openssl.resolve(strict=True),root,'cert')
        with fixture.disposable(root/'server',tls_dir=certificates,server_options=() if args.routines_only else ('--sdp-ai-sql=ON',)) as instance:
            exercise(instance,args.routines_only)


if __name__=='__main__':main()
