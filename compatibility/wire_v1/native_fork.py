"""Real TLS/native SQL acceptance for the versioned SDK transport."""
from contextlib import contextmanager
from pathlib import Path
import os
import re
import secrets
import subprocess
import sys
import time
from decimal import Decimal
import argparse

CODE=Path(__file__).resolve().parent
sys.path.insert(1,str(CODE.parent/'tests'))
sys.path.insert(2,str(CODE.parent.parent/'tests'))
from fixture import Fixture,BASEDIR,BIN,SHELL,HIDDEN,MIGRATION,run,query
from service_smoke import certificates,require
from stardataparadise_sdk import connect_native,connect as connect_legacy,NativeJson,NativeTime,Error


@contextmanager
def bridge(command,env,root):
    log=root/'bridge.log'
    with log.open('wb') as output:
        process=subprocess.Popen(command,env=env,stdin=subprocess.DEVNULL,stdout=output,
                                 stderr=subprocess.STDOUT,creationflags=HIDDEN)
        try:
            for _ in range(100):
                text=log.read_text(encoding='utf-8',errors='replace')
                found=re.search(r'^SDP_WIRE_V1_READY (\d+)$',text,re.M)
                if found:
                    yield int(found[1]); return
                require(process.poll() is None,'native bridge exited: '+text)
                time.sleep(.1)
            raise RuntimeError('native bridge did not become ready')
        finally:
            if process.poll() is None: process.terminate(); process.wait(timeout=10)


