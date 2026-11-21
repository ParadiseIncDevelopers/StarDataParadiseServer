"""Prepared native COMMIT helper identity and post-commit wait boundaries.

Uses only disposable TLS data. The provider result is explicitly a fixture;
this test does not contact an external model or claim a live provider test.
"""
import argparse
import json
import threading
import time
from pathlib import Path

from .ai_catalog_acceptance import call, catalog_source, execute, install
from .fixture import BASEDIR, BIN, MIGRATION, SHELL, Fixture, require
from .workbench_native import backend_certificate


def drain(cursor):
    rows = cursor.fetchall() if cursor.with_rows else []
    while cursor.nextset():
        if cursor.with_rows:
            rows.extend(cursor.fetchall())
    return rows


def connection(instance):
    import mysql.connector
    options = {part[2:].split('=', 1)[0]: part.split('=', 1)[1]
               for part in instance[1] if part.startswith('--') and '=' in part}
    return mysql.connector.connect(host='127.0.0.1', port=int(options['port']),
        user='root', password='', ssl_ca=options['ssl-ca'],
        ssl_verify_cert=True, ssl_verify_identity=True, autocommit=True,
        use_pure=True, connection_timeout=5)


def statement(cursor, source, parameters=None):
    cursor.execute(source, parameters)
    return drain(cursor)


def original_raise_error():
    source = catalog_source(False)
    start = source.index("CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.raise_error(")
    end = source.index('END$$', start) + len('END$$')
    return ('DELIMITER $$\nDROP PROCEDURE sdp_ai.raise_error$$\n' + source[start:end] +
            "\nDELIMITER ;\nGRANT EXECUTE ON PROCEDURE sdp_ai.raise_error TO 'sdp_ai_internal'@'localhost';")


