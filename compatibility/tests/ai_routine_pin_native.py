"""Prove transitive routine DD pins during a bounded native worker RPC.

The disposable fixture installs a trusted test-only claim body which pauses
on an advisory lock. It deliberately does not call the two probed helpers:
their protection must come from the native fixed manifest, not ordinary CALL
prelocking. No application/provider work is performed.
"""
import argparse
import json
import threading
import time
from pathlib import Path

from .ai_catalog_acceptance import execute, install
from .ai_commit_guard_native import connection, statement
from .fixture import BASEDIR, BIN, MIGRATION, SHELL, Fixture, require
from .workbench_native import backend_certificate


def exercise(instance):
    import mysql.connector
    install(instance, False)
    execute(instance, """DROP PROCEDURE sdp_ai.claim_run;
      DELIMITER $$
      CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.claim_run(
        IN worker_id VARCHAR(32), IN lease_seconds INT) SQL SECURITY DEFINER
      BEGIN
        DO GET_LOCK('sdp_fixture_transitive_pin_pause',10);
        DO RELEASE_LOCK('sdp_fixture_transitive_pin_pause');
        SELECT JSON_OBJECT('fixture',TRUE) AS document;
      END$$
      DELIMITER ;
      GRANT EXECUTE ON PROCEDURE sdp_ai.claim_run TO 'sdp_ai_internal'@'localhost';""")
    worker, admin, ddl = connection(instance), connection(instance), connection(instance)
    thread = None
    try:
        agent, control, writer = worker.cursor(), admin.cursor(), ddl.cursor()
        try:
            statement(agent, 'SET SESSION sdp_ai_worker_rpc_guard=ON')
            worker_id = statement(agent, 'SELECT CONNECTION_ID()')[0][0]
            require(statement(control, "SELECT GET_LOCK('sdp_fixture_transitive_pin_pause',0)") == [(1,)],
                    'could not acquire fixture pause lock')
            outcome = {}

            def invoke():
                try:
                    outcome['rows'] = statement(agent, 'CALL sdp_ai.claim_run(%s,3720)', ('a' * 32,))
                except Exception as error:
                    outcome['error'] = error

            thread = threading.Thread(target=invoke, name='disposable-private-rpc-pin', daemon=True)
            thread.start()
            deadline = time.monotonic() + 8
            paused = False
            while time.monotonic() < deadline and thread.is_alive():
                rows = statement(control, 'SELECT STATE FROM information_schema.PROCESSLIST WHERE ID=%s', (worker_id,))
                if rows and 'user lock' in str(rows[0][0]).lower():
                    paused = True
                    break
                time.sleep(0.02)
            require(paused, 'RPC did not reach its bounded fixture pause: ' + repr(outcome))
            statement(writer, 'SET SESSION lock_wait_timeout=1')
            probes = [
                "ALTER FUNCTION sdp_ai.JSON_KIND COMMENT 'fixture normalized function key'",
                "ALTER PROCEDURE sdp_ai.ASSERT_WORKER COMMENT 'fixture normalized procedure key'",
            ]
            missing = []
            for source in probes:
                try:
                    statement(writer, source)
                except mysql.connector.Error as error:
                    require(error.errno == 1205, 'unexpected DDL pin result: ' + repr(error))
                else:
                    missing.append(source)
            locks = statement(control, """SELECT OBJECT_TYPE,OBJECT_SCHEMA,OBJECT_NAME,LOCK_TYPE,LOCK_STATUS
              FROM performance_schema.metadata_locks
              WHERE OWNER_THREAD_ID=(SELECT THREAD_ID FROM performance_schema.threads WHERE PROCESSLIST_ID=%s)
                AND OBJECT_SCHEMA='sdp_ai' AND OBJECT_TYPE IN ('FUNCTION','PROCEDURE')
              ORDER BY OBJECT_TYPE,OBJECT_NAME""", (worker_id,))
            print('Active native RPC routine locks: ' + json.dumps(locks, default=str))
            statement(control, "SELECT RELEASE_LOCK('sdp_fixture_transitive_pin_pause')")
            thread.join(8)
            require(not thread.is_alive(), 'RPC did not leave fixture pause')
            require('error' not in outcome, 'native fixture RPC failed: ' + repr(outcome))
            # Tickets must be released at RPC return, before provider I/O.
            for source in probes:
                statement(writer, source)
            require(not missing, 'transitive normalized routine MDL failed to exclude concurrent DDL: ' + repr(missing))
        finally:
            if thread is not None and thread.is_alive():
                try:
                    statement(control, "SELECT RELEASE_LOCK('sdp_fixture_transitive_pin_pause')")
                finally:
                    thread.join(12)
            agent.close()
            control.close()
            writer.close()
    finally:
        worker.close()
        admin.close()
        ddl.close()
    print('PASS normalized FUNCTION/PROCEDURE transitive DD pins exclude active-RPC DDL and release before provider I/O')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld', type=Path, default=BIN/'mysqld.exe')
    parser.add_argument('--mysql', type=Path, default=BIN/'mysql.exe')
    parser.add_argument('--basedir', type=Path, default=BASEDIR)
    args = parser.parse_args()
    fixture = Fixture(args.mysqld.resolve(strict=True), args.mysql.resolve(strict=True), args.basedir.resolve(strict=True), SHELL, None)
    with fixture.scratch('ai-routine-pin') as root:
        cert = backend_certificate(MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe', root, 'cert')
        with fixture.disposable(root/'server', tls_dir=cert, server_options=('--sdp-ai-sql=ON',)) as instance:
            exercise(instance)


if __name__ == '__main__':
    main()
