"""Native BY NAME edge acceptance: view translations and explicit ODKU."""
import argparse
from pathlib import Path

from .fixture import BASEDIR, BIN, SHELL, Fixture, query, require
from .sql_short_native_acceptance import execute


def exercise(instance):
    execute(instance, '''CREATE SCHEMA named_edges;
      CREATE TABLE named_edges.target(
        id INT PRIMARY KEY,qty INT NOT NULL DEFAULT 7,label VARCHAR(20) NOT NULL DEFAULT 'plain',
        doubled INT GENERATED ALWAYS AS (qty*2) STORED) ENGINE=InnoDB;
      CREATE VIEW named_edges.v AS
        SELECT id AS code,qty AS amount,label AS caption FROM named_edges.target WHERE qty>=0 WITH CHECK OPTION;
      CREATE VIEW named_edges.nested_v AS SELECT code AS book_id,amount AS quantity FROM named_edges.v;
      INSERT INTO named_edges.v BY NAME SELECT 20 AS amount,1 AS code;
      INSERT INTO named_edges.v BY NAME SELECT 2 AS code;
      INSERT INTO named_edges.nested_v BY NAME SELECT 9 AS quantity,3 AS book_id;
    ''')
    require(query(instance, 'SELECT id,qty,label,doubled FROM named_edges.target ORDER BY id') ==
            '1\t20\tplain\t40\n2\t7\tplain\t14\n3\t9\tplain\t18',
            'BY NAME view aliases/defaults/generated fields/nested translation differ')
    execute(instance, 'INSERT INTO named_edges.v BY NAME SELECT 4 AS code,-1 AS amount', 1369)
    execute(instance, 'INSERT INTO named_edges.v BY NAME SELECT 4 AS code,2 AS qty', 1054)
    execute(instance, 'INSERT INTO named_edges.target BY NAME SELECT 1 AS id,5 AS qty', 1062)
    execute(instance, '''INSERT INTO named_edges.target BY NAME SELECT 5 AS qty,1 AS id
      ON DUPLICATE KEY UPDATE qty=qty+VALUES(qty),label='changed';
      INSERT INTO named_edges.v BY NAME SELECT 4 AS code,8 AS amount
      ON DUPLICATE KEY UPDATE amount=VALUES(amount);
      INSERT INTO named_edges.v BY NAME SELECT 4 AS code,11 AS amount
      ON DUPLICATE KEY UPDATE amount=VALUES(amount);
    ''')
    require(query(instance, 'SELECT id,qty,label,doubled FROM named_edges.target WHERE id IN(1,4) ORDER BY id') ==
            '1\t25\tchanged\t50\n4\t11\tplain\t22', 'explicit ODKU insert/update or view target translation differs')

    execute(instance, '''PREPARE named_update FROM
      'INSERT INTO named_edges.target BY NAME SELECT ? AS qty,? AS id ON DUPLICATE KEY UPDATE qty=VALUES(qty)';
      SET @amount=12,@code=1;EXECUTE named_update USING @amount,@code;
      SET @amount=13,@code=5;EXECUTE named_update USING @amount,@code;
      DEALLOCATE PREPARE named_update;
      INSERT INTO named_edges.target BY NAME SELECT 1 AS id,6 AS qty UNION ALL SELECT 1,8
      ON DUPLICATE KEY UPDATE qty=VALUES(qty);
    ''')
    require(query(instance, 'SELECT id,qty,doubled FROM named_edges.target WHERE id IN(1,5) ORDER BY id') ==
            '1\t8\t16\n5\t13\t26', 'ODKU prepared target typing or UNION binding differs')

    execute(instance, '''CREATE TABLE named_edges.trigger_audit(kind CHAR(1),id INT,amount INT) ENGINE=InnoDB;
      CREATE TRIGGER named_edges.record_insert AFTER INSERT ON named_edges.target FOR EACH ROW
        INSERT INTO named_edges.trigger_audit VALUES('I',NEW.id,NEW.qty);
      CREATE TRIGGER named_edges.record_update AFTER UPDATE ON named_edges.target FOR EACH ROW
        INSERT INTO named_edges.trigger_audit VALUES('U',NEW.id,NEW.qty);
      INSERT INTO named_edges.v BY NAME SELECT 6 AS code,15 AS amount
        ON DUPLICATE KEY UPDATE amount=VALUES(amount);
      INSERT INTO named_edges.v BY NAME SELECT 6 AS code,16 AS amount
        ON DUPLICATE KEY UPDATE amount=VALUES(amount);
    ''')
    require(query(instance, 'SELECT kind,id,amount FROM named_edges.trigger_audit ORDER BY amount') ==
            'I\t6\t15\nU\t6\t16', 'native view/ODKU triggers were bypassed or duplicated')
    before = query(instance, 'SELECT id,qty,label,doubled FROM named_edges.target ORDER BY id')
    execute(instance, '''START TRANSACTION;
      INSERT INTO named_edges.v BY NAME SELECT 6 AS code,99 AS amount ON DUPLICATE KEY UPDATE amount=VALUES(amount);
      INSERT INTO named_edges.v BY NAME SELECT 7 AS code,21 AS amount;
      ROLLBACK;''')
    require(query(instance, 'SELECT id,qty,label,doubled FROM named_edges.target ORDER BY id') == before and
            query(instance, 'SELECT COUNT(*) FROM named_edges.trigger_audit') == '2',
            'BY NAME view/ODKU or native trigger writes escaped rollback')

    execute(instance, '''CREATE USER name_view_writer@localhost IDENTIFIED BY '' REQUIRE SSL;
      GRANT INSERT(code,amount),UPDATE(amount) ON named_edges.v TO name_view_writer@localhost;
    ''')
    env, command, shell = instance
    writer = (env, ['--user=name_view_writer' if argument == '--user=root' else argument for argument in command], shell)
    execute(writer, 'INSERT INTO named_edges.v BY NAME SELECT 8 AS code,17 AS amount;')
    # VALUES(amount) has the same native column-read privilege requirement in
    # ordinary INSERT and BY NAME. Verify that boundary before granting it.
    execute(writer, '''INSERT INTO named_edges.v(code,amount) VALUES(8,18)
      ON DUPLICATE KEY UPDATE amount=VALUES(amount);''', 1143)
    execute(writer, '''INSERT INTO named_edges.v BY NAME SELECT 8 AS code,18 AS amount
      ON DUPLICATE KEY UPDATE amount=VALUES(amount);''', 1143)
    execute(instance, 'GRANT SELECT(amount) ON named_edges.v TO name_view_writer@localhost;')
    execute(writer, '''INSERT INTO named_edges.v BY NAME SELECT 8 AS code,18 AS amount
      ON DUPLICATE KEY UPDATE amount=VALUES(amount);''')
    execute(writer, "INSERT INTO named_edges.v BY NAME SELECT 9 AS code,'denied' AS caption", 'denied')
    execute(writer, "INSERT INTO named_edges.v BY NAME SELECT 8 AS code,19 AS amount ON DUPLICATE KEY UPDATE caption='denied'", 'denied')
    require(query(instance, 'SELECT qty,label FROM named_edges.target WHERE id=8') == '18\tplain',
            'view column permissions were not enforced on named INSERT and explicit UPDATE')
    print('PASS native BY NAME aliased/nested views, CHECK OPTION, explicit ODKU, PS/UNION typing, triggers, rollback and column ACL', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld', type=Path, default=BIN/'mysqld.exe')
    parser.add_argument('--mysql', type=Path, default=BIN/'mysql.exe')
    parser.add_argument('--basedir', type=Path, default=BASEDIR)
    args = parser.parse_args()
    fixture = Fixture(args.mysqld.resolve(strict=True), args.mysql.resolve(strict=True), args.basedir.resolve(strict=True), SHELL, None)
    with fixture.scratch('sql-short-named-edges') as root:
        with fixture.disposable(root/'server') as instance:
            exercise(instance)


if __name__ == '__main__':
    main()
