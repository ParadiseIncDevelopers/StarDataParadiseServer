"""Conversation metadata preserves durable context, pinned deps and user DML."""
import argparse
import json
from pathlib import Path
from .ai_catalog_acceptance import call,execute,install,literal
from .fixture import BASEDIR,BIN,MIGRATION,SHELL,Fixture,query,require
from .workbench_native import backend_certificate


def exercise(instance):
    install(instance,False)
    for name in ('m1','m2'):
        execute(instance,call('create','model',name,dict(provider='OPENAI',remote_model=name,settings=dict(secret_env='FIXTURE_KEY'))))
    execute(instance,call('create','response_schema','shape',dict(version=1,schema={'type':'object'})))
    execute(instance,call('create','conversation','c',dict(model_schema='fixture',model_name='m1',auto_append=False)))
    execute(instance,call('create','conversation','collision',dict(model_schema='fixture',model_name='m1')))
    ticket=query(instance,"SELECT object_id FROM sdp_ai.objects WHERE kind='conversation' AND name='c'")
    # A context-only turn must not schedule external inference.
    execute(instance,"START AI CONVERSATION fixture.c;APPEND PROMPT 'retained';SET MEMORY n=18446744073709551615 PERSISTENT;COMMIT ASYNC;FINISH;")
    old=json.loads(query(instance,"SELECT JSON_OBJECT('messages',messages,'memory',memory,'epoch',epoch) FROM sdp_ai.conversations WHERE conversation_id=%s"%literal(ticket)))
    execute(instance,call('alter','conversation','c',dict(auto_append=True,persistent=False,model_schema='fixture',model_name='m2',response=dict(format='JSON',schema='shape',schema_version=1))))
    doc=json.loads(execute(instance,call('describe','conversation','c')))
    require(doc['id']==ticket and doc['version']==2 and doc['settings']['auto_append'] is True and doc['settings']['persistent'] is False,'ALTER lost identity or typed settings')
    require(doc['settings']['response']['schema']=='fixture.shape','default response schema was not resolved in the object namespace')
    current=json.loads(query(instance,"SELECT JSON_OBJECT('messages',messages,'memory',memory,'epoch',epoch) FROM sdp_ai.conversations WHERE conversation_id=%s"%literal(ticket)))
    require(old==current,'ALTER reset durable messages, memory or epoch')
    execute(instance,call('drop','model','m2'),61041)
    execute(instance,call('drop','response_schema','shape'),61041)
    for payload,error in ((dict(model_name='m1'),61005),(dict(response=dict(format='TEXT',schema='shape',schema_version=1)),61005),(dict(functions=True),61072),(dict(unknown=1),61001)):
        execute(instance,call('alter','conversation','c',payload),error)
    require(json.loads(execute(instance,call('describe','conversation','c')))['version']==2,'failed ALTER leaked a version')
    execute(instance,call('rename','conversation','c',dict(name='collision')),61014)
    renamed=json.loads(execute(instance,call('rename','conversation','c',dict(name='renamed'))))
    require(renamed['id']==ticket and renamed['version']==3,'rename replaced the stable ticket')
    execute(instance,"START AI CONVERSATION fixture.c;",61011)
    # Catalog metadata inside an existing transaction must roll back with the
    # caller, without an implicit commit of unrelated application writes.
    execute(instance,'START TRANSACTION;INSERT INTO fixture.ledger VALUES(101);'+call('rename','conversation','renamed',dict(name='rollback_name'))+'ROLLBACK;')
    require(query(instance,'SELECT COUNT(*) FROM fixture.ledger')=='0','metadata rename committed caller DML')
    require(query(instance,"SELECT name FROM sdp_ai.objects WHERE object_id=%s"%literal(ticket))=='renamed','metadata rollback retained the rename')
    archived=json.loads(execute(instance,call('archive','conversation','renamed')))
    require(archived['state']=='archived' and archived['id']==ticket,'archive removed conversation identity')
    execute(instance,"START AI CONVERSATION fixture.renamed;",61011)
    execute(instance,call('start_ticket','conversation',payload=dict(value=ticket)),61011)
    execute(instance,call('drop','model','m2'),61041)
    restored=json.loads(execute(instance,call('unarchive','conversation','renamed')))
    require(restored['state']=='active' and restored['id']==ticket,'unarchive did not restore the same ticket')
    execute(instance,"START AI CONVERSATION fixture.renamed;FINISH;")
    # A different owner may neither change nor revive a conversation.
    execute(instance,"CREATE USER conversation_other@localhost IDENTIFIED BY '' REQUIRE SSL;GRANT EXECUTE ON sdp_ai.* TO conversation_other@localhost;")
    env,command,cli=instance
    other=(env,['--user=conversation_other' if value=='--user=root' else value for value in command],cli)
    execute(other,call('rename','conversation','renamed',dict(name='stolen')),61011)
    execute(other,call('archive','conversation','renamed'),61011)
    print('PASS conversation metadata: ALTER defaults/model pins, schema RESTRICT deps, stable rename/archive/revive, failure atomicity and authenticated owner checks')
    print('PASS conversation metadata: durable context preserved and caller business transaction rollback preserved')
    print('Native ALTER/rename/archive syntax: NOT_RUN by this SQL-entry suite; covered separately by ai_context_native')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--openssl',type=Path,default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-conversation') as root:
        cert=backend_certificate(args.openssl.resolve(strict=True),root,'cert')
        with fixture.disposable(root/'server',tls_dir=cert,server_options=('--sdp-ai-sql=ON',)) as instance:exercise(instance)


if __name__=='__main__':main()
