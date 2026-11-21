"""Native inspection metadata, source ranges, privilege checks and DD restart."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import subprocess
import time

from .classic_binary import binary_connection
from .fixture import BASEDIR, BIN, HIDDEN, HOST, OUT, SHELL, Fixture, local_port, mysql_client, require, stop, wait_ready
from .sql_short_native_acceptance import execute


def represented(rows, kind):
    return [row for row in rows if row[0] == kind]


def check_routine(client, name, expected_version):
    _, rows = client.query('SHOW EXPANDED PROCEDURE inspection_fixture.'+name)
    source = represented(rows, 'source_sql')
    require(len(source) == 1, 'routine source document missing')
    source_bytes = source[0][2].encode('utf-8')
    language = represented(rows, 'language_metadata')
    require(len(language) == 1 and f'version={expected_version} ' in language[0][2] and
            f'persisted={int(expected_version != 0)} ' in language[0][2], 'routine DD language version not preserved')
    code = represented(rows, 'compiler_ir')
    maps = {row[1]: json.loads(row[2]) for row in represented(rows, 'source_map')}
    require(bool(code) and len(code) == len(maps), 'compiler instructions lost source-map entries')
    fragments = []
    for row in code:
        mapping = maps[row[1]]
        require(mapping['available'] and mapping['unit'] == 'byte' and mapping['scope'] == 'stored_body',
                'native instruction has no real parser source range')
        first, last = mapping['start_byte'], mapping['end_byte']
        require(0 <= first < last <= len(source_bytes), 'stored instruction source range escaped the body')
        fragments.append(source_bytes[first:last].decode('utf-8'))
    if expected_version:
        require(any('LET şehir' in fragment for fragment in fragments), 'Unicode LET did not map to its source statement')
        require(any('FOR entry' in fragment for fragment in fragments), 'generated cursor code did not map to the FOR statement')
    return language[0][2], source[0][2]


def exercise(instance):
    execute(instance, '''CREATE SCHEMA inspection_fixture;
      CREATE TABLE inspection_fixture.sample(id INT PRIMARY KEY,a INT,label VARCHAR(30),secret TEXT) ENGINE=InnoDB;
      INSERT INTO inspection_fixture.sample VALUES(1,10,'İstanbul','hidden'),(2,20,'Ankara','hidden');
      DELIMITER //
      CREATE PROCEDURE inspection_fixture.short_program()
      AS
        LET şehir='İstanbul';
        LET total BIGINT=0;
        FOR entry IN SELECT id,a FROM inspection_fixture.sample DO
          WHEN :entry.id>0 DO SET total=total+:entry.a;
        END FOR;
        SELECT :şehir,:total;
      END//
      CREATE PROCEDURE inspection_fixture.legacy_program()
      BEGIN SELECT id FROM inspection_fixture.sample;END//
      DELIMITER ;
      CREATE USER limited_inspector@localhost IDENTIFIED BY '' REQUIRE SSL;
      GRANT EXECUTE ON PROCEDURE inspection_fixture.short_program TO limited_inspector@localhost;
    ''')
    sql = ('SHOW EXPANDED SELECT t.* EXCLUDE(secret) REPLACE(a+1 AS a),'
           ' LENGTH(label) AS letters FROM inspection_fixture.sample t')
    with binary_connection(instance) as client:
        _, rows = client.query(sql)
        require(represented(rows, 'source_sql') == [['source_sql', '0', sql]], 'query source bytes changed')
        maps = [json.loads(row[2]) for row in represented(rows, 'source_map') if row[1] != '0']
        require(len(maps) == 4 and all(value['available'] and 0 <= value['start_byte'] < value['end_byte'] <= len(sql.encode('utf-8'))
                                      for value in maps), 'native star/replacement/expression source positions missing')
        client.query('SET @inspection_hits=0')
        before = time.monotonic()
        client.query('SHOW EXPANDED SELECT (@inspection_hits:=@inspection_hits+1),SLEEP(5),RAND() FROM inspection_fixture.sample')
        require(time.monotonic()-before < 3 and client.query('SELECT @inspection_hits')[1] == [['0']],
                'inspection evaluated expressions or executed a query plan')

        statement, parameters, metadata = client.prepare(
            'SHOW EXPANDED SELECT * EXCLUDE(secret),? AS supplied FROM inspection_fixture.sample')
        require(parameters == 1 and [field['name'] for field in metadata] ==
                ['representation_kind', 'position', 'representation'], 'binary prepare exposed the inspected SELECT schema')
        execute(instance, 'ALTER TABLE inspection_fixture.sample ADD first_added INT DEFAULT 8')
        for ordinal, secret in enumerate(('never_print_sdp_secret_one', 'never_print_sdp_secret_two')):
            if ordinal:
                execute(instance, 'ALTER TABLE inspection_fixture.sample ADD second_added DECIMAL(8,2) DEFAULT 2.30')
            actual, rows = client.execute(statement, secret)
            require(actual == metadata, 'prepared inspection result metadata changed on execution/reprepare')
            require(not any(secret in (row[2] or '') for row in rows), 'inspection leaked the bound binary parameter value')
            fields = represented(rows, 'output_metadata')
            require(any(row[2].startswith('first_added type=') for row in fields), 'DDL before first EXECUTE left a stale bound AST')
            if ordinal:
                require(any(row[2].startswith('second_added type=') for row in fields), 'DDL between EXECUTEs left a stale bound AST')
        client.close_statement(statement)
        short = check_routine(client, 'short_program', 1)
        legacy = check_routine(client, 'legacy_program', 0)
        statement, parameters, metadata = client.prepare('SHOW EXPANDED PROCEDURE inspection_fixture.short_program')
        actual, rows = client.execute(statement)
        require(parameters == 0 and actual == metadata and bool(represented(rows, 'compiler_ir')),
                'routine binary prepare/execute metadata mismatch')
        client.close_statement(statement)

    env, command, shell = instance
    guest = (env, ['--user=limited_inspector' if argument == '--user=root' else argument for argument in command], shell)
    execute(guest, 'SHOW EXPANDED PROCEDURE inspection_fixture.short_program', 1227)
    with binary_connection(guest) as client:
        try:
            client.prepare('SHOW EXPANDED PROCEDURE inspection_fixture.short_program')
        except RuntimeError as error:
            require('1227' in str(error), 'binary prepare returned an unexpected privilege error')
        else:
            raise RuntimeError('binary prepare exposed routine inspection without SHOW_ROUTINE rights')
    print('PASS actual text/binary inspection metadata, AST source ranges, no execution/bind leaks, DDL reprepare and privileges', flush=True)
    return short, legacy


@contextmanager
def restart(fixture, root, env):
    """Restart only the initialized datadir created by this fixture's first run."""
    port = local_port()
    ca = root/'data/ca.pem'
    require(root.parent.parent == OUT/'compatibility-tests', 'unexpected restart directory')
    connection = [f'--host={HOST}', f'--port={port}', '--user=root', '--ssl-mode=VERIFY_CA', f'--ssl-ca={ca}']
    classic = mysql_client(fixture.mysql, connection)
    command = [str(fixture.mysqld), '--no-defaults', f'--basedir={fixture.basedir}', f'--datadir={root/"data"}',
               f'--bind-address={HOST}', f'--port={port}',
               f'--pid-file={root/"mysqld.pid"}', f'--tmpdir={root}', '--skip-log-bin', '--loose-mysqlx=OFF',
               '--local-infile=ON', '--require-secure-transport=ON']
    if os.name == 'nt':
        command.extend(('--console', '--no-monitor'))
    with (root/'restart.log').open('wb') as log:
        process = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, creationflags=HIDDEN)
        ready = False
        try:
            wait_ready(process, classic, root, env)
            ready = True
            yield env, classic, []
        finally:
            stop(process, classic, env, ready)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld', type=Path, default=BIN/'mysqld.exe')
    parser.add_argument('--mysql', type=Path, default=BIN/'mysql.exe')
    parser.add_argument('--basedir', type=Path, default=BASEDIR)
    args = parser.parse_args()
    fixture = Fixture(args.mysqld.resolve(strict=True), args.mysql.resolve(strict=True), args.basedir.resolve(strict=True), SHELL, None)
    with fixture.scratch('sql-short-inspection') as scratch:
        root = scratch/'server'
        with fixture.disposable(root) as instance:
            expected = exercise(instance)
            env = instance[0]
        with restart(fixture, root, env) as instance:
            with binary_connection(instance) as client:
                actual = (check_routine(client, 'short_program', 1), check_routine(client, 'legacy_program', 0))
                require(actual == expected, 'DD language/source metadata changed across a real server restart')
        print('PASS routine language-version and source-map persistence across server restart', flush=True)


if __name__ == '__main__':
    main()
