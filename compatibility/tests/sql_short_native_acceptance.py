"""Exercise native shorthand on a disposable InnoDB server, without rewriting SQL."""
import argparse
from pathlib import Path

from .fixture import BASEDIR, BIN, SHELL, Fixture, query, require, run


def execute(instance, source, error=None):
    env, command, _ = instance
    result = run(command, env, 60, script=source)
    if error is None:
        require(result.returncode == 0, result.stdout + result.stderr)
    else:
        marker = f'ERROR {error} (' if isinstance(error, int) else str(error)
        require(result.returncode != 0 and marker in result.stderr,
                result.stdout + result.stderr)
        if isinstance(error, int) and 63001 <= error <= 63005:
            public = 630000 + error - 63000
            require(f'{public}: ' in result.stderr,
                    'SQL-short public error prefix missing: '+result.stdout+result.stderr)
    return result.stdout.strip()


def exercise(instance):
    execute(instance, """CREATE SCHEMA short_fixture;
      CREATE TABLE short_fixture.source(
        id INT PRIMARY KEY,a INT,b INT,label VARCHAR(30),secret TEXT,
        invisible_value INT INVISIBLE DEFAULT 91) ENGINE=InnoDB;
      INSERT INTO short_fixture.source(id,a,b,label,secret) VALUES
        (1,10,20,'İSTANBUL','private'),(2,NULL,5,'Ankara','private');
    """)
    short = "SELECT * EXCLUDE(secret) REPLACE(b AS a,a AS b,LOWER(label) AS label) FROM short_fixture.source ORDER BY id"
    long = "SELECT id,b AS a,a AS b,LOWER(label) AS label FROM short_fixture.source ORDER BY id"
    require(query(instance, short) == query(instance, long),
            'REPLACE siblings must bind the original inputs and preserve order')
    require(query(instance, 'SELECT s.* EXCLUDE(secret) FROM short_fixture.source AS s ORDER BY id') ==
            query(instance, 'SELECT id,a,b,label FROM short_fixture.source ORDER BY id'),
            'qualified wildcard changed visibility or column order')
    require(query(instance, 'SELECT l.* EXCLUDE(secret),r.id AS right_id FROM short_fixture.source AS l JOIN short_fixture.source AS r USING(id) ORDER BY l.id') ==
            query(instance, 'SELECT l.id,l.a,l.b,l.label,r.id AS right_id FROM short_fixture.source AS l JOIN short_fixture.source AS r USING(id) ORDER BY l.id'),
            'qualified wildcard did not stay within its source')
    require(query(instance, 'WITH c AS (SELECT * EXCLUDE(secret) FROM short_fixture.source) SELECT * FROM c ORDER BY id') ==
            query(instance, 'SELECT id,a,b,label FROM short_fixture.source ORDER BY id'), 'CTE output differs')
    require(query(instance, 'SELECT * EXCLUDE(secret) FROM short_fixture.source UNION ALL SELECT * EXCLUDE(secret) FROM short_fixture.source ORDER BY id,a') ==
            query(instance, 'SELECT id,a,b,label FROM short_fixture.source UNION ALL SELECT id,a,b,label FROM short_fixture.source ORDER BY id,a'), 'set operation output differs')
    for source, error in (
        ('SELECT * EXCLUDE(missing) FROM short_fixture.source', 1054),
        ('SELECT * EXCLUDE(secret,secret) FROM short_fixture.source', 1060),
        ('SELECT * REPLACE(1 AS missing) FROM short_fixture.source', 1054),
        ('SELECT * REPLACE(1 AS a,2 AS a) FROM short_fixture.source', 1060),
        ('SELECT * EXCLUDE(a) REPLACE(1 AS a) FROM short_fixture.source', 1054),
        ('SELECT * EXCLUDE(id,a,b,label,secret) FROM short_fixture.source', 1210),
        ('SELECT * EXCLUDE(id) FROM short_fixture.source l JOIN short_fixture.source r ON l.id=r.id', 1052),
        ('SELECT EXISTS(SELECT * EXCLUDE(missing) FROM short_fixture.source)', 1054),
    ):
        execute(instance, source, error)

    execute(instance, 'CREATE VIEW short_fixture.visible AS SELECT * EXCLUDE(secret) REPLACE(CAST(a AS CHAR(20)) AS a) FROM short_fixture.source')
    require(query(instance, "SELECT data_type FROM information_schema.columns WHERE table_schema='short_fixture' AND table_name='visible' AND column_name='a'") == 'varchar',
            'replacement type did not reach result metadata')
    prepared = execute(instance, """PREPARE p FROM 'SELECT * EXCLUDE(secret) FROM short_fixture.source WHERE id=?';
      SET @id=1;EXECUTE p USING @id;EXECUTE p USING @id;DEALLOCATE PREPARE p;""")
    expected = query(instance, 'SELECT id,a,b,label FROM short_fixture.source WHERE id=1')
    require(prepared == expected+'\n'+expected, 'prepared re-execution changed projection')
    execute(instance, """ALTER TABLE short_fixture.source ADD added INT DEFAULT 7;
      CREATE TABLE short_fixture.sink AS SELECT * EXCLUDE(secret) FROM short_fixture.source;
    """)
    require(query(instance, "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema='short_fixture' AND table_name='visible' AND column_name='added'") == '0',
            'stored view silently added a new base-table column')
    require(query(instance, 'SELECT added FROM short_fixture.sink ORDER BY id') == '7\n7', 'fresh wildcard did not rebind added column')
    execute(instance, """CREATE USER short_reader@localhost IDENTIFIED BY '' REQUIRE SSL;
      GRANT SELECT(id,a,b,label,added) ON short_fixture.source TO short_reader@localhost;
    """)
    env, command, cli = instance
    reader = (env, ['--user=short_reader' if value == '--user=root' else value for value in command], cli)
    execute(reader, 'SELECT * EXCLUDE(secret) FROM short_fixture.source', 'denied')
    require(query(reader, 'SELECT id,a FROM short_fixture.source WHERE id=1') == '1\t10', 'column grant fixture differs')

    execute(instance, """CREATE FUNCTION short_fixture.add_one(n INT) RETURNS INT DETERMINISTIC NO SQL AS RETURN n+1;
      CREATE FUNCTION short_fixture.old_add_one(n INT) RETURNS INT DETERMINISTIC NO SQL RETURN n+1;
      DELIMITER //
      CREATE PROCEDURE short_fixture.when_one(IN n INT,OUT result INT)
      BEGIN
        SET result=0;
        WHEN n>0 DO SET result=result+1;
        WHEN n>1 DO BEGIN SET result=result+2;SET result=result+4;END;
        WHEN n>2 DO WHEN n>3 DO SET result=result+8;
      END//
      DELIMITER ;
    """)
    require(query(instance, 'SELECT short_fixture.add_one(10),short_fixture.add_one(NULL)') ==
            query(instance, 'SELECT short_fixture.old_add_one(10),short_fixture.old_add_one(NULL)'), 'AS RETURN changed NULL/type semantics')
    result=execute(instance, 'CALL short_fixture.when_one(0,@r);SELECT @r;CALL short_fixture.when_one(2,@r);SELECT @r;CALL short_fixture.when_one(4,@r);SELECT @r;CALL short_fixture.when_one(NULL,@r);SELECT @r;')
    require(result == '0\n7\n15\n0', 'WHEN single statement/block/nesting/NULL condition differs')
    execute(instance, """CREATE TABLE short_fixture.alerts(n BIGINT) ENGINE=InnoDB;
      DELIMITER //
      CREATE PROCEDURE short_fixture.stock_check(p_limit BIGINT)
      AS
        LET amount=(SELECT COUNT(*) FROM short_fixture.source WHERE a<:p_limit);
        WHEN amount>0 DO INSERT INTO short_fixture.alerts VALUES(:amount);
      END//
      CREATE PROCEDURE short_fixture.let_types()
      AS
        LET money=1.20;
        LET label='hazır';
        LET nullable BIGINT=NULL;
        LET total DECIMAL(18,2)=0.00;
        SET money=money+2.30;
        SET total=money;
        SELECT :money,:label,:nullable,:total;
      END//
      CREATE PROCEDURE short_fixture.scope_binding(a INT)
      AS
        LET result=1;
        BEGIN LET result=2;SET @inner=result;END;
        SELECT a,:a,:result FROM short_fixture.source WHERE id=1;
      END//
      CREATE PROCEDURE short_fixture.scalar_zero()
      AS
        LET value=(SELECT a FROM short_fixture.source WHERE id=99);
        SELECT :value;
      END//
      CREATE PROCEDURE short_fixture.interleaved_scopes()
      AS
        LET first_local=1;
        BEGIN LET inner_value='abc';SET @scope_inner=inner_value;END;
        LET later_value=100;
        SET later_value=123456;
        SELECT :first_local,:later_value,@scope_inner;
      END//
      CREATE PROCEDURE short_fixture.scalar_many()
      AS
        LET value=(SELECT a FROM short_fixture.source);
        SELECT :value;
      END//
      CREATE PROCEDURE short_fixture.let_overflow()
      AS
        LET value DECIMAL(3,2)=1.20;
        SET value=100.00;
      END//
      DELIMITER ;
    """)
    execute(instance, 'CALL short_fixture.stock_check(11)')
    require(query(instance, 'SELECT n FROM short_fixture.alerts') == '1', 'short body/scalar query/explicit SQL local binding differs')
    require(execute(instance, 'CALL short_fixture.let_types()') == '3.50\thazır\tNULL\t3.50', 'LET inferred/explicit DECIMAL/string/NULL types differ')
    require(execute(instance, 'CALL short_fixture.scope_binding(99);SELECT @inner') == '10\t99\t1\n2', 'short local shadowing or column/parameter binding differs')
    require(execute(instance, 'CALL short_fixture.interleaved_scopes()') == '1\t123456\tabc', 'LET after a nested scope reused another local runtime slot/type')
    require(execute(instance, 'CALL short_fixture.scalar_zero()') == 'NULL', 'scalar zero rows did not produce typed NULL')
    execute(instance, 'CALL short_fixture.scalar_many()', 1242)
    execute(instance, 'CALL short_fixture.let_overflow()', 1264)
    execute(instance, 'DELIMITER //\nCREATE PROCEDURE short_fixture.bad_null() AS LET x=NULL;END//\nDELIMITER ;', 63001)
    execute(instance, 'DELIMITER //\nCREATE PROCEDURE short_fixture.bad_duplicate() AS LET x=1;LET x=2;END//\nDELIMITER ;', 1331)
    expanded=execute(instance, 'SHOW EXPANDED SELECT * EXCLUDE(secret) REPLACE(a+1 AS a) FROM short_fixture.source')
    bound_sql='\n'.join(row for row in expanded.splitlines() if row.startswith('bound_sql\t'))
    require('bound_sql\t0\tselect' in bound_sql and 'EXCLUDE' not in bound_sql and 'output_metadata' in expanded,
            'SHOW EXPANDED did not print a bound native projection and metadata')
    expanded=execute(instance, 'SHOW EXPANDED PROCEDURE short_fixture.stock_check')
    require('compiler_ir' in expanded and 'jump_if_not' in expanded, 'routine expansion did not use compiled SP instructions')
    require(query(instance, 'SELECT COUNT(*) FROM short_fixture.alerts') == '1', 'routine expansion executed the procedure')
    expanded=execute(instance, "PREPARE e FROM 'SHOW EXPANDED SELECT * EXCLUDE(secret) FROM short_fixture.source WHERE id=?';SET @id=1;EXECUTE e USING @id;DEALLOCATE PREPARE e")
    require('?' in expanded, 'expanded prepared SQL revealed a bound parameter value')
    execute(instance, """DELIMITER //
      CREATE PROCEDURE short_fixture.for_rows()
      AS
        LET total BIGINT=0;
        FOR entry IN SELECT id,a FROM short_fixture.source ORDER BY id DO
          WHEN :entry.id=1 DO ITERATE entry;
          SET total=total+:entry.id;
          WHEN :entry.id=2 DO LEAVE entry;
        END FOR;
        SELECT :total;
      END//
      CREATE PROCEDURE short_fixture.for_zero()
      AS
        LET total BIGINT=0;
        FOR entry IN SELECT * FROM short_fixture.source WHERE id=99 DO
          SET total=total+1;
        END FOR;
        SELECT :total;
      END//
      CREATE PROCEDURE short_fixture.for_duplicate()
      AS
        FOR entry IN SELECT id,id FROM short_fixture.source WHERE id=99 DO
          SELECT :entry.id;
        END FOR;
      END//
      CREATE PROCEDURE short_fixture.for_missing()
      AS
        FOR entry IN SELECT id FROM short_fixture.source WHERE id=99 DO
          SELECT :entry.missing;
        END FOR;
      END//
      CREATE PROCEDURE short_fixture.for_open_handler()
      AS
        DECLARE errors INT DEFAULT 0;
        DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET errors=errors+1;
        FOR entry IN SELECT id,id FROM short_fixture.source WHERE id=99 DO
          SET @unexpected_body=1;
        END FOR;
        SELECT :errors;
      END//
      CREATE PROCEDURE short_fixture.for_write()
      AS
        FOR entry IN SELECT id FROM short_fixture.source DO
          SELECT id INTO entry FROM short_fixture.source LIMIT 1;
        END FOR;
      END//
      CREATE FUNCTION short_fixture.for_return() RETURNS INT READS SQL DATA
      AS
        FOR entry IN SELECT id FROM short_fixture.source ORDER BY id DO
          RETURN :entry.id;
        END FOR;
        RETURN 0;
      END//
      DELIMITER ;
    """)
    require(execute(instance, 'CALL short_fixture.for_rows();CALL short_fixture.for_rows()') == '2\n2', 'FOR iteration/LEAVE/ITERATE or cursor reuse differs')
    require(execute(instance, 'CALL short_fixture.for_zero()') == '0', 'zero-row FOR entered its body')
    execute(instance, 'CALL short_fixture.for_duplicate()', 63003)
    execute(instance, 'CALL short_fixture.for_missing()', 1054)
    require(execute(instance, 'CALL short_fixture.for_open_handler()') == '1', 'failed FOR OPEN did not continue through cursor cleanup exactly once')
    execute(instance, 'CALL short_fixture.for_write()', 63002)
    require(query(instance, 'SELECT short_fixture.for_return(),short_fixture.for_return()') == '1\t1', 'RETURN did not release its FOR cursor')
    execute(instance, 'CREATE PROCEDURE short_fixture.for_bad_write() AS FOR entry IN SELECT id FROM short_fixture.source DO SET entry=1;END FOR;END', 63002)
    require(query(instance, 'SELECT id AS do FROM short_fixture.source AS do ORDER BY id') == '1\n2', 'FOR delimiter changed legacy DO identifier/alias parsing')
    print('PASS native star projection/metadata/ACL/PS/DDL, short SP types/scopes/scalars/control flow, FOR row cursors/cleanup/errors and compiler expansion', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld', type=Path, default=BIN/'mysqld.exe')
    parser.add_argument('--mysql', type=Path, default=BIN/'mysql.exe')
    parser.add_argument('--basedir', type=Path, default=BASEDIR)
    args = parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('sql-short-native') as root:
        with fixture.disposable(root/'server') as instance:
            exercise(instance)


if __name__ == '__main__':
    main()
