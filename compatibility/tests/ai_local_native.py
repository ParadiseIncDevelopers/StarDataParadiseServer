"""Real GGUF import -> native SQL turn -> internal queue -> inference -> sync capture.

No cloud credentials, transaction shims, fake model responses or user server.
Requires the pinned local runtime/model supplied through explicit arguments.
"""
import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import os
import secrets
import sys
from threading import Event,Thread
import time

from .ai_catalog_acceptance import install,execute
from .dotnet_native_acceptance import native_bridge
from .fixture import Fixture,BASEDIR,BIN,MIGRATION,REPOSITORY,SHELL,query,require,run
from compatibility.ai.local_model import LlamaRuntime
from compatibility.ai.worker import RunWorker
sys.path.insert(1,str(REPOSITORY/'tests'))
from service_smoke import certificates
from stardataparadise_sdk import connect_native,NativeJson,Error


class Rows:
    def __init__(self,rows):self.rows=iter(rows)
    def fetch_one(self):return next(self.rows,None)


class NativeWorkerSession:
    def __init__(self,connection):self.connection=connection
    def run_sql(self,sql,parameters=()):
        result=self.connection.execute(sql,parameters)
        rows=[]
        for part in result.sets:
            rows.extend(tuple(value.text if isinstance(value,NativeJson) else value for value in row) for row in part.rows)
        return Rows(rows)


def first(connection,sql,parameters=()):
    for part in connection.execute(sql,parameters).sets:
        if part.rows:return part.rows[0]
    return None


def document(connection,sql,parameters=()):
    value=first(connection,sql,parameters)[0]
    return value.decode() if isinstance(value,NativeJson) else json.loads(value)


def full_profile_bootstrap(instance):
    """Install in the explicit maintenance profile before the owned restart."""
    install(instance,False)
    for name in ('tool_catalog.sql','action_catalog.sql'):
        execute(instance,(REPOSITORY/'compatibility/ai'/name).read_text(encoding='utf-8'))


