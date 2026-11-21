"""Real InnoDB acceptance for QUALIFY, INSERT BY NAME and relational pipelines."""
import argparse
from pathlib import Path

from .fixture import BASEDIR, BIN, SHELL, Fixture, query, require
from .sql_short_native_acceptance import execute


def prepare_schema(instance):
    execute(instance, """CREATE SCHEMA relational_fixture;
      CREATE TABLE relational_fixture.sales(
        id INT PRIMARY KEY,branch_id INT,qty INT,unit_price DECIMAL(9,2),discount DECIMAL(3,2),rn INT DEFAULT 999) ENGINE=InnoDB;
      INSERT INTO relational_fixture.sales VALUES
        (1,1,2,10.00,0.10,999),(2,1,3,12.00,0.00,999),
        (3,2,1,20.00,0.25,999),(4,2,4,15.00,0.10,999);
      CREATE TABLE relational_fixture.branches(branch_id INT PRIMARY KEY,label VARCHAR(20)) ENGINE=InnoDB;
      INSERT INTO relational_fixture.branches VALUES(1,'bir'),(2,'iki');
    """)


def exercise_qualify(instance):
    pairs = [
        ('SELECT id,branch_id FROM relational_fixture.sales QUALIFY ROW_NUMBER() OVER(PARTITION BY branch_id ORDER BY id DESC)=1 ORDER BY id',
         'SELECT id,branch_id FROM (SELECT id,branch_id,ROW_NUMBER() OVER(PARTITION BY branch_id ORDER BY id DESC) AS n FROM relational_fixture.sales) x WHERE n=1 ORDER BY id'),
        ('SELECT id,ROW_NUMBER() OVER(ORDER BY id) AS ordinal FROM relational_fixture.sales QUALIFY ordinal>=3 ORDER BY id DESC LIMIT 1',
         'SELECT id,ordinal FROM (SELECT id,ROW_NUMBER() OVER(ORDER BY id) AS ordinal FROM relational_fixture.sales) x WHERE ordinal>=3 ORDER BY id DESC LIMIT 1'),
        ('SELECT DISTINCT branch_id FROM relational_fixture.sales QUALIFY ROW_NUMBER() OVER(PARTITION BY branch_id ORDER BY id)=2 ORDER BY branch_id DESC LIMIT 1',
         'SELECT DISTINCT branch_id FROM (SELECT branch_id,ROW_NUMBER() OVER(PARTITION BY branch_id ORDER BY id) AS n FROM relational_fixture.sales) x WHERE n=2 ORDER BY branch_id DESC LIMIT 1'),
        ('SELECT id FROM relational_fixture.sales QUALIFY SUM(qty) OVER(ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW)>4 AND ROW_NUMBER() OVER(ORDER BY id)>1 ORDER BY id',
         'SELECT id FROM (SELECT id,SUM(qty) OVER(ORDER BY id ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS s,ROW_NUMBER() OVER(ORDER BY id) AS n FROM relational_fixture.sales) x WHERE s>4 AND n>1 ORDER BY id'),
        ('SELECT branch_id,SUM(qty) AS total FROM relational_fixture.sales GROUP BY branch_id QUALIFY ROW_NUMBER() OVER(ORDER BY SUM(qty) DESC,branch_id)=1',
         'SELECT branch_id,total FROM (SELECT branch_id,SUM(qty) AS total,ROW_NUMBER() OVER(ORDER BY SUM(qty) DESC,branch_id) AS n FROM relational_fixture.sales GROUP BY branch_id) x WHERE n=1'),
    ]
    for short, long in pairs:
        require(query(instance, short) == query(instance, long), 'QUALIFY execution order differs: '+short)
    require(query(instance, 'SELECT id,ROW_NUMBER() OVER(ORDER BY id) AS rn FROM relational_fixture.sales QUALIFY rn=1') == '',
            'source column must take precedence over a same-named SELECT alias')
    require(query(instance, 'SELECT id FROM relational_fixture.sales QUALIFY NULL AND ROW_NUMBER() OVER(ORDER BY id)>0') == '',
            'NULL QUALIFY condition passed a row')
    execute(instance, 'SELECT id FROM relational_fixture.sales QUALIFY id=1', 'window')
    expected=query(instance, pairs[1][1])
    repeated=execute(instance, "PREPARE q FROM '%s';EXECUTE q;EXECUTE q;DEALLOCATE PREPARE q" % pairs[1][0])
    require(repeated == expected+'\n'+expected, 'prepared window slices were not reset')
    plan=execute(instance, 'EXPLAIN FORMAT=TREE '+pairs[2][0])
    print('QUALIFY plan (DISTINCT / post-window filter / LIMIT):\n'+plan,flush=True)

    print('PASS native QUALIFY execution order/plans/alias binding/prepared statements',flush=True)


