"""Stored Knowledge typed compiler/invoker acceptance on a disposable Server.

This suite requires the native Knowledge overlay; it has no routine-only shim.
Provider requests, local model inference and UDF initialization are not claimed.
"""
import argparse
import json
from pathlib import Path
from decimal import Decimal

from .ai_catalog_acceptance import call, execute, install, literal
from .fixture import BASEDIR, BIN, MIGRATION, SHELL, Fixture, query, require
from .workbench_native import backend_certificate


def binary_protocol(instance):
    import mysql.connector
    from mysql.connector.constants import FieldFlag, FieldType
    command=instance[1]
    options={value[2:].split('=',1)[0]:value.split('=',1)[1]
             for value in command if value.startswith('--') and '=' in value}
    connection=mysql.connector.connect(host='127.0.0.1',port=int(options['port']),user='root',password='',
      ssl_ca=options['ssl-ca'],ssl_verify_cert=True,ssl_verify_identity=True,autocommit=True,use_pure=True)
    def decode_text(value,column):
        # Pinned Connector/Python returns bytes for a binary-comparison flag
        # even when the wire charset is UTF-8. Binary columns stay bytes.
        if isinstance(value,bytes) and column[1] in (FieldType.STRING,FieldType.VAR_STRING,FieldType.VARCHAR) and column[8]!=63:
            return value.decode('utf-8')
        return value
    try:
        cursor=connection.cursor(prepared=True)
        try:
            cursor.execute('EXECUTE KNOWLEDGE fixture.order_knowledge(?)',(18446744073709551615,))
            row=cursor.fetchall()
            columns=cursor.description
            row=[tuple(decode_text(value,columns[index]) for index,value in enumerate(record)) for record in row]
            require(row==[(18446744073709551615,Decimal('12345678901234567890123456789.123456789'),'Türkçe')],
                    'native prepared protocol lost exact typed Knowledge values: '+repr(row)+' metadata='+repr(columns))
            require(columns[0][0]=='order_id' and columns[0][1]==FieldType.LONGLONG and
                    columns[0][7]&FieldFlag.UNSIGNED and columns[0][7]&FieldFlag.NOT_NULL,
                    'declared uint64 result metadata was lost')
            require(columns[1][1]==FieldType.NEWDECIMAL and columns[1][7]&FieldFlag.NOT_NULL and
                    not(columns[2][7]&FieldFlag.NOT_NULL),'declared Decimal/nullability metadata was lost')
            while cursor.nextset():
                if cursor.with_rows:cursor.fetchall()
            text="SELECT fixture.mutate(); -- bound data, not SQL"
            cursor.execute('EXECUTE KNOWLEDGE fixture.echo_sql_text(?)',(text,))
            echoed=cursor.fetchall()
            require([(decode_text(record[0],cursor.description[0]),) for record in echoed]==[(text,)],'prepared parameter data was interpreted as SQL source')
            while cursor.nextset():
                if cursor.with_rows:cursor.fetchall()
            cursor.execute('EXECUTE KNOWLEDGE fixture.order_knowledge(?)',(2,))
            require(cursor.fetchall()==[(2,Decimal('2.500000000'),None)],'prepared rebinding or nullable result failed')
            while cursor.nextset():
                if cursor.with_rows:cursor.fetchall()
        finally:
            cursor.close()
    finally:
        connection.close()