def check(fixture,root,args):
    front,back=root/'front',root/'back'
    front.mkdir();back.mkdir()
    openssl=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe'
    for location in (front,back):certificates(str(openssl),location)
    owner,worker='local_ai_owner','local_ai_worker'
    password=secrets.token_hex(24)
    full_profile=getattr(args,'full_profile',False)
    options=dict(server_options=('--sdp-ai-sql=ON',))
    if full_profile:
        options.update(bootstrap=full_profile_bootstrap,
            bootstrap_options=('--sdp-ai-sql=ON','--sdp-ai-actions=OFF'),
            server_options=('--sdp-ai-sql=ON','--sdp-ai-actions=ON'))
    with fixture.disposable(root/'server',tls_dir=back,**options) as instance:
        if full_profile:
            require(query(instance,'SELECT @@global.sdp_ai_sql,@@global.sdp_ai_actions')=='1\t1',
                    'full-profile local acceptance did not enable both native startup options')
            print('PASS full AI profile: core/tool/action catalogs bootstrapped in maintenance and restarted with AI/actions ON',flush=True)
        else:
            install(instance,False)
        setup=f"""ALTER TABLE fixture.ledger ADD COLUMN note TEXT;
CREATE USER '{owner}'@'localhost' IDENTIFIED BY '{password}' REQUIRE SSL;
CREATE USER '{worker}'@'localhost' IDENTIFIED BY '{password}' REQUIRE SSL;
GRANT SELECT,INSERT,UPDATE,DELETE ON fixture.ledger TO '{owner}'@'localhost';
INSERT INTO sdp_ai.worker_accounts(actor) VALUES('{worker}@localhost');
"""
        for routine in ('dispatch','before_commit','after_commit','poll_commit','rollback_turn','knowledge_document'):
            setup+=f"GRANT EXECUTE ON PROCEDURE sdp_ai.{routine} TO '{owner}'@'localhost';\n"
        for routine in ('claim_model_import','renew_model_import','publish_model_import','claim_run','renew_lease','publish_run'):
            setup+=f"GRANT EXECUTE ON PROCEDURE sdp_ai.{routine} TO '{worker}'@'localhost';\n"
        execute(instance,setup)
        environment,classic,_=instance
        port=next(value.split('=',1)[1] for value in classic if value.startswith('--port='))
        uuid,version=query(instance,'SELECT @@server_uuid,@@version;').split('\t')
        command=[sys.executable,'-B',str(REPOSITORY/'compatibility/wire_v1/service.py'),'--port','0','--cert',str(front/'server.pem'),
            '--key',str(front/'server.key'),'--mysql-port',port,'--mysql-ca',str(back/'ca.pem'),'--mysql-uuid',uuid,
            '--product-version',version.removesuffix('-StarDataParadise'),'--schema','fixture','--native-sql']
        bridge_environment=environment.copy()
        bridge_environment['PYTHONPATH']=str(MIGRATION/'wire-v1/dependencies')+os.pathsep+bridge_environment.get('PYTHONPATH','')
        with native_bridge(command,bridge_environment,root) as gateway:
            def connect(user=owner):return connect_native(port=gateway,user=user,password=password,ca_file=str(front/'ca.pem'),tls_name='localhost',timeout=75)
            configuration={'local_runtime':{'executable':str(args.runtime),'model_store':str(root/'models'),
                'import_roots':{owner+'@localhost':str(args.model.parent)},'threads':2,'memory_limit_bytes':1024**3}}
            observed=Event()
            class ObservedRuntime(LlamaRuntime):
                @contextmanager
                def start(self,*arguments,**options):
                    with super().start(*arguments,**options) as process:
                        if observed.is_set():
                            with connect() as observer:
                                require(first(observer,'SELECT note FROM fixture.ledger WHERE id=1')==('committed before inference',),'business row not committed before provider call')
                                observer.begin()
                                observer.execute('SET innodb_lock_wait_timeout=1')
                                observer.execute('UPDATE fixture.ledger SET note=note WHERE id=1')
                                observer.rollback()
                            observed.clear()
                        yield process
            with connect() as application,connect(worker) as worker_connection:
                imported=document(application,'IMPORT MODEL fixture.local_llm FROM FILE ? WITH(runtime=\'llama.cpp\',sha256=?,context_tokens=512)',
                                  (args.model.name,args.sha256))
                require(imported['state']=='pending_import','import did not produce pending job')
                provider_worker=RunWorker(NativeWorkerSession(worker_connection),configuration,runtime_factory=ObservedRuntime)
                result=provider_worker.work_once()
                require(result and result['state']=='succeeded','real import/load publication failed')
                require(document(application,'DESCRIBE MODEL fixture.local_llm')['state']=='active','verified local model not activated')
                print('PASS native SQL local GGUF import, internal import queue and real runtime-load publication',flush=True)
                application.execute('CREATE AI CONVERSATION fixture.local_chat USING MODEL fixture.local_llm NO FUNCTIONS AUTO APPEND MESSAGE MEMORY PERSISTENT')
                application.execute('START AI CONVERSATION fixture.local_chat')
                # Supplied JSON is a local terminal result. It must stay typed
                # when captured, nested and inserted, without provider I/O.
                application.execute('APPEND RESPONSE INTO @value AS JSON RETURNING METADATA INTO @metadata')
                application.execute('APPEND RESPONSE CAST(? AS JSON) AS JSON',('{"id":9007199254740993,"empty":"","nothing":null}',))
                try:first(application,'SELECT @value')
                except Error as failure:require(failure.native_errno==61025,'pending capture did not reject reads')
                else:raise RuntimeError('pending capture was usable before commit')
                application.commit()
                captured=first(application,"SELECT @value,JSON_OBJECT('nested',@value),@metadata")
                require(all(isinstance(value,NativeJson) for value in captured),'native JSON capture metadata lost')
                require(captured[0].decode()['id']==9007199254740993 and isinstance(captured[1].decode()['nested'],dict),'JSON capture was quoted or rounded')
                require(captured[2].decode()['source']=='application','supplied value caused provider inference')
                print('PASS native pending-read guard and true JSON supplied-response capture over SDK/TLS',flush=True)
                application.begin();application.execute('INSERT INTO fixture.ledger VALUES(1,?)',('committed before inference',))
                application.execute("APPEND PROMPT 'Once upon a time there was a little cat'")
                application.execute("APPEND RESPONSE INTO @answer AS TEXT WITH(max_output_tokens=128,temperature=0,stop=JSON_ARRAY('.')) RETURNING METADATA INTO @provider_metadata")
                observed.set()
                stop,failures=Event(),[]
                def work():
                    try:
                        with connect(worker) as connection:
                            active=RunWorker(NativeWorkerSession(connection),configuration,runtime_factory=ObservedRuntime)
                            while not stop.is_set():
                                if active.work_once() is None:stop.wait(.05)
                    except Exception as failure:failures.append(type(failure).__name__+': '+str(failure).replace(password,'<redacted>'))
                thread=Thread(target=work,daemon=True);thread.start()
                try:
                    receipt=application.commit(wait_timeout_ms=20000)
                    require(not failures,'real local worker failed: '+str(failures))
                    answer,metadata=first(application,'SELECT @answer,@provider_metadata')
                    require(isinstance(answer,str) and bool(answer.strip()),'no real model answer captured')
                    require(isinstance(metadata,NativeJson) and metadata.decode()['provider']=='local_gguf','provider metadata lost')
                    require(not observed.is_set(),'no independent observer lock/commit check ran')
                    require(not receipt.in_transaction,'synchronous response retained caller transaction')
                    print('PASS native SQL -> internal AI queue -> real local GGUF inference -> sync TEXT/JSON metadata capture; no business locks during runtime I/O',flush=True)
                finally:
                    stop.set();thread.join(timeout=35)
                    require(not thread.is_alive(),'test worker did not stop')
                # ASYNC cannot capture a future value or commit business rows
                # before that validation error is returned.
                application.begin();application.execute("INSERT INTO fixture.ledger VALUES(2,'must roll back')")
                application.execute("APPEND PROMPT 'next'")
                application.execute('APPEND RESPONSE INTO @future AS TEXT')
                try:application.commit(asynchronous=True)
                except Error as failure:require(failure.native_errno==61005 and failure.commit_state=='not_committed','ASYNC capture rejected after committing')
                else:raise RuntimeError('ASYNC accepted session capture')
                application.rollback()
                require(first(application,'SELECT @future,COUNT(*) FROM fixture.ledger WHERE id=2')==(None,0),'rollback retained capture or business row')
                print('PASS ASYNC capture precommit rejection preserves atomic business rollback and clears pending targets',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--runtime',type=Path,required=True)
    parser.add_argument('--model',type=Path,required=True)
    parser.add_argument('--sha256',required=True)
    parser.add_argument('--full-profile',action='store_true',
        help='Bootstrap core/tool/action catalogs with actions OFF, then restart the owned fixture with AI SQL/actions ON.')
    args=parser.parse_args()
    args.runtime=args.runtime.resolve(strict=True);args.model=args.model.resolve(strict=True)
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-local-native') as root:check(fixture,root,args)


if __name__=='__main__':main()
