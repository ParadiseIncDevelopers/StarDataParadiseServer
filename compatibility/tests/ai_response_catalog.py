"""AI response request/supplied/capture and AUTO generation lifecycle gates."""
import argparse
import json
from pathlib import Path

from .ai_catalog_acceptance import call,execute,install,literal
from .fixture import BASEDIR,BIN,MIGRATION,SHELL,Fixture,query,require,run
from .workbench_native import backend_certificate


def exercise(instance,routines_only):
    install(instance,routines_only)
    execute(instance,"UPDATE performance_schema.setup_consumers SET ENABLED='YES' WHERE NAME='events_statements_history_long';")
    def checked_query(source):
        observed=run([*instance[1],'--execute='+source],instance[0],30)
        require(observed.returncode==0,observed.stdout+observed.stderr)
        if observed.stdout.strip():return observed.stdout.strip()
        diagnostic=dict(sql=source,returncode=observed.returncode,stdout=observed.stdout,stderr=observed.stderr)
        try:
            import mysql.connector
            options={v[2:].split('=',1)[0]:v.split('=',1)[1] for v in instance[1] if v.startswith('--') and '=' in v}
            connection=mysql.connector.connect(host='127.0.0.1',port=int(options['port']),user='root',password='',
              ssl_ca=options['ssl-ca'],ssl_verify_cert=True,ssl_verify_identity=True,autocommit=True,use_pure=True)
            try:
                cursor=connection.cursor()
                cursor.execute('SELECT COUNT(*) FROM sdp_ai.runs');diagnostic['connector_run_count']=cursor.fetchall()
                cursor.execute('SELECT THREAD_ID,SQL_TEXT,ROWS_SENT,MYSQL_ERRNO,RETURNED_SQLSTATE,ERRORS FROM performance_schema.events_statements_history_long WHERE SQL_TEXT=%s ORDER BY TIMER_START DESC LIMIT 3',(source,))
                diagnostic['server_statement_history']=cursor.fetchall();cursor.close()
            finally:connection.close()
        except Exception as failure:diagnostic['diagnostic_error']=str(failure)
        raise RuntimeError('Unexpected empty classic query result; no command was retried: '+repr(diagnostic))
    execute(instance,call('create','model','m',dict(provider='OPENAI',remote_model='fixture',settings=dict(secret_env='FIXTURE_KEY'))))
    execute(instance,call('create','conversation','c',dict(model_schema='fixture',model_name='m',functions=0,auto_append=1,persistent=1)))
    start=call('start','conversation','c') if routines_only else 'START AI CONVERSATION fixture.c;'
    commit='SET @fixture_transaction=1;START TRANSACTION;CALL sdp_ai.before_commit();COMMIT;CALL sdp_ai.after_commit();' if routines_only else 'COMMIT;'
    def ready(value,fmt='JSON',source_json=True,options=None,capture=None,sql_null=False):
        if routines_only:
            payload=dict(format=fmt,options=options or {},provided=dict(value=value,source_json=source_json,source_sql_null=sql_null))
            if capture:payload['capture']=capture
            return call('append_ready','conversation',payload=payload)
        expression='NULL' if sql_null else ('CAST(%s AS JSON)'%literal(json.dumps(value)) if source_json else literal(value))
        target=' INTO @answer' if capture and 'value_target' in capture else ''
        # Supplied syntax does not need an INTO value target: metadata capture
        # is tested on a request separately below; direct local result uses FETCH.
        require(not target,'native supplied value capture uses request surface')
        source='APPEND RESPONSE %s AS %s'%(expression,fmt)
        if options:
            source+=' WITH('+','.join(k+'='+(literal(v) if isinstance(v,str) else json.dumps(v)) for k,v in options.items())+')'
        if capture and 'metadata_target' in capture:
            source+=' RETURNING METADATA INTO @meta'
        return source+';'
    cases=[(None,'JSON',True,None),(True,'JSON',True,True),
           (18446744073709551615,'JSON',True,18446744073709551615),
           ({},'JSON',True,{}),('actual JSON string','JSON',True,'actual JSON string'),
           ('"encoded JSON string"','JSON',False,'encoded JSON string'),
           ('','TEXT',False,''),('Türkçe supplied','TEXT',False,'Türkçe supplied')]
    for value,fmt,source_json,expected in cases:
        before=int(checked_query('SELECT COUNT(*) FROM sdp_ai.runs'))
        result=execute(instance,start+ready(value,fmt,source_json)+commit)
        run_id=checked_query('SELECT run_id FROM sdp_ai.runs ORDER BY created_at DESC,run_id DESC LIMIT 1')
        document=json.loads(execute(instance,call('fetch_response','run',payload=dict(value=run_id))))
        require(document['result']==dict(format=fmt,value=expected),'supplied response changed JSON/TEXT/NULL identity')
        require(int(checked_query('SELECT COUNT(*) FROM sdp_ai.runs'))==before+1,'supplied response created duplicate/no local run')
        require(checked_query("SELECT COUNT(*) FROM sdp_jobs.compute_jobs WHERE state='queued'")=='0','supplied response scheduled inference')
    execute(instance,start+ready(None,'JSON',False,sql_null=True),61026)
    execute(instance,start+ready('invalid JSON','JSON',False),61022)
    execute(instance,start+ready('too long','TEXT',False,options=dict(max_output_bytes=1))+commit,61024)

    # AUTO disabled commits the prompt without creating a provider run.
    execute(instance,call('create','conversation','manual',dict(model_schema='fixture',model_name='m',functions=0,auto_append=0,persistent=1)))
    manual=call('start','conversation','manual') if routines_only else 'START AI CONVERSATION fixture.manual;'
    prompt=call('append_prompt','conversation',payload=dict(value='manual prompt')) if routines_only else "APPEND PROMPT 'manual prompt';"
    before=checked_query('SELECT COUNT(*) FROM sdp_ai.runs')
    execute(instance,manual+prompt+commit)
    require(checked_query('SELECT COUNT(*) FROM sdp_ai.runs')==before,'AUTO disabled generated a response')
    request=call('response_options','conversation',payload=dict(format='TEXT',options={})) if routines_only else 'APPEND RESPONSE AS TEXT;'
    execute(instance,manual+request+('SET @sdp_ai_commit_mode=\'async\';'+commit if routines_only else 'COMMIT ASYNC;'))
    claim=json.loads(checked_query("CALL sdp_ai.claim_run('%s',3720)"%('a'*32)))
    require(claim['messages'][-1]['content']=='manual prompt','explicit response lost committed prompt context')
    execute(instance,'CALL sdp_ai.publish_run(%s,%s,CAST(%s AS JSON),CAST(%s AS JSON),CAST(%s AS JSON));'%
      (literal(claim['run_id']),literal('a'*32),literal('{"format":"TEXT","value":"manual answer"}'),literal('{}'),literal('null')))
    require(checked_query("SELECT JSON_UNQUOTE(JSON_EXTRACT(messages,'$[last].role')) FROM sdp_ai.conversations WHERE conversation_id=(SELECT conversation_id FROM sdp_ai.runs WHERE run_id=%s)"%literal(claim['run_id']))=='assistant',
            'manual response was not retained as an assistant message')

    # Target names are bounded and cannot overwrite native/session internals.
    cap=dict(format='TEXT',value_target=dict(kind='user_variable',name='answer'),metadata_target=dict(kind='user_variable',name='meta'))
    for bad,error in [(dict(cap,value_target=dict(kind='user_variable',name='sdp_ai_session')),61026),
                      (dict(cap,metadata_target=dict(kind='user_variable',name='ANSWER')),61005)]:
        execute(instance,start+call('response_options','conversation',payload=dict(format='TEXT',options={},capture=bad)),error)
    async_request=call('response_options','conversation',payload=dict(format='TEXT',options={},capture=cap)) if routines_only else 'APPEND RESPONSE INTO @answer AS TEXT RETURNING METADATA INTO @meta;'
    execute(instance,start+async_request+('SET @sdp_ai_commit_mode=\'async\';SET @fixture_transaction=1;START TRANSACTION;CALL sdp_ai.before_commit();' if routines_only else 'COMMIT ASYNC;'),61005)
    metadata_cap=dict(format='JSON',metadata_target=dict(kind='user_variable',name='meta'))
    result=execute(instance,start+ready({},'JSON',True,capture=metadata_cap)+commit)
    if routines_only:
        document=json.loads(result.splitlines()[-1].split('\t')[-1])
        require(document['capture']==metadata_cap,'after_commit lost captured metadata target')
    else:
        # Native helper assigns typed JSON metadata in the still-open session.
        checked=execute(instance,start+ready({},'JSON',True,capture=metadata_cap)+commit+"SELECT JSON_TYPE(@meta),JSON_UNQUOTE(JSON_EXTRACT(@meta,'$.source'));")
        require(checked.splitlines()[-1]=='OBJECT\tapplication','native metadata capture was not a JSON object')
    print('PASS supplied responses: SQL NULL rejected, JSON null/string/scalar/object and empty TEXT preserved, no provider inference')
    print('PASS response lifecycle: AUTO disabled, explicit request, assistant history, capture targets and ASYNC precommit rejection')
    print('Native response grammar/capture: '+('NOT_RUN (explicit routines-only shims)' if routines_only else 'PASS'))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--routines-only',action='store_true')
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--openssl',type=Path,default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-response') as root:
        certificates=backend_certificate(args.openssl.resolve(strict=True),root,'cert')
        with fixture.disposable(root/'server',tls_dir=certificates,server_options=() if args.routines_only else ('--sdp-ai-sql=ON',)) as instance:
            exercise(instance,args.routines_only)


if __name__=='__main__':main()