def check(fixture,root,baseline=False):
    front,back=root/'front',root/'back'
    front.mkdir();back.mkdir()
    openssl=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe'
    for location in (front,back): certificates(str(openssl),location)
    owner,reader=('native_owner',secrets.token_hex(24)),('native_reader',secrets.token_hex(24))
    with fixture.disposable(root/'fork',tls_dir=back) as instance:
        env,classic,_=instance
        port=next(item.split('=',1)[1] for item in classic if item.startswith('--port='))
        require(query(instance,'CREATE DATABASE native_sdk;')=='','native schema creation failed')
        bootstrap=f"""
CREATE TABLE native_sdk.sample(id BIGINT PRIMARY KEY,note TEXT) ENGINE=InnoDB;
CREATE USER '{owner[0]}'@'localhost' IDENTIFIED BY '{owner[1]}' REQUIRE SSL;
CREATE USER '{reader[0]}'@'localhost' IDENTIFIED BY '{reader[1]}' REQUIRE SSL;
GRANT SELECT,INSERT,UPDATE,DELETE,CREATE ROUTINE,ALTER ROUTINE,EXECUTE ON native_sdk.* TO '{owner[0]}'@'localhost';
GRANT SELECT ON native_sdk.* TO '{reader[0]}'@'localhost';
"""
        created=run(classic,env,20,script=bootstrap+'\n')
        require(created.returncode==0,'native fixture accounts failed')
        uuid,version=query(instance,'SELECT @@server_uuid,@@version;').split('\t')
        command=[sys.executable,'-B',str(CODE/'service.py'),'--port','0','--cert',str(front/'server.pem'),
                 '--key',str(front/'server.key'),'--mysql-port',port,'--mysql-ca',str(back/'ca.pem'),
                 '--mysql-uuid',uuid,'--product-version',version.removesuffix('-StarDataParadise'),
                 '--schema','native_sdk','--native-sql']
        with bridge(command,env,root) as gateway:
            def connect(account=owner):
                return connect_native(port=gateway,user=account[0],password=account[1],ca_file=str(front/'ca.pem'),
                                      tls_name='localhost',timeout=75)
            def first(db,sql,values=()): return db.execute(sql,values).sets[0].rows[0]
            with connect() as db:
                value=NativeJson('{"id":9007199254740993,"cost":123.4500}')
                row=first(db,'SELECT CAST(? AS JSON) AS document,CAST(? AS DECIMAL(65,30)) AS exact,CAST(? AS UNSIGNED) AS large,UNHEX(\'00FF\') AS payload',
                          (value,Decimal('12345678901234567890123456789012345.123456789012345678901234567890'),2**64-1))
                require(isinstance(row[0],NativeJson) and row[0].decode()['id']==9007199254740993,'native JSON digits lost')
                require(row[1]==Decimal('12345678901234567890123456789012345.123456789012345678901234567890'),'native decimal lost')
                require(row[2]==2**64-1 and row[3]==b'\0\xff','native unsigned/binary lost')
                for bound in (b'\0\x01\xfe\xff',b'',b'valid UTF8 bytes','Türkçe - 🌌',None):
                    require(first(db,'SELECT ?',(bound,))==(bound,),'bound binary/text/NULL changed value or type')
                for text in ('-01:02:03.000004','25:00:00','-00:00:00.000001'):
                    row=first(db,'SELECT CAST(CAST(? AS CHAR) AS TIME(6)) AS duration',(text,))
                    if not (isinstance(row[0],NativeTime) and row[0].text==text):
                        diagnostic=first(db,'SELECT CAST(CAST(? AS TIME(6)) AS CHAR),HEX(CAST(? AS TIME(6)))',(text,text))
                        raise RuntimeError('native binary TIME differs: expected '+text+' actual '+str(row[0])+' text '+str(diagnostic))
                print('PASS native SQL typed JSON/BIGINT/DECIMAL/binary/negative TIME over TLS',flush=True)
                body='CREATE PROCEDURE native_sdk.echo(IN p BIGINT,OUT result BIGINT) BEGIN SET result=p+1; SELECT result AS emitted; END'
                db.execute(body)
                sets=db.execute('CALL native_sdk.echo(?,@result)',(7,)).sets
                require(any(part.rows==((8,),) for part in sets),'prepared CALL result lost')
                require(first(db,'SELECT @result')==(8,),'native OUT capture lost')
                db.execute("CREATE PROCEDURE native_sdk.text_out(OUT q VARCHAR(32) CHARACTER SET utf8mb4) SET q='Türkçe'")
                declared=db.execute('CALL native_sdk.text_out(?)',(b'\xff',)).sets
                require([part.rows[0][0] for part in declared if part.rows]==['Türkçe'],
                        'declared text OUT did not override the binary input metadata')
                if not baseline:
                    db.execute('CREATE FUNCTION native_sdk.short_value(p BIGINT) RETURNS BIGINT DETERMINISTIC AS RETURN :p+1')
                    require(first(db,'SELECT native_sdk.short_value(?)',(9,))==(10,),'native AS RETURN/binder failed through SDK')
                print('PASS unsplit procedure body and prepared CALL/OUT/multiple sets'+(' (baseline transport only)' if baseline else ' and native short function'),flush=True)
                text="'); DELETE FROM native_sdk.sample; -- ? :p"
                db.begin();db.execute('INSERT INTO native_sdk.sample VALUES (?,?)',(1,text));db.commit()
                require(first(db,'SELECT note FROM native_sdk.sample WHERE id=?',(1,))==(text,),'native binding was interpolated')
                db.begin();db.execute('INSERT INTO native_sdk.sample VALUES (2,\'rollback\')');db.rollback()
                require(first(db,'SELECT COUNT(*) FROM native_sdk.sample')==(1,),'native rollback failed')
                try: db.execute('INSERT INTO native_sdk.sample VALUES (100,\'a\'); INSERT INTO native_sdk.sample VALUES (101,\'b\')')
                except Error as error: require(error.native_errno==1064,'native multi-statement rejection failed')
                else: raise RuntimeError('native transport enabled multiple top-level statements')
                require(first(db,'SELECT COUNT(*) FROM native_sdk.sample')==(1,),'rejected batch wrote a prefix')
                try: db._wire.call('tx_begin')
                except Error as error: require(error.code==1013,'native and legacy lifecycle mixed')
                else: raise RuntimeError('legacy transaction entered a native session')
                huge=first(db,"SELECT JSON_OBJECT('text',REPEAT('İğ',200000)) AS document")[0]
                require(huge.decode()['text']=='İğ'*200000,'chunked native JSON corrupted')
                print('PASS native transactions, bound values, single statement guard and large UTF-8 result chunks',flush=True)
                db.execute("CREATE PROCEDURE native_sdk.commit_signal() SQL SECURITY INVOKER BEGIN INSERT INTO native_sdk.sample VALUES (3,'durable'); COMMIT; SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='after commit'; END")
                try: db.execute('CALL native_sdk.commit_signal()')
                except Error as error:
                    require(error.native_errno==1644 and error.unknown_outcome and error.commit_state=='unknown','routine failure claimed rollback')
                else: raise RuntimeError('SIGNAL did not fail')
                require(first(db,'SELECT COUNT(*) FROM native_sdk.sample WHERE id=3')==(1,),'pre-SIGNAL commit was not durable')
                print('PASS routine commit before SIGNAL remains durable and is reported conservatively',flush=True)
            db=connect()
            try:
                try: db.execute("SELECT CAST('0000-00-00' AS DATE),NULL")
                except Error as error: require(error.code==1004,'zero date did not produce explicit representation error')
                else: raise RuntimeError('zero DATE silently became SQL NULL')
            finally: db.close()
            db=connect(reader)
            try:
                try: db.execute("INSERT INTO native_sdk.sample VALUES (4,'denied')")
                except Error as error: require(error.native_errno==1142,'native invoker ACL was bypassed')
                else: raise RuntimeError('native SDK bypassed invoker INSERT privilege')
            finally: db.close()
            print('PASS explicit zero-date rejection and original caller ACL enforcement',flush=True)
            with connect_legacy(port=gateway,user=owner[0],password=owner[1],ca_file=str(front/'ca.pem'),
                                tls_name='localhost',autocommit=True) as legacy:
                with legacy.cursor() as cursor:
                    cursor.execute('SELECT note FROM sample WHERE id=:id',{'id':1})
                    require(cursor.fetchone()==(text,),'native-enabled bridge changed the legacy default-schema read')
            print('PASS native-enabled authentication retains ordinary legacy schema/CRUD behavior',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--baseline',action='store_true',help='Transport/ACL acceptance only; do not claim new native grammar support.')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('native-sdk') as root: check(fixture,root,args.baseline)


if __name__=='__main__': main()