def exercise(instance):
    import mysql.connector
    install(instance, False)
    execute(instance, """CREATE TABLE fixture.guard_side_effect(id INT PRIMARY KEY) ENGINE=MyISAM;
      CREATE USER guard_commit_namespace@localhost IDENTIFIED BY '' REQUIRE SSL;
      GRANT INSERT ON fixture.guard_side_effect TO guard_commit_namespace@localhost;""")
    execute(instance, call('create', 'model', 'guard_model', dict(provider='OPENAI',
        remote_model='fixture-model', settings=dict(secret_env='FIXTURE_PROVIDER_KEY'))))
    for name in ('prepared_guard', 'waiting_guard'):
        execute(instance, call('create', 'conversation', name, dict(model_schema='fixture',
            model_name='guard_model', functions=False, auto_append=True, persistent=True)))

    app, admin = connection(instance), connection(instance)
    try:
        cursor, observer = app.cursor(), admin.cursor()
        try:
            statement(cursor, 'START AI CONVERSATION fixture.prepared_guard')
            statement(cursor, "APPEND PROMPT 'Pending turn - no provider request'")
            # A bad option reaches raise_error in dispatch before the physical
            # COMMIT hook. Merely guarding before_commit would be too late.
            prepared = app.cmd_stmt_prepare(b'COMMIT ASYNC WITH(guard_unknown=1)')
            try:
                execute(instance, """SET SESSION lock_wait_timeout=2;
                  DROP PROCEDURE sdp_ai.raise_error;
                  DELIMITER $$
                  CREATE DEFINER='guard_commit_namespace'@'localhost'
                    PROCEDURE sdp_ai.raise_error(IN p_name VARCHAR(64)) SQL SECURITY DEFINER
                  BEGIN
                    INSERT INTO fixture.guard_side_effect VALUES(401);
                    SIGNAL SQLSTATE '45000' SET MYSQL_ERRNO=61001,MESSAGE_TEXT='fixture replacement reached';
                  END$$
                  DELIMITER ;
                  GRANT EXECUTE ON PROCEDURE sdp_ai.raise_error TO 'sdp_ai_internal'@'localhost';""")
                try:
                    app.cmd_stmt_execute(prepared['statement_id'], data=(), parameters=())
                except mysql.connector.Error as error:
                    require(error.errno == 61081, 'prepared native COMMIT trusted changed helper: ' + str(error))
                else:
                    raise RuntimeError('prepared native COMMIT unexpectedly accepted changed helper')
                require(statement(observer, 'SELECT COUNT(*) FROM fixture.guard_side_effect') == [(0,)],
                    'untrusted pre-COMMIT helper evaluated an irreversible fixture write')
                require(statement(observer, 'SELECT COUNT(*) FROM sdp_ai.runs') == [(0,)],
                    'rejected prepared COMMIT published a run')
            finally:
                app.cmd_stmt_close(prepared['statement_id'])
                execute(instance, original_raise_error())
            statement(cursor, 'ROLLBACK')
            statement(cursor, 'FINISH')

            statement(cursor, 'START AI CONVERSATION fixture.waiting_guard')
            statement(cursor, 'START TRANSACTION')
            statement(cursor, 'INSERT INTO fixture.ledger VALUES(402)')
            statement(cursor, "APPEND PROMPT 'Fixture publication after catalog drift'")
            statement(cursor, 'APPEND RESPONSE AS TEXT WITH(provider_timeout_ms=1000,run_timeout_ms=10000)')
            outcome = {}

            def commit():
                try:
                    outcome['rows'] = statement(cursor, 'COMMIT WITH(wait_timeout_ms=10000)')
                except Exception as error:
                    outcome['error'] = error

            pending = threading.Thread(target=commit, name='disposable-ai-commit', daemon=True)
            pending.start()
            deadline, run_id = time.monotonic() + 8, None
            while time.monotonic() < deadline and pending.is_alive():
                found = statement(observer, 'SELECT run_id FROM sdp_ai.runs ORDER BY created_at DESC LIMIT 1')
                if found:
                    run_id = found[0][0]
                    if isinstance(run_id, bytes):
                        run_id = run_id.decode('ascii')
                    break
                time.sleep(0.02)
            require(run_id is not None, 'sync COMMIT did not publish a run before waiting: ' + repr(outcome))
            require(statement(observer, 'SELECT id FROM fixture.ledger') == [(402,)],
                'provider wait began before the local business transaction committed')
            require(pending.is_alive(), 'COMMIT ended before the provider-wait DD boundary was observed: ' + repr(outcome))
            # This DDL must complete while COMMIT is waiting. A routine MDL
            # ticket retained for the whole native producer would time out.
            statement(observer, 'SET SESSION lock_wait_timeout=2')
            try:
                statement(observer, 'ALTER PROCEDURE sdp_ai.assert_worker SQL SECURITY INVOKER')
                pending.join(timeout=8)
                require(not pending.is_alive(), 'catalog drift did not interrupt the synchronous wait')
                failure = outcome.get('error')
                require(isinstance(failure, mysql.connector.Error) and failure.errno == 61081,
                    'post-commit catalog drift did not report native identity failure: ' + repr(outcome))
                message = str(getattr(failure, 'msg', failure))
                require('510001:' in message, 'catalog identity error lost its public code: ' + message)
                document = json.loads(message.split('510001:', 1)[1].strip())
                require(document.get('phase') == 'after_commit' and document.get('commit_state') == 'committed' and document.get('unknown_outcome') is False,
                    'post-commit drift was described as an uncommitted or unknown local transaction: ' + message)
            finally:
                statement(observer, 'ALTER PROCEDURE sdp_ai.assert_worker SQL SECURITY DEFINER')
            require(statement(observer, 'SELECT id FROM fixture.ledger') == [(402,)],
                'post-commit catalog error changed committed business data')
            claimed = json.loads(statement(observer, "CALL sdp_ai.claim_run('" + 'c'*32 + "',3720)")[0][0])
            require(claimed['run_id'] == run_id, 'the wait failure lost the committed durable run')
            statement(observer, 'CALL sdp_ai.publish_run(%s,%s,CAST(%s AS JSON),CAST(%s AS JSON),CAST(%s AS JSON))',
                (run_id, 'c'*32, json.dumps(dict(format='TEXT', value='Fixture response - no external provider')), '{}', 'null'))
            statement(cursor, 'FINISH')
        finally:
            cursor.close()
            observer.close()
    finally:
        app.close()
        admin.close()
    print('PASS prepared native COMMIT: private helper revalidated before pre-COMMIT dispatch code; irreversible fixture ledger unchanged')
    print('PASS sync COMMIT: business transaction committed, helper DDL completed during provider wait, drift returned committed-state error')
    print('PASS fixture-only publication: committed run remained claimable and publishable after restoring trusted metadata')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld', type=Path, default=BIN/'mysqld.exe')
    parser.add_argument('--mysql', type=Path, default=BIN/'mysql.exe')
    parser.add_argument('--basedir', type=Path, default=BASEDIR)
    parser.add_argument('--openssl', type=Path, default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    args = parser.parse_args()
    fixture = Fixture(args.mysqld.resolve(strict=True), args.mysql.resolve(strict=True), args.basedir.resolve(strict=True), SHELL, None)
    with fixture.scratch('ai-commit-guard') as root:
        cert = backend_certificate(args.openssl.resolve(strict=True), root, 'cert')
        with fixture.disposable(root/'server', tls_dir=cert, server_options=('--sdp-ai-sql=ON',)) as instance:
            exercise(instance)


if __name__ == '__main__':
    main()
