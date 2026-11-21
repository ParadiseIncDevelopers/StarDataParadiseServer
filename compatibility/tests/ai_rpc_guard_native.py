"""Native worker RPC and producer private-catalog boundary on a fresh TLS Server."""
import argparse
import json
from pathlib import Path
from .ai_catalog_acceptance import call,execute,install
from .fixture import BASEDIR,BIN,MIGRATION,SHELL,Fixture,query,require,run
from .workbench_native import backend_certificate


def exercise(instance):
    import mysql.connector
    abi_failures=[]
    def negative(target,source,expected):
        result=run(target[1],target[0],60,script=source)
        require(result.returncode!=0,'guard unexpectedly accepted negative SQL: '+source)
        if 'ERROR '+str(expected)+' ' not in result.stderr:
            abi_failures.append({'expected':expected,'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
    install(instance,False)
    execute(instance,"""CREATE USER guard_worker@localhost IDENTIFIED BY '' REQUIRE SSL;
      CREATE USER guard_namespace@localhost IDENTIFIED BY '' REQUIRE SSL;
      GRANT EXECUTE ON PROCEDURE sdp_ai.claim_run TO guard_worker@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.dispatch TO guard_worker@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.assert_worker TO guard_worker@localhost;
      GRANT INSERT ON fixture.ledger TO guard_namespace@localhost;
      INSERT INTO sdp_ai.worker_accounts(actor) VALUES('guard_worker@localhost');
      DELIMITER $$
      CREATE FUNCTION fixture.rpc_argument_write(p INT) RETURNS VARCHAR(32) SQL SECURITY DEFINER MODIFIES SQL DATA
      BEGIN INSERT INTO fixture.ledger VALUES(p);RETURN REPEAT('a',32);END$$
      CREATE PROCEDURE sdp_ai.not_a_worker_rpc() SQL SECURITY DEFINER
      BEGIN INSERT INTO fixture.ledger VALUES(901);END$$
      DELIMITER ;
      GRANT EXECUTE ON FUNCTION fixture.rpc_argument_write TO guard_worker@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.not_a_worker_rpc TO guard_worker@localhost;""")
    env,command,cli=instance
    worker_command=[('--user=guard_worker' if v.startswith('--user=') else v) for v in command]
    worker_instance=env,worker_command,cli
    execute(worker_instance,"SET SESSION sdp_ai_worker_rpc_guard=ON;CALL sdp_ai.claim_run('"+'a'*32+"',3720);")
    execute(worker_instance,'SET SESSION sdp_ai_worker_rpc_guard=ON;SET SESSION sdp_ai_worker_rpc_guard=OFF;',1231)
    execute(worker_instance,'SET SESSION sdp_ai_worker_rpc_guard=ON;SET SESSION sdp_ai_worker_rpc_guard=DEFAULT;',1231)
    for source in (call('list','model'),'CALL sdp_ai.assert_worker();','CALL sdp_ai.not_a_worker_rpc();'):
        negative(worker_instance,'SET SESSION sdp_ai_worker_rpc_guard=ON;'+source,61081)
    negative(worker_instance,'SET SESSION sdp_ai_worker_rpc_guard=ON;CALL sdp_ai.claim_run(fixture.rpc_argument_write(900),3720);',61006)
    require(query(instance,'SELECT COUNT(*) FROM fixture.ledger')=='0','worker argument policy evaluated an application writing function')
    # The dedicated worker opt-in does not change ordinary connection policy.
    execute(worker_instance,call('list','model'))

    options={v[2:].split('=',1)[0]:v.split('=',1)[1] for v in command if v.startswith('--') and '=' in v}
    connection=mysql.connector.connect(host='127.0.0.1',port=int(options['port']),user='guard_worker',password='',
      ssl_ca=options['ssl-ca'],ssl_verify_cert=True,ssl_verify_identity=True,autocommit=True,use_pure=True)
    try:
        control=connection.cursor();control.execute('SET SESSION sdp_ai_worker_rpc_guard=ON');control.close()
        prepared=connection.cursor(prepared=True)
        try:
            def claim():
                prepared.execute('CALL sdp_ai.claim_run(?,?)',('b'*32,3720))
                if prepared.with_rows:prepared.fetchall()
                while prepared.nextset():
                    if prepared.with_rows:prepared.fetchall()
            claim()
            # No explicit guard ticket may survive an RPC merely because its
            # prepared statement handle remains open during provider I/O.
            execute(instance,'SET SESSION lock_wait_timeout=2;ALTER PROCEDURE sdp_ai.assert_worker SQL SECURITY INVOKER;')
            try:claim()
            except mysql.connector.Error as error:require(error.errno==61081,'prepared RPC did not revalidate routine security: '+str(error))
            else:raise RuntimeError('changed SQL SECURITY was trusted by a cached prepared RPC')
            execute(instance,'ALTER PROCEDURE sdp_ai.assert_worker SQL SECURITY DEFINER;')
            claim()
            execute(instance,"""SET SESSION lock_wait_timeout=2;DROP PROCEDURE sdp_ai.assert_worker;
              DELIMITER $$
              CREATE DEFINER='guard_namespace'@'localhost' PROCEDURE sdp_ai.assert_worker() SQL SECURITY DEFINER
              BEGIN INSERT INTO fixture.ledger VALUES(902);END$$
              DELIMITER ;
              GRANT EXECUTE ON PROCEDURE sdp_ai.assert_worker TO 'sdp_ai_internal'@'localhost';""")
            try:claim()
            except mysql.connector.Error as error:require(error.errno==61081,'prepared RPC trusted a namespace replacement: '+str(error))
            else:raise RuntimeError('namespace-authored worker helper was executed')
            execute(instance,'SHOW MODELS;',61081)
            require(query(instance,'SELECT COUNT(*) FROM fixture.ledger')=='0','untrusted helper ran under a worker or native producer')
        finally:prepared.close()
    finally:connection.close()
    print('PASS native RPC: monotonic dedicated-session opt-in, bounded public entries, nested helper policy and pre-evaluation argument rejection')
    print('PASS native RPC: prepared metadata revalidation, private definer/security drift and no guard MDL retained across idle provider intervals')
    print('PASS native producer: transitive private helper replacement rejected before SQL execution')
    require(not abi_failures,'Native guard error ABI mismatches (negative protection was exercised; the gate remains FAILED): '+json.dumps(abi_failures,ensure_ascii=False))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--openssl',type=Path,default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-rpc-guard') as root:
        cert=backend_certificate(args.openssl.resolve(strict=True),root,'cert')
        with fixture.disposable(root/'server',tls_dir=cert,server_options=('--sdp-ai-sql=ON',)) as instance:exercise(instance)


if __name__=='__main__':main()