def prepared_json_arguments(instance):
    """Compound argument types resolve before binary PS values are bound."""
    from .ai_commit_guard_native import connection
    execute(instance,"""CREATE KNOWLEDGE fixture.json_arguments(p JSON NULL)
      RETURNS TABLE(kind VARCHAR(16) NULL,content JSON NULL) READ ONLY SQL SECURITY INVOKER
      AS SELECT JSON_TYPE(:p),:p;""")
    client=connection(instance)
    try:
        cursor=client.cursor(prepared=True)
        try:
            sql='EXECUTE KNOWLEDGE fixture.json_arguments(CAST(? AS JSON))'
            values=[('null','NULL',None),('{"x":9007199254740993}','OBJECT',{'x':9007199254740993}),
                    ('123','INTEGER',123),('true','BOOLEAN',True),('"Türkçe"','STRING','Türkçe'),
                    ('[1,null,{"x":9007199254740993}]','ARRAY',[1,None,{'x':9007199254740993}])]
            for source,kind,expected in values:
                cursor.execute(sql,(source,));rows=cursor.fetchall()
                while cursor.nextset():
                    if cursor.with_rows:cursor.fetchall()
                require(len(rows)==1 and len(rows[0])==2,'prepared JSON result shape changed: '+repr(rows))
                actual_kind,content=rows[0]
                if isinstance(actual_kind,(bytes,bytearray)):actual_kind=actual_kind.decode('utf-8')
                require(actual_kind==kind and content is not None and json.loads(content)==expected,
                        'prepared Knowledge JSON argument lost its native type/value: '+repr(rows))
            cursor.execute(sql,(None,));rows=cursor.fetchall()
            while cursor.nextset():
                if cursor.with_rows:cursor.fetchall()
            require(rows==[(None,None)],'prepared SQLNULL became JSONnull: '+repr(rows))
        finally:cursor.close()
    finally:client.close()
    print('PASS prepared Knowledge JSON null/object/scalar/string/array, exact integer, same-handle rebind and SQLNULL distinction')


def cached_validator_recovery(instance):
    """A handled rejection cannot leave a cached SP expression SQL-NULL."""
    import mysql.connector
    from .ai_commit_guard_native import statement
    options={value[2:].split('=',1)[0]:value.split('=',1)[1]
             for value in instance[1] if value.startswith('--') and '=' in value}
    connection=mysql.connector.connect(host='127.0.0.1',port=int(options['port']),user='root',password='',
      ssl_ca=options['ssl-ca'],ssl_verify_cert=True,ssl_verify_identity=True,autocommit=True,use_pure=True)
    try:
        cursor=connection.cursor()
        try:
            try:
                statement(cursor,"""CREATE KNOWLEDGE fixture.cached_invalid() RETURNS TABLE(n BIGINT NULL)
                  READ ONLY SQL SECURITY INVOKER AS SELECT SLEEP(0)""")
            except mysql.connector.Error as error:
                require(error.errno==61006,'cached Knowledge rejection had the wrong error: '+repr(error))
            else:
                raise RuntimeError('cached Knowledge validator accepted a forbidden expression')
            statement(cursor,"""CREATE KNOWLEDGE fixture.cached_valid() RETURNS TABLE(n BIGINT NULL)
              READ ONLY SQL SECURITY INVOKER AS SELECT 41""")
            rows=statement(cursor,'DESCRIBE KNOWLEDGE fixture.cached_valid')
            document=json.loads(rows[0][0])
            require(document.get('result_columns')==[dict(name='n',type='BIGINT',nullable=True)],
                    'successful cached Knowledge validation retained the prior NULL state: '+repr(document))
            require(statement(cursor,'EXECUTE KNOWLEDGE fixture.cached_valid()')==[(41,)],
                    'Knowledge could not execute after a handled validator rejection')
        finally:
            cursor.close()
    finally:
        connection.close()
    print('PASS cached Knowledge validator: handled rejection then typed create/describe/execute in the same session')


