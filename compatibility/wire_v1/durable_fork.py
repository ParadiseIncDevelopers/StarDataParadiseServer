"""Run wire receipt acceptance on an isolated local fork with freshly generated TLS material."""

import argparse
from contextlib import contextmanager
from pathlib import Path
import re
import secrets
import ssl
import subprocess
import sys
import time

CODE = Path(__file__).resolve().parent
sys.path.insert(1, str(CODE.parent / "tests"))
from fixture import BASEDIR, BIN, HIDDEN, MIGRATION, SHELL, Fixture, local_port, mysql_client, run, stop, wait_ready
from durable_acceptance import verify, verify_restart
from provision_ledger import provision
from service_smoke import certificates, require
from mysql import connector
from mysql.connector.errors import Error


@contextmanager
def start(command, env, log, cwd):
    with log.open('ab') as stream:
        process = subprocess.Popen(command, env=env, cwd=cwd, stdin=subprocess.DEVNULL,
                                   stdout=stream, stderr=subprocess.STDOUT, creationflags=HIDDEN)
        try:
            yield process
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


def check(options, test_server, instance):
    instance.mkdir()
    env = test_server.environment(instance)
    for name in ("front", "back"):
        location = instance / name
        location.mkdir()
        certificates(str(options.openssl), location)
    front, back = instance / 'front', instance / 'back'
    key = secrets.token_bytes(32)
    key_file = instance / 'ledger.key'
    key_file.write_text(key.hex(), encoding='ascii')
    owner, reader, limited = ((name, secrets.token_hex(24)) for name in ('wire_owner', 'wire_reader', 'wire_limited'))
    common = [str(test_server.mysqld), '--no-defaults', f'--basedir={test_server.basedir}', f'--datadir={instance / "data"}',
              '--console', '--no-monitor']
    initialize = run(common + ['--initialize-insecure'], env, 90)
    require(initialize.returncode == 0, 'fixture initialization failed')
    port = local_port()
    admin = mysql_client(test_server.mysql, ["--host=127.0.0.1", f"--port={port}", "--user=root",
                                              "--ssl-mode=VERIFY_IDENTITY", f"--ssl-ca={back / 'ca.pem'}"])
    command = [*common, '--bind-address=127.0.0.1', f'--port={port}', f'--log-bin={instance / "data" / "binlog"}',
               '--gtid-mode=ON', '--enforce-gtid-consistency=ON', f'--pid-file={instance / "mysqld.pid"}',
               '--loose-mysqlx=OFF', '--require-secure-transport=ON', f'--tmpdir={instance}',
               f'--ssl-ca={back / "ca.pem"}', f'--ssl-cert={back / "server.pem"}', f'--ssl-key={back / "server.key"}']

    def native(sql):
        result = run(admin, env, 20, script=sql)
        require(result.returncode == 0, 'fixture SQL failed: ' + result.stderr)
        return result.stdout.strip()

    def wire(uuid, version, log):
        args = [sys.executable, '-B', str(CODE / 'service.py'), '--port', '0', '--cert', str(front / 'server.pem'),
                '--key', str(front / 'server.key'), '--mysql-port', str(port), '--mysql-ca', str(back / 'ca.pem'),
                '--mysql-uuid', uuid, '--product-version', version, '--schema', 'wire_durable', '--ledger-key-file', str(key_file)]
        return start(args, env, log, instance)

    def wire_port(child, log):
        for _ in range(100):
            match = re.search(r'^SDP_WIRE_V1_READY (\d+)$', log.read_text(encoding='utf-8', errors='replace'), re.M)
            if match:
                return int(match[1])
            require(child.poll() is None, 'bridge exited')
            time.sleep(0.1)
        raise RuntimeError('bridge not ready')

    tls = ssl.create_default_context(cafile=str(front / 'ca.pem'))
    with start(command, env, instance / 'mysqld.log', instance) as server:
        wait_ready(server, admin, instance, env, 90)
        try:
            uuid, version = native('SELECT @@server_uuid, @@version;').split('\t')
            version = version.removesuffix('-StarDataParadise')
            native('CREATE DATABASE wire_durable;')
            for account in (owner, reader, limited):
                # Suppress credential-bearing bootstrap SQL errors in test output.
                created = run(admin, env, 10, script=f"CREATE USER '{account[0]}'@'localhost' IDENTIFIED BY '{account[1]}' REQUIRE SSL;")
                require(created.returncode == 0, 'fixture user creation failed')
            native('''
CREATE TABLE wire_durable.durable_sample(id BIGINT PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE wire_durable.durable_trigger(id BIGINT PRIMARY KEY) ENGINE=InnoDB;
CREATE TABLE wire_durable.nontransactional_direct(id BIGINT PRIMARY KEY) ENGINE=MyISAM;
CREATE TABLE wire_durable.nontransactional_trigger(id BIGINT PRIMARY KEY) ENGINE=MyISAM;
CREATE TRIGGER wire_durable.after_durable_trigger AFTER INSERT ON wire_durable.durable_trigger FOR EACH ROW
  INSERT INTO wire_durable.nontransactional_trigger VALUES (NEW.id);
GRANT SELECT, INSERT, UPDATE, DELETE ON wire_durable.* TO 'wire_owner'@'localhost', 'wire_limited'@'localhost';
GRANT SELECT ON wire_durable.* TO 'wire_reader'@'localhost';
''')
            try:
                with connector.connect(host="127.0.0.1", user="root", port=port, use_pure=True, autocommit=True,
                                       ssl_ca=str(back / "ca.pem"), ssl_verify_cert=True, ssl_verify_identity=True) as db:
                    provision(db, key, [name + "@localhost" for name, _ in (owner, reader)])
            except Error as error:
                raise RuntimeError(f"fixture ledger provisioning failed ({error.errno})") from None
            with connector.connect(host='127.0.0.1', user=owner[0], password=owner[1], port=port, use_pure=True,
                                   ssl_ca=str(back / 'ca.pem'), ssl_verify_cert=True, ssl_verify_identity=True) as db:
                with db.cursor(buffered=True) as cursor:
                    cursor.execute('SHOW CREATE PROCEDURE sdp_wire_v1.receipt')
                    require(cursor.fetchone()[2] is None, 'routine body, including its protected key, was exposed')
                    for sql, errno in [('SELECT * FROM performance_schema.session_connect_attrs', 1142),
                                       ('SELECT * FROM sdp_wire_v1.outcome', 1142),
                                       ("CALL sdp_wire_v1.receipt('41','probe',REPEAT('0',32),REPEAT('0',32),REPEAT('0',64),@result)",60002)]:
                        try:
                            cursor.execute(sql)
                        except connector.Error as err:
                            require(err.errno == errno, f'wrong classic denial ({err.errno}, {errno})')
                        else:
                            raise RuntimeError('directly authenticated account gained receipt access')
            print('PASS MySQL least privilege and caller proof', flush=True)
            first_log = instance / 'bridge.log'
            with wire(uuid, version, first_log) as bridge:
                recorded, expired = verify(wire_port(bridge, first_log), tls, owner, reader, limited)
            counts = native('SELECT (SELECT COUNT(*) FROM wire_durable.nontransactional_direct), (SELECT COUNT(*) FROM wire_durable.nontransactional_trigger), (SELECT COUNT(*) FROM wire_durable.durable_trigger), (SELECT COUNT(*) FROM sdp_wire_v1.anchor);')
            require(counts == '0\t0\t0\t0', 'unsafe table or orphan transaction anchor persisted: ' + counts)
            native(f"UPDATE sdp_wire_v1.outcome SET confirmed_at = UTC_TIMESTAMP(6) - INTERVAL 25 HOUR WHERE token = UNHEX('{expired}');")
        except BaseException:
            stop(server, admin, env, True)
            raise
        # wait_ready verified both the datadir and the PID of this isolated process.
        server.kill()
        require(server.wait(timeout=15) != 0, "test Server was expected to stop abruptly")
        stale_pid = instance / "mysqld.pid"
        if stale_pid.exists():
            require(stale_pid.read_text(encoding="ascii").strip() == str(server.pid), "unexpected PID file")
            stale_pid.unlink()
        print("PASS stopped the verified disposable fork without SQL shutdown", flush=True)
    with start(command, env, instance / 'mysqld.log', instance) as server:
        wait_ready(server, admin, instance, env, 90)
        try:
            second_log = instance / 'bridge-restarted.log'
            with wire(uuid, version, second_log) as bridge:
                verify_restart(wire_port(bridge, second_log), tls, owner, recorded, expired)
        finally:
            stop(server, admin, env, True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mysqld", type=Path, default=BIN / "mysqld.exe")
    parser.add_argument("--mysql", type=Path, default=BIN / "mysql.exe")
    parser.add_argument("--basedir", type=Path, default=BASEDIR)
    parser.add_argument("--openssl", type=Path, default=MIGRATION / "toolchain/openssl-firedaemon/x64/bin/openssl.exe")
    args = parser.parse_args()
    args.openssl = args.openssl.resolve(strict=True)
    server = Fixture(args.mysqld.resolve(strict=True), args.mysql.resolve(strict=True),
                     args.basedir.resolve(strict=True), SHELL, None)
    with server.scratch("wire-durable") as scratch:
        check(args, server, scratch / "fork")


if __name__ == "__main__":
    main()
