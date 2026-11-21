"""Real native FULL OUTER SQL acceptance; requires the integrated FULL build."""
import argparse
from collections import Counter
from pathlib import Path

from .fixture import BASEDIR, BIN, SHELL, Fixture, query, require
from .sql_short_native_acceptance import execute


def expected(left, right, predicate, projection=lambda l, r: (l, r)):
    output = []
    matched_right = set()
    for li, l in enumerate(left):
        matched = False
        for ri, r in enumerate(right):
            if predicate(l, r):
                output.append(projection(l, r))
                matched_right.add(ri)
                matched = True
        if not matched:
            output.append(projection(l, None))
    for ri, r in enumerate(right):
        if ri not in matched_right:
            output.append(projection(None, r))
    return output


def bag(rows):
    return Counter('\t'.join('NULL' if value is None else str(value)
                             for value in row) for row in rows)


def exercise(instance):
    execute(instance, '''CREATE SCHEMA full_fixture;
      CREATE TABLE full_fixture.l(id INT PRIMARY KEY,k INT,payload LONGTEXT,doc JSON) ENGINE=InnoDB;
      CREATE TABLE full_fixture.r LIKE full_fixture.l;
      INSERT INTO full_fixture.l(id,k) VALUES(1,1),(2,1),(3,NULL),(4,9);
      INSERT INTO full_fixture.r(id,k) VALUES(10,1),(20,1),(30,NULL),(40,8);''')
    left = [(1, 1), (2, 1), (3, None), (4, 9)]
    right = [(10, 1), (20, 1), (30, None), (40, 8)]

    def projected(l, r):
        return (*(l or (None, None)), *(r or (None, None)))

    cases = [
        ('l.k=r.k', lambda l, r: l[1] is not None and r[1] is not None and l[1] == r[1]),
        ('l.k<=>r.k', lambda l, r: l[1] == r[1]),
        ('l.k<r.k', lambda l, r: l[1] is not None and r[1] is not None and l[1] < r[1]),
        ('TRUE', lambda l, r: True),
        ('FALSE', lambda l, r: False),
        ('NULL', lambda l, r: False),
    ]
    for sql, predicate in cases:
        actual = query(instance, 'SELECT l.id,l.k,r.id,r.k FROM full_fixture.l l '
                       'FULL OUTER JOIN full_fixture.r r ON ' + sql)
        require(Counter(actual.splitlines()) == bag(expected(left, right, predicate, projected)),
                'FULL bag/NULL/duplicate semantics differ for ON ' + sql + ': ' + actual)

    # Every 2x2 bipartite relation, including those where equal-valued physical
    # right rows have different match histories. A key/NULL-sentinel anti-join
    # approximation does not implement these semantics.
    for mask in range(16):
        on = ' OR '.join('(l.id=%d AND r.id=%d)' % (left[i][0], right[j][0])
                         for i in range(2) for j in range(2)
                         if mask & (1 << (i * 2 + j))) or 'FALSE'
        full = ('SELECT l.id,l.k,r.id,r.k FROM full_fixture.l l '
                'FULL OUTER JOIN full_fixture.r r ON (' + on + ')')
        exp = expected(left, right,
                       lambda l, r: l[0] in [1, 2] and r[0] in [10, 20] and
                       bool(mask & (1 << ((l[0] - 1) * 2 + (r[0] // 10 - 1)))), projected)
        require(Counter(query(instance, full).splitlines()) == bag(exp),
                'FULL physical match flags differ for graph ' + str(mask))

    base = ('SELECT l.id AS left_id,l.k AS left_k,r.id AS right_id,r.k AS right_k '
            'FROM full_fixture.l l FULL /* lexer lookahead */ OUTER JOIN full_fixture.r r ON l.k=r.k')
    filtered = query(instance, base + ' WHERE l.id IS NULL OR r.id IS NULL ORDER BY left_id,right_id')
    require(filtered == 'NULL\tNULL\t30\tNULL\nNULL\tNULL\t40\t8\n3\tNULL\tNULL\tNULL\n4\t9\tNULL\tNULL',
            'WHERE was incorrectly applied before FULL null-extension: ' + filtered)
    require(query(instance, base + ' ORDER BY left_id,right_id LIMIT 2 OFFSET 2') ==
            '1\t1\t10\t1\n1\t1\t20\t1', 'FULL ORDER/OFFSET/LIMIT placement differs')
    # Full inputs and the ON expression execute once per source/candidate. This
    # must not be implemented by repeating an ON expression in a UNION branch.
    calls = execute(instance, 'SET @full_calls=0;' + base.replace('ON l.k=r.k',
                    'ON ((@full_calls:=@full_calls+1)>0 AND l.k=r.k)') + ';SELECT @full_calls;')
    require(calls.splitlines()[-1] == '16', 'FULL repeated or skipped a candidate ON evaluation: ' + calls)
    # Check a repeated prepared execution with parameters and native reprepare.
    prepared = base.replace('l.k=r.k', 'l.k=r.k AND l.id>=?') + ' ORDER BY left_id,right_id'
    result = execute(instance, "PREPARE full_p FROM '%s';SET @minimum=2;EXECUTE full_p USING @minimum;"
                     "SET @minimum=3;EXECUTE full_p USING @minimum;DEALLOCATE PREPARE full_p" % prepared)
    one = expected(left, right, lambda l, r: l[0] >= 2 and l[1] is not None and l[1] == r[1], projected)
    two = expected(left, right, lambda l, r: l[0] >= 3 and l[1] is not None and l[1] == r[1], projected)
    require(Counter(result.splitlines()) == bag(one + two), 'FULL prepared state was not reset')
    execute(instance, 'CREATE VIEW full_fixture.v AS ' + base)
    require(Counter(query(instance, 'SELECT * FROM full_fixture.v').splitlines()) ==
            bag(expected(left, right, cases[0][1], projected)), 'FULL view printer/reparse changed semantics')
    require(Counter(query(instance, 'SELECT * FROM full_fixture.v').splitlines()) ==
            bag(expected(left, right, cases[0][1], projected)), 'FULL view reopen/reset changed semantics')
    view_prepared=execute(instance, """PREPARE full_view FROM
      'SELECT * FROM full_fixture.v WHERE left_id>=? ORDER BY left_id,right_id';
      SET @minimum=1;EXECUTE full_view USING @minimum;
      SET @minimum=999;EXECUTE full_view USING @minimum;
      SET @minimum=2;EXECUTE full_view USING @minimum;
      DEALLOCATE PREPARE full_view;""")
    all_rows=expected(left,right,cases[0][1],projected)
    view_expected=[row for row in all_rows if row[0] is not None and row[0]>=1]
    view_expected += [row for row in all_rows if row[0] is not None and row[0]>=2]
    require(Counter(view_prepared.splitlines())==bag(view_expected),
            'FULL materialized view repeated PS/empty intermediate/reset/close changed results')
    require(query(instance, "SELECT GROUP_CONCAT(is_nullable ORDER BY ordinal_position) FROM information_schema.columns "
                  "WHERE table_schema='full_fixture' AND table_name='v'") == 'YES,YES,YES,YES',
            'FULL nullable complement metadata is wrong for NOT NULL base keys')
    require('full outer join' in query(instance, 'SHOW CREATE VIEW full_fixture.v').lower(),
            'canonical FULL view SQL lost its operator')
    execute(instance, 'DELIMITER //\nCREATE PROCEDURE full_fixture.full_program()\nBEGIN\n' +
            base + ';\nEND//\nDELIMITER ;')
    routine = execute(instance, 'CALL full_fixture.full_program();CALL full_fixture.full_program();')
    require(Counter(routine.splitlines()) == bag(expected(left, right, cases[0][1], projected) * 2),
            'FULL stored routine state or typed source binding changed between calls')
    inspected = execute(instance, 'SHOW EXPANDED PROCEDURE full_fixture.full_program')
    require('version=1 persisted=1 ' in inspected,
            'FULL-only classic routine did not persist its native language version')

    # Native packed rows must preserve long BLOB-backed Unicode and JSON values.
    execute(instance, "UPDATE full_fixture.l SET payload=REPEAT(CONVERT(0xe4b896 USING utf8mb4),400000),"
            "doc=JSON_OBJECT('n',CAST('18446744073709551615' AS UNSIGNED),'s','yerel') WHERE id=1;"
            "UPDATE full_fixture.r SET payload=CONCAT('a',CHAR(0),'b'),doc=JSON_OBJECT('ok',TRUE) WHERE id=10;")
    blobs = query(instance, 'SELECT l.id,OCTET_LENGTH(l.payload),SHA2(l.payload,256),'
                  'JSON_EXTRACT(l.doc,\'$.n\'),r.id,OCTET_LENGTH(r.payload),HEX(r.payload) '
                  'FROM full_fixture.l l FULL OUTER JOIN full_fixture.r r ON l.k=r.k '
                  'WHERE l.id=1 AND r.id=10')
    reference = query(instance, 'SELECT l.id,OCTET_LENGTH(l.payload),SHA2(l.payload,256),'
                      'JSON_EXTRACT(l.doc,\'$.n\'),r.id,OCTET_LENGTH(r.payload),HEX(r.payload) '
                      'FROM full_fixture.l l JOIN full_fixture.r r ON l.k=r.k WHERE l.id=1 AND r.id=10')
    require(blobs == reference and '\t1200000\t' in blobs and blobs.endswith('\t3\t610062'),
            'native FULL packed Unicode/JSON/binary payload was corrupted')
    plan = query(instance, 'EXPLAIN FORMAT=TREE ' + base + ' WHERE l.id IS NULL ORDER BY right_id LIMIT 2')
    require('Disk-backed full outer join' in plan and 'Filter' in plan and 'Sort' in plan,
            'FULL operator or post-join plan stages are missing: ' + plan)
    execute(instance, 'EXPLAIN FORMAT=TRADITIONAL ' + base, 1235)
    for suffix in ['FOR UPDATE', 'FOR SHARE']:
        execute(instance, base + ' ' + suffix, 1235)
    execute(instance, 'SELECT l.id FROM full_fixture.l l FULL OUTER JOIN (SELECT * FROM full_fixture.r) r ON l.k=r.k', 1235)
    execute(instance, base + ' JOIN full_fixture.r x ON x.id=l.id', 1235)
    execute(instance, 'SELECT DISTINCT l.k FROM full_fixture.l l FULL OUTER JOIN full_fixture.r r ON l.k=r.k', 1235)

    # FULL is still an ordinary nonreserved identifier. The old FULL alias plus
    # ordinary JOIN spelling retains its INNER join meaning.
    alias = query(instance, 'SELECT full.id,r.id FROM full_fixture.l FULL JOIN full_fixture.r r ON full.k=r.k ORDER BY full.id,r.id')
    require(alias == '1\t10\n1\t20\n2\t10\n2\t20', 'legacy FULL alias/identifier semantics changed')
    execute(instance, 'CREATE USER full_reader@localhost IDENTIFIED BY \'\' REQUIRE SSL;'
            'GRANT SELECT(id) ON full_fixture.l TO full_reader@localhost;'
            'GRANT SELECT(id) ON full_fixture.r TO full_reader@localhost;')
    env, command, shell = instance
    reader = (env, ['--user=full_reader' if arg == '--user=root' else arg for arg in command], shell)
    execute(reader, 'SELECT l.id,r.id FROM full_fixture.l l FULL OUTER JOIN full_fixture.r r ON l.k=r.k', 'denied')
    execute(instance, 'DELETE FROM full_fixture.l;')
    require(query(instance, 'SELECT l.id,r.id FROM full_fixture.l l FULL OUTER JOIN full_fixture.r r ON l.k=r.k ORDER BY r.id') ==
            'NULL\t10\nNULL\t20\nNULL\t30\nNULL\t40', 'empty left FULL lost right rows')
    execute(instance, 'DELETE FROM full_fixture.r;')
    require(query(instance, base) == '', 'empty/empty FULL returned a phantom row')
    execute(instance, 'INSERT INTO full_fixture.l(id,k) VALUES(100,NULL),(101,1)')
    require(query(instance, 'SELECT l.id,r.id FROM full_fixture.l l FULL OUTER JOIN full_fixture.r r ON TRUE ORDER BY l.id') ==
            '100\tNULL\n101\tNULL', 'empty right FULL lost left rows')
    print('PASS native FULL physical bags/NULL/all 2x2 graphs/volatile ON/post-filter/sort/limit/PS/view/metadata/BLOB/JSON/ACL/empty inputs', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld', type=Path, default=BIN/'mysqld.exe')
    parser.add_argument('--mysql', type=Path, default=BIN/'mysql.exe')
    parser.add_argument('--basedir', type=Path, default=BASEDIR)
    args = parser.parse_args()
    fixture = Fixture(args.mysqld.resolve(strict=True), args.mysql.resolve(strict=True),
                      args.basedir.resolve(strict=True), SHELL, None)
    with fixture.scratch('full-outer-native') as root:
        with fixture.disposable(root/'server') as instance:
            exercise(instance)


if __name__ == '__main__':
    main()