def exercise(instance):
    install(instance, False)
    execute(instance,"""CREATE TABLE fixture.knowledge_rows(
      id BIGINT UNSIGNED PRIMARY KEY, amount DECIMAL(38,9) NOT NULL,
      label VARCHAR(60) CHARACTER SET utf8mb4 NULL) ENGINE=InnoDB;
      INSERT INTO fixture.knowledge_rows VALUES
        (18446744073709551615,12345678901234567890123456789.123456789,'Türkçe'),
        (2,2.500000000,NULL);
    """)
    declaration="""CREATE KNOWLEDGE fixture.order_knowledge(p_id BIGINT UNSIGNED NOT NULL)
      RETURNS TABLE(order_id BIGINT UNSIGNED NOT NULL,amount DECIMAL(38,9) NOT NULL,label VARCHAR(60) NULL)
      READ ONLY SQL SECURITY INVOKER AS
      SELECT id,amount,label FROM fixture.knowledge_rows WHERE id=:p_id;"""
    execute(instance,declaration)
    execute(instance,"""CREATE KNOWLEDGE fixture.echo_sql_text(p VARCHAR(100))
      RETURNS TABLE(script_text VARCHAR(100)) READ ONLY SQL SECURITY INVOKER AS SELECT :p;""")
    expected='18446744073709551615\t12345678901234567890123456789.123456789\tTürkçe'
    require(execute(instance,'EXECUTE KNOWLEDGE fixture.order_knowledge(18446744073709551615);')==expected,
            'typed Knowledge lost uint64, Decimal precision, UTF-8 or declared column shape')
    require(execute(instance,'EXECUTE KNOWLEDGE fixture.order_knowledge(2);')=='2\t2.500000000\tNULL',
            'nullable typed result was not preserved')
    binary_protocol(instance)
    prepared_json_arguments(instance)
    execute(instance,'EXECUTE KNOWLEDGE fixture.order_knowledge(NULL);',61042)
    description=json.loads(execute(instance,call('describe','knowledge','order_knowledge')))
    require(description['parameters'][0]['type']=='BIGINT UNSIGNED','parameter metadata was not normalized')
    require(description['result_columns'][1]['type']=='DECIMAL(38,9)','Decimal declaration was not durable')
    require(any(item['kind']=='Table' and item['name']=='knowledge_rows' and item['object_id']
                for item in description['dependencies']),'native table dependency identity was lost')
    require(query(instance,"SELECT COUNT(*) FROM information_schema.routines WHERE ROUTINE_SCHEMA='fixture'")=='0',
            'Knowledge compilation created a persistent hidden routine')

    # Anonymous typed programs preserve the caller's transaction.
    transaction='START TRANSACTION;INSERT INTO fixture.ledger VALUES(100);EXECUTE KNOWLEDGE fixture.order_knowledge(2);ROLLBACK;'
    execute(instance,transaction)
    require(query(instance,'SELECT COUNT(*) FROM fixture.ledger')=='0','Knowledge implicitly committed user SQL')
    cte="""CREATE KNOWLEDGE fixture.cte_knowledge(p BIGINT UNSIGNED)
      RETURNS TABLE(id BIGINT UNSIGNED) READ ONLY SQL SECURITY INVOKER AS
      WITH selected_rows AS (SELECT id FROM fixture.knowledge_rows)
      SELECT id FROM selected_rows WHERE id=:p;"""
    execute(instance,cte)
    require(execute(instance,'EXECUTE KNOWLEDGE fixture.cte_knowledge(2);')=='2','readonly CTE binding failed')
    execute(instance,"""CREATE KNOWLEDGE fixture.bad_count() RETURNS TABLE(id INT)
      READ ONLY SQL SECURITY INVOKER AS SELECT 1,2;""")
    execute(instance,'EXECUTE KNOWLEDGE fixture.bad_count();',61042)
    execute(instance,"""CREATE KNOWLEDGE fixture.bad_null() RETURNS TABLE(id INT NOT NULL)
      READ ONLY SQL SECURITY INVOKER AS SELECT NULL;""")
    execute(instance,'EXECUTE KNOWLEDGE fixture.bad_null();',61042)
    execute(instance,"""CREATE KNOWLEDGE fixture.bad_range() RETURNS TABLE(id TINYINT)
      READ ONLY SQL SECURITY INVOKER AS SELECT 1000;""")
    execute(instance,'EXECUTE KNOWLEDGE fixture.bad_range();',1264)

    # The catalog's direct JSON entry cannot assert readonly safety itself.
    shape=dict(parameters=[],result_columns=[dict(name='n',type='BIGINT',nullable=True)])
    forbidden=["INSERT INTO fixture.ledger VALUES(200)",
               "SELECT 1 INTO @knowledge_side_effect", "SELECT GET_LOCK('knowledge_gate',0)",
               "SELECT SLEEP(0)", "SELECT 1 INTO OUTFILE 'knowledge-unexpected.txt'",
               "SELECT 1; INSERT INTO fixture.ledger VALUES(201)"]
    for index,body in enumerate(forbidden):
        execute(instance,call('create','knowledge','unsafe_%d'%index,dict(shape,definition=body)),61006)
    require(query(instance,'SELECT COUNT(*) FROM fixture.ledger')=='0','readonly rejection executed business DML')
    execute(instance,"""DELIMITER $$
      CREATE FUNCTION fixture.mutate() RETURNS INT DETERMINISTIC MODIFIES SQL DATA
      BEGIN INSERT INTO fixture.ledger VALUES(202); RETURN 1; END$$
      DELIMITER ;
    """)
    execute(instance,call('create','knowledge','stored_function',dict(shape,definition='SELECT fixture.mutate()')),61006)
    execute(instance,'EXECUTE KNOWLEDGE fixture.order_knowledge(fixture.mutate());',61006)
    from .classic_binary import binary_connection
    with binary_connection(instance) as client:
        try:prepared,_,_=client.prepare('EXECUTE KNOWLEDGE fixture.order_knowledge(fixture.mutate())')
        except RuntimeError as error:require('native error 61006:' in str(error),'PS argument purity failed with '+str(error))
        else:
            client.close_statement(prepared)
            raise RuntimeError('PS preparation accepted a writing routine argument')
    require(query(instance,'SELECT COUNT(*) FROM fixture.ledger')=='0','stored function executed before readonly rejection')
    execute(instance,'CREATE VIEW fixture.knowledge_view AS SELECT id FROM fixture.knowledge_rows;')
    execute(instance,call('create','knowledge','view_blocked',dict(shape,definition='SELECT id FROM fixture.knowledge_view')),61006)
    cached_validator_recovery(instance)

    # Object USE/EXECUTE and base-table SELECT are independent invoker checks.
    execute(instance,"CREATE USER knowledge_reader@localhost IDENTIFIED BY '' REQUIRE SSL;GRANT EXECUTE ON sdp_ai.* TO knowledge_reader@localhost;GRANT SELECT ON fixture.knowledge_rows TO knowledge_reader@localhost;")
    env,command,cli=instance
    other=(env,['--user=knowledge_reader' if value=='--user=root' else value for value in command],cli)
    actor=query(other,'SELECT STARDATAPARADISE_ACTOR()')
    execute(other,'EXECUTE KNOWLEDGE fixture.order_knowledge(2);',61081)
    execute(instance,"INSERT INTO sdp_ai.object_grants(object_id,principal,privilege_name,granted_by) SELECT object_id,%s,'EXECUTE',STARDATAPARADISE_ACTOR() FROM sdp_ai.objects WHERE kind='knowledge' AND name='order_knowledge';"%literal(actor))
    require(execute(other,'EXECUTE KNOWLEDGE fixture.order_knowledge(2);')=='2\t2.500000000\tNULL',
            'authorized invoker could not execute Knowledge')
    execute(instance,'REVOKE SELECT ON fixture.knowledge_rows FROM knowledge_reader@localhost;')
    execute(other,'EXECUTE KNOWLEDGE fixture.order_knowledge(2);',1142)

    # An object with the same SQL name is not the original table dependency.
    execute(instance,'DROP VIEW fixture.knowledge_view;DROP TABLE fixture.knowledge_rows;CREATE TABLE fixture.knowledge_rows(id BIGINT UNSIGNED,amount DECIMAL(38,9),label VARCHAR(60));')
    execute(instance,'EXECUTE KNOWLEDGE fixture.order_knowledge(2);',61041)
    print('PASS native Knowledge: typed compiler, uint64/Decimal/UTF-8/NULL results, CTE, shape/range validation and caller rollback')
    print('PASS native Knowledge: prebind readonly AST rejection, invoker object/table ACL, dependency identity and no persistent routine')
    print('PASS native Knowledge prepared protocol: exact typed uint64/Decimal/NULL, declared wire metadata and rebinding')
    print('Views: explicitly unsupported; UDF init fixture and provider execution: NOT_RUN')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--openssl',type=Path,default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('knowledge-native') as root:
        certificates=backend_certificate(args.openssl.resolve(strict=True),root,'cert')
        with fixture.disposable(root/'server',tls_dir=certificates,server_options=('--sdp-ai-sql=ON',)) as instance:
            exercise(instance)


if __name__=='__main__':
    main()