def exercise_named_insert(instance):
    execute(instance, """CREATE TABLE relational_fixture.target(
      id INT PRIMARY KEY,qty INT NOT NULL DEFAULT 7,label VARCHAR(20) DEFAULT 'default',
      doubled INT GENERATED ALWAYS AS (qty*2) STORED) ENGINE=InnoDB;
      INSERT INTO relational_fixture.target BY NAME SELECT 12 AS qty,1 AS id;
      INSERT INTO relational_fixture.target BY NAME SELECT 2 AS id;
      INSERT INTO relational_fixture.target BY NAME SELECT 3 AS id,8 AS qty UNION ALL SELECT 4,9;
    """)
    require(query(instance, 'SELECT id,qty,label,doubled FROM relational_fixture.target ORDER BY id') ==
            '1\t12\tdefault\t24\n2\t7\tdefault\t14\n3\t8\tdefault\t16\n4\t9\tdefault\t18',
            'BY NAME positional mapping/defaults/generated/UNION values differ')
    execute(instance, 'INSERT INTO relational_fixture.target BY NAME SELECT 5 AS id,1 AS missing',1054)
    execute(instance, 'INSERT INTO relational_fixture.target BY NAME SELECT 5 AS id,6 AS id',1060)
    execute(instance, 'INSERT INTO relational_fixture.target BY NAME SELECT 5 AS id,90 AS doubled',3105)
    execute(instance, "INSERT INTO relational_fixture.target BY NAME SELECT 5 AS id,'bad' AS qty",1366)
    execute(instance, "PREPARE ins FROM 'INSERT INTO relational_fixture.target BY NAME SELECT ? AS qty,? AS id';SET @qty=10,@id=5;EXECUTE ins USING @qty,@id;DEALLOCATE PREPARE ins")
    require(query(instance, 'SELECT qty,doubled FROM relational_fixture.target WHERE id=5') == '10\t20', 'BY NAME prepared parameters differ')
    execute(instance, '''CREATE TABLE relational_fixture.function_defaults(
      id INT PRIMARY KEY,created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
      generated_token VARCHAR(36) NOT NULL DEFAULT (UUID())) ENGINE=InnoDB;
      INSERT INTO relational_fixture.function_defaults BY NAME SELECT 1 AS id;
      INSERT INTO relational_fixture.function_defaults BY NAME SELECT 2 AS id;''')
    require(query(instance, '''SELECT COUNT(*),COUNT(created_at),COUNT(DISTINCT generated_token),
      SUM(CHAR_LENGTH(generated_token)=36) FROM relational_fixture.function_defaults''') == '2\t2\t2\t2',
            'BY NAME lost functional defaults while deferring its target column list')
    execute(instance, 'START TRANSACTION;INSERT INTO relational_fixture.target BY NAME SELECT 20 AS id;ROLLBACK;')
    require(query(instance, 'SELECT COUNT(*) FROM relational_fixture.target WHERE id=20') == '0', 'BY NAME escaped rollback')
    execute(instance, """CREATE USER name_writer@localhost IDENTIFIED BY '' REQUIRE SSL;
      GRANT INSERT(id,qty) ON relational_fixture.target TO name_writer@localhost;
    """)
    env, command, cli=instance
    writer=(env,['--user=name_writer' if arg=='--user=root' else arg for arg in command],cli)
    execute(writer, 'INSERT INTO relational_fixture.target BY NAME SELECT 6 AS id,13 AS qty')
    execute(writer, "INSERT INTO relational_fixture.target BY NAME SELECT 7 AS id,'denied' AS label",'denied')

    print('PASS native BY NAME defaults/generated/parameters/ACL/rollback',flush=True)


