"""Native ordered pipeline stage-boundary acceptance on real InnoDB data."""
import argparse
from pathlib import Path
import re

from .fixture import BASEDIR, BIN, SHELL, Fixture, query, require
from .sql_short_native_acceptance import execute
from .sql_short_relational_acceptance import prepare_schema


def exercise(instance):
    prepare_schema(instance)
    comparisons=[
        ('FROM relational_fixture.sales |> ORDER BY id DESC |> WHERE id<>3 |> SELECT id,qty |> LIMIT 2',
         'SELECT id,qty FROM relational_fixture.sales WHERE id<>3 ORDER BY id DESC LIMIT 2'),
        ('FROM relational_fixture.sales |> ORDER BY id DESC |> SELECT qty AS id,id AS original_id |> WHERE original_id<>3 |> LIMIT 2',
         'SELECT qty AS id,id AS original_id FROM relational_fixture.sales WHERE id<>3 ORDER BY original_id DESC LIMIT 2'),
        ('FROM relational_fixture.sales |> ORDER BY id DESC |> EXTEND qty+1 AS extra |> WHERE extra>2 |> SELECT id,extra |> LIMIT 2',
         'SELECT id,qty+1 AS extra FROM relational_fixture.sales WHERE qty+1>2 ORDER BY id DESC LIMIT 2'),
        ('FROM relational_fixture.sales |> ORDER BY id DESC |> LIMIT 3 |> WHERE id<>3 |> SELECT id |> LIMIT 2',
         'SELECT id FROM (SELECT * FROM relational_fixture.sales ORDER BY id DESC LIMIT 3) x WHERE id<>3 ORDER BY id DESC LIMIT 2'),
        ('FROM relational_fixture.sales |> ORDER BY id DESC |> LIMIT 3 |> ORDER BY qty,id |> SELECT id,qty |> LIMIT 2',
         'SELECT id,qty FROM (SELECT * FROM relational_fixture.sales ORDER BY id DESC LIMIT 3) x ORDER BY qty,id LIMIT 2'),
        ('FROM relational_fixture.sales |> SELECT qty,id |> ORDER BY 1 DESC,2 |> WHERE id<>3 |> SELECT id,qty |> LIMIT 2',
         'SELECT id,qty FROM relational_fixture.sales WHERE id<>3 ORDER BY qty DESC,id LIMIT 2'),
        ('FROM relational_fixture.sales |> ORDER BY id DESC |> SELECT DISTINCT branch_id |> ORDER BY branch_id',
         'SELECT DISTINCT branch_id FROM relational_fixture.sales ORDER BY branch_id'),
        ('FROM relational_fixture.sales |> ORDER BY id DESC |> AGGREGATE SUM(qty) AS total GROUP BY branch_id |> ORDER BY branch_id',
         'SELECT branch_id,SUM(qty) AS total FROM relational_fixture.sales GROUP BY branch_id ORDER BY branch_id'),
        ('FROM relational_fixture.sales |> ORDER BY id DESC |> JOIN relational_fixture.branches USING(branch_id) |> SELECT id,label |> ORDER BY id',
         'SELECT id,label FROM relational_fixture.sales JOIN relational_fixture.branches USING(branch_id) ORDER BY id'),
        ('FROM relational_fixture.sales |> ORDER BY id DESC |> SELECT * EXCLUDE(rn) |> LIMIT 2',
         'SELECT id,branch_id,qty,unit_price,discount FROM relational_fixture.sales ORDER BY id DESC LIMIT 2'),
        ('FROM relational_fixture.sales |> ORDER BY id DESC |> SELECT * REPLACE(qty+1 AS qty) |> LIMIT 2',
         'SELECT id,branch_id,qty+1 AS qty,unit_price,discount,rn FROM relational_fixture.sales ORDER BY id DESC LIMIT 2'),
    ]
    for short,long in comparisons:
        require(query(instance,short)==query(instance,long),'ordered pipeline result/schema differs: '+short)
    for position in ['0','7','999999999999']:
        execute(instance,'FROM relational_fixture.sales |> ORDER BY '+position+' |> WHERE id>0',1054)
    # Rank generation evaluates a volatile sort expression once per original
    # row. Later projection, filtering and view reparse must read that value.
    result=execute(instance,'''SET @calls=0;
      FROM relational_fixture.sales |> ORDER BY (@calls:=@calls+1) DESC
      |> WHERE id<>2 |> SELECT id |> LIMIT 2;
      SELECT @calls;''')
    require(result=='4\n3\n4','ORDER expression was repeated or the carrier order was lost: '+result)
    result=execute(instance,'''SET @first_calls=0,@second_calls=0;
      FROM relational_fixture.sales
      |> ORDER BY (@first_calls:=@first_calls+1) DESC,(@second_calls:=@second_calls+1)
      |> SELECT id |> LIMIT 2;
      SELECT @first_calls,@second_calls;''')
    require(result=='4\n3\n4\t4','multiple ORDER expressions were not materialized once: '+result)
    pipeline=('FROM relational_fixture.sales |> ORDER BY id DESC |> SELECT id,qty '
              '|> WHERE qty>? |> LIMIT 2')
    prepared=execute(instance,"PREPARE ordered FROM '%s';SET @minimum=1;EXECUTE ordered USING @minimum;"
                     "SET @minimum=2;EXECUTE ordered USING @minimum;DEALLOCATE PREPARE ordered"%pipeline)
    require(prepared=='4\t4\n2\t3\n4\t4\n2\t3','prepared ordered pipeline changed its output metadata/order')
    execute(instance,'''CREATE VIEW relational_fixture.ordered_view AS
      FROM relational_fixture.sales |> ORDER BY id DESC |> EXTEND qty+1 AS extra
      |> WHERE extra>2 |> SELECT id,extra |> LIMIT 2''')
    require(query(instance,'SELECT * FROM relational_fixture.ordered_view')=='4\t5\n2\t4',
            'ordered pipeline carrier did not survive canonical view SQL reparse')
    columns=query(instance,"SELECT column_name FROM information_schema.columns WHERE table_schema='relational_fixture' AND table_name='ordered_view' ORDER BY ordinal_position")
    require(columns=='id\nextra','compiler order carrier leaked into view result metadata: '+columns)
    # Discover an actual compiler symbol, then introduce a user field with that
    # name without changing the SQL source. The next native prepare must choose
    # another symbol and must retain the user's field and value.
    execute(instance,'CREATE TABLE relational_fixture.collision_source(id INT PRIMARY KEY) ENGINE=InnoDB;'
                     'INSERT INTO relational_fixture.collision_source VALUES(1),(2);')
    prefix='FROM relational_fixture.collision_source |> ORDER BY id'
    expanded=execute(instance,'SHOW EXPANDED '+prefix)
    symbol=re.search(r'__sdp_ord_[0-9a-f_]+',expanded)
    require(symbol is not None,'ordered pipeline did not expose its real native ordinal in diagnostic SQL')
    name=symbol.group(0)
    execute(instance,'ALTER TABLE relational_fixture.collision_source ADD COLUMN `'+name+'` INT DEFAULT 99;')
    require(query(instance,prefix+' |> WHERE id>0 |> SELECT *')=='1\t99\n2\t99',
            'compiler ordinal collided with and replaced a user field')
    # A right JOIN field can legitimately have the ordinal's name too. Only the
    # actual compiler field from input is hidden; the right field must survive.
    other_prefix='FROM relational_fixture.sales |> ORDER BY id'
    other_symbol=re.search(r'__sdp_ord_[0-9a-f_]+',execute(instance,'SHOW EXPANDED '+other_prefix))
    require(other_symbol is not None,'native diagnostic ordinal unavailable for JOIN identity check')
    other_name=other_symbol.group(0)
    execute(instance,'CREATE TABLE relational_fixture.collision_right(id INT PRIMARY KEY,`'+other_name+'` INT) ENGINE=InnoDB;'
                     'INSERT INTO relational_fixture.collision_right VALUES(1,91),(2,92);')
    joined=other_prefix+' |> JOIN relational_fixture.collision_right USING(id) |> SELECT id,`'+other_name+'` |> ORDER BY id'
    require(query(instance,joined)=='1\t91\n2\t92','ordinal star filtering deleted a same-named right JOIN field')
    print('PASS native pipeline ORDER continuation/position/DISTINCT/aggregate/JOIN/volatility/PS/view',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('sql-short-ordered') as root:
        with fixture.disposable(root/'server') as instance:
            exercise(instance)


if __name__=='__main__':
    main()
