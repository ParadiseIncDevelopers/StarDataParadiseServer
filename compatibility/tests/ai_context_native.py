"""Typed Knowledge table source, native context and metadata lifecycle acceptance."""
import argparse
from decimal import Decimal
import json
from pathlib import Path
from .ai_catalog_acceptance import call,execute,install,literal
from .fixture import BASEDIR,BIN,MIGRATION,SHELL,Fixture,query,require
from .workbench_native import backend_certificate


def exercise(instance,full=False):
    import mysql.connector
    from mysql.connector.constants import FieldFlag,FieldType
    install(instance,False)
    execute(instance,"""CREATE TABLE fixture.source_rows(id BIGINT UNSIGNED PRIMARY KEY,amount DECIMAL(38,9),label VARCHAR(60) CHARACTER SET utf8mb4) ENGINE=InnoDB;
      INSERT INTO fixture.source_rows VALUES(18446744073709551615,12345678901234567890123456789.123456789,'Türkçe'),(2,2.500000000,NULL);
      CREATE KNOWLEDGE fixture.source_k(p BIGINT UNSIGNED NOT NULL)
      RETURNS TABLE(id BIGINT UNSIGNED NOT NULL,amount DECIMAL(38,9) NULL,label VARCHAR(60) NULL)
      READ ONLY SQL SECURITY INVOKER AS SELECT id,amount,label FROM fixture.source_rows WHERE id=:p;
      CREATE TABLE fixture.knowledge(id INT);INSERT INTO fixture.knowledge VALUES(7);
      CREATE TABLE fixture.behavior(id INT);INSERT INTO fixture.behavior VALUES(9);
    """)
    expected='18446744073709551615\t12345678901234567890123456789.123456789\tTürkçe'
    require(execute(instance,'SELECT k.id,k.amount,k.label FROM KNOWLEDGE fixture.source_k(18446744073709551615) AS k;')==expected,'table source lost uint64/Decimal/UTF8')
    require(execute(instance,'SELECT k.id,k.amount,k.label FROM KNOWLEDGE fixture.source_k(2) AS k;')=='2\t2.500000000\tNULL','nullable table source lost SQL NULL')
    require(execute(instance,'SELECT k.id,t.id FROM KNOWLEDGE fixture.source_k(2) AS k LEFT JOIN fixture.source_rows t ON t.id=k.id;')=='2\t2','typed source could not join ordinary tables')
    require(execute(instance,'SELECT k.id FROM fixture.knowledge AS k;')=='7','legacy knowledge table alias was stolen by native source grammar')
    execute(instance,'INSERT INTO fixture.behavior VALUES(10);DELETE FROM fixture.behavior WHERE id=10;')
    require(execute(instance,'SELECT b.id FROM fixture.behavior b;')=='9','legacy behavior INSERT/DELETE changed')
    execute(instance,'START TRANSACTION;INSERT INTO fixture.ledger VALUES(1);SELECT k.id FROM KNOWLEDGE fixture.source_k(2) AS k;ROLLBACK;')
    require(query(instance,'SELECT COUNT(*) FROM fixture.ledger')=='0','table source committed caller DML')
    execute(instance,"""DELIMITER $$
      CREATE FUNCTION fixture.context_write() RETURNS INT MODIFIES SQL DATA
      BEGIN INSERT INTO fixture.ledger VALUES(2);RETURN 2;END$$
      DELIMITER ;""")
    execute(instance,'SELECT k.id FROM KNOWLEDGE fixture.source_k(fixture.context_write()) AS k;',61006)
    require(query(instance,'SELECT COUNT(*) FROM fixture.ledger')=='0','table source evaluated impure arguments')
    options={value[2:].split('=',1)[0]:value.split('=',1)[1] for value in instance[1] if value.startswith('--') and '=' in value}
    connection=mysql.connector.connect(host='127.0.0.1',port=int(options['port']),user='root',password='',
      ssl_ca=options['ssl-ca'],ssl_verify_cert=True,ssl_verify_identity=True,autocommit=True,use_pure=True)
    try:
        cursor=connection.cursor(prepared=True)
        try:
            cursor.execute('SELECT k.id,k.amount,k.label FROM KNOWLEDGE fixture.source_k(?) AS k',(18446744073709551615,))
            result=cursor.fetchall();metadata=cursor.description
            normalized=[tuple(value.decode('utf-8') if isinstance(value,bytes) and metadata[index][8]!=63 else value for index,value in enumerate(row)) for row in result]
            require(normalized==[(18446744073709551615,Decimal('12345678901234567890123456789.123456789'),'Türkçe')],'prepared source exact data changed: '+repr(normalized))
            require(metadata[0][1]==FieldType.LONGLONG and metadata[0][7]&FieldFlag.UNSIGNED and metadata[1][1]==FieldType.NEWDECIMAL,'source declared wire types were lost')
            cursor.execute('SELECT k.id,k.amount,k.label FROM KNOWLEDGE fixture.source_k(?) AS k',(2,))
            require(cursor.fetchall()==[(2,Decimal('2.500000000'),None)],'prepared source rebinding failed')
        finally:cursor.close()
    finally:connection.close()
    execute(instance,call('create','model','m',dict(provider='OPENAI',remote_model='fixture',settings=dict(secret_env='FIXTURE_KEY'))))
    execute(instance,'CREATE AI CONVERSATION fixture.c USING MODEL fixture.m NO FUNCTIONS MEMORY PERSISTENT;')
    execute(instance,"START AI CONVERSATION fixture.c;APPEND CONTEXT AS order_data FROM KNOWLEDGE fixture.source_k(18446744073709551615);COMMIT ASYNC;FINISH;")
    require(query(instance,'SELECT COUNT(*) FROM sdp_ai.runs')=='0','context-only turn requested model inference')
    messages=json.loads(query(instance,'SELECT messages FROM sdp_ai.conversations'))
    require(len(messages)==1 and messages[0]['role']=='user' and 'Structured context data - order_data' in messages[0]['content'],'native context was lost or promoted to instructions')
    content=json.loads(messages[0]['content'].split('\n',1)[1])
    require(content['rows']==[dict(id=18446744073709551615,amount='12345678901234567890123456789.123456789',label='Türkçe')],'context numeric values were rounded')
    execute(instance,"CREATE BEHAVIOR fixture.policy AS 'first';ALTER BEHAVIOR fixture.policy AS 'second';")
    execute(instance,'INSERT BEHAVIOR fixture.policy WITH(version=1,priority=-5) INTO CONVERSATION fixture.c;')
    doc=json.loads(execute(instance,'DESCRIBE CONVERSATION fixture.c;'))
    require(doc['settings']['behaviors'][0]['version']==1 and doc['settings']['behaviors'][0]['priority']==-5,'behavior native version/priority pin failed')
    execute(instance,'DELETE BEHAVIOR fixture.policy FROM CONVERSATION fixture.c;')
    execute(instance,'ALTER AI CONVERSATION fixture.c WITH(auto_append=FALSE,persistent=FALSE);ALTER AI CONVERSATION fixture.c RENAME TO renamed;ALTER AI CONVERSATION fixture.renamed ARCHIVE;')
    execute(instance,'START AI CONVERSATION fixture.renamed;',61011)
    execute(instance,'ALTER AI CONVERSATION fixture.renamed UNARCHIVE;START AI CONVERSATION fixture.renamed;FINISH;')
    execute(instance,"CREATE RESPONSE SCHEMA fixture.shape VERSION 1 AS JSON '{\"type\":\"object\"}';")
    execute(instance,"START AI CONVERSATION fixture.renamed;APPEND RESPONSE '{\"ok\":true}' AS JSON WITH(schema='fixture.shape',schema_version=1);COMMIT;FINISH;")
    if full:
        require(execute(instance,'SELECT k.id,t.id FROM KNOWLEDGE fixture.source_k(2) AS k FULL OUTER JOIN fixture.source_rows t ON t.id=k.id ORDER BY t.id;')=='2\t2\nNULL\t18446744073709551615','FULL did not use the trusted typed source child')
        maximum=18446744073709551615
        directions=[
          ('SELECT k.id,t.id FROM KNOWLEDGE fixture.source_k(?) AS k FULL OUTER JOIN fixture.source_rows t ON t.id=k.id ORDER BY t.id',
           {2:[(2,2),(None,maximum)],maximum:[(None,2),(maximum,maximum)],99:[(None,2),(None,maximum)]}),
          ('SELECT t.id,k.id FROM fixture.source_rows t FULL OUTER JOIN KNOWLEDGE fixture.source_k(?) AS k ON t.id=k.id ORDER BY t.id',
           {2:[(2,2),(maximum,None)],maximum:[(2,None),(maximum,maximum)],99:[(2,None),(maximum,None)]})]
        connection=mysql.connector.connect(host='127.0.0.1',port=int(options['port']),user='root',password='',
          ssl_ca=options['ssl-ca'],ssl_verify_cert=True,ssl_verify_identity=True,autocommit=True,use_pure=True)
        try:
            for source,expected_rows in directions:
                prepared=connection.cursor(prepared=True)
                try:
                    for value in (2,maximum,2,99,2):
                        prepared.execute(source,(value,))
                        rows=prepared.fetchall()
                        require(rows==expected_rows[value],'FULL typed source/spool retained old PS rows or lost unmatched rows: '+repr((source,value,rows)))
                        require(all(column[1]==FieldType.LONGLONG and column[7]&FieldFlag.UNSIGNED for column in prepared.description),
                          'FULL typed source lost exact uint64 wire metadata')
                finally:prepared.close()
        finally:connection.close()
        print('PASS typed Knowledge FULL children: left/right spool, repeated prepared rebind, empty source, unmatched rows and exact uint64 metadata')
    print('PASS Knowledge table/context source: typed uint64/Decimal/UTF8/NULL, joins, real prepared rebinding, rollback and prebind purity')
    print('PASS native conversation ALTER/rename/archive, behavior pinned insertion/removal and reserved schema option')
    print('External providers: NOT_RUN (context-only and application-supplied response)')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--openssl',type=Path,default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    parser.add_argument('--full',action='store_true')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-context-native') as root:
        cert=backend_certificate(args.openssl.resolve(strict=True),root,'cert')
        with fixture.disposable(root/'server',tls_dir=cert,server_options=('--sdp-ai-sql=ON',)) as instance:exercise(instance,args.full)


if __name__=='__main__':main()