def exercise_pipelines(instance):
    pipeline='''FROM relational_fixture.sales
      |> WHERE id>=1
      |> EXTEND qty*unit_price AS gross
      |> EXTEND gross*(1-discount) AS net
      |> AGGREGATE SUM(net) AS total GROUP BY branch_id
      |> WHERE total>=50
      |> ORDER BY total DESC,branch_id'''
    long='''SELECT branch_id,SUM(qty*unit_price*(1-discount)) AS total
      FROM relational_fixture.sales WHERE id>=1 GROUP BY branch_id
      HAVING total>=50 ORDER BY total DESC,branch_id'''
    require(query(instance,pipeline)==query(instance,long), 'pipeline EXTEND/AGGREGATE/filter/order differs')
    require(query(instance,'FROM relational_fixture.sales |> SELECT DISTINCT branch_id |> ORDER BY branch_id')==
            query(instance,'SELECT DISTINCT branch_id FROM relational_fixture.sales ORDER BY branch_id'), 'pipeline DISTINCT differs')
    require(query(instance,'FROM relational_fixture.sales |> JOIN relational_fixture.branches USING(branch_id) |> SELECT id,label |> ORDER BY id')==
            query(instance,'SELECT id,label FROM relational_fixture.sales JOIN relational_fixture.branches USING(branch_id) ORDER BY id'), 'pipeline explicit JOIN USING differs')
    require(query(instance,'FROM relational_fixture.sales |> ORDER BY id DESC |> LIMIT 2')==
            query(instance,'SELECT * FROM relational_fixture.sales ORDER BY id DESC LIMIT 2'), 'pipeline ORDER then LIMIT differs')
    require(query(instance,'FROM relational_fixture.sales |> LIMIT 2 |> ORDER BY id DESC')==
            query(instance,'SELECT * FROM (SELECT * FROM relational_fixture.sales LIMIT 2) x ORDER BY id DESC'), 'pipeline LIMIT boundary moved through ORDER')
    execute(instance,'FROM relational_fixture.sales |> EXTEND qty+1 AS qty',1060)
    execute(instance,'FROM relational_fixture.sales |> EXTEND qty+1 AS new_qty,new_qty+1 AS second_qty',1054)
    execute(instance,'FROM relational_fixture.sales |> EXTEND qty+1 AS new_qty,(SELECT new_qty) AS second_qty',1054)
    execute(instance,'FROM relational_fixture.sales |> AGGREGATE SUM(qty) AS total GROUP BY branch_id |> WHERE qty>0',1054)
    execute(instance,'FROM relational_fixture.sales |> SELECT SUM(qty)',63005)
    counter=execute(instance,'SET @counter=0;FROM relational_fixture.sales |> EXTEND (@counter:=@counter+1) AS serial |> SELECT serial,serial AS repeated |> ORDER BY serial;SELECT @counter;')
    require(counter=='1\t1\n2\t2\n3\t3\n4\t4\n4','pipeline duplicated a volatile EXTEND value')
    execute(instance,'CREATE VIEW relational_fixture.pipeline_view AS '+pipeline)
    require(query(instance,'SELECT * FROM relational_fixture.pipeline_view')==query(instance,long), 'pipeline view definition differs')
    execute(instance, '''CREATE VIEW relational_fixture.volatile_pipeline AS
      FROM relational_fixture.sales |> EXTEND RAND() AS sampled
      |> SELECT id,sampled,sampled AS same_sample''')
    # Views are reparsed from the server's canonical SQL. Repeated references to
    # a stage output must still read one stored value after that reparse.
    require(query(instance, '''SELECT COUNT(*),SUM(sampled=same_sample)
      FROM relational_fixture.volatile_pipeline''') == '4\t4',
            'pipeline materialization boundary did not survive canonical view reparse')
    prepared=pipeline.replace('id>=1','id>=?').replace('total>=50','total>=?')
    require(execute(instance,"PREPARE pipe FROM '%s';SET @lo=1,@total=50;EXECUTE pipe USING @lo,@total;DEALLOCATE PREPARE pipe" % prepared)==query(instance,long), 'pipeline placeholder order/type binding differs')
    require(query(instance,"SELECT '|>',1|2")=='|>\t3','pipeline lexer changed string or bitwise-OR tokens')
    print('PASS native pipeline scopes/boundaries/volatility/view/parameters',flush=True)


def exercise(instance):
    prepare_schema(instance)
    exercise_qualify(instance)
    exercise_named_insert(instance)
    exercise_pipelines(instance)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('sql-short-relational') as root:
        with fixture.disposable(root/'server') as instance:
            exercise(instance)


if __name__=='__main__':
    main()
