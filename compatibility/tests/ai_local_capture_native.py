"""Native SP-local JSON/TEXT capture, pending guards and lifetime acceptance.

Uses an explicit application-supplied response, without provider inference or
transaction shims. All statements are executed by the native SQL parser.
"""
import argparse
from pathlib import Path

from .ai_catalog_acceptance import call, execute, install
from .fixture import BASEDIR, BIN, MIGRATION, SHELL, Fixture, query, require, run
from .workbench_native import backend_certificate


def exercise(instance):
    install(instance,False)
    execute(instance,call('create','model','m',dict(provider='OPENAI',remote_model='fixture',settings=dict(secret_env='FIXTURE_KEY'))))
    execute(instance,call('create','conversation','c',dict(model_schema='fixture',model_name='m',functions=0,auto_append=1,persistent=1)))
    execute(instance,'''CREATE TABLE fixture.capture_sink(value JSON) ENGINE=InnoDB;
      DELIMITER //
      CREATE PROCEDURE fixture.local_json()
      BEGIN
        DECLARE answer JSON DEFAULT JSON_OBJECT('old',TRUE);
        DECLARE metadata JSON;
        DECLARE copied JSON;
        DECLARE CONTINUE HANDLER FOR SQLSTATE '55000' SET @pending_guards=COALESCE(@pending_guards,0)+1;
        SET @pending_guards=0;
        START AI CONVERSATION fixture.c;
        APPEND RESPONSE INTO :answer AS JSON RETURNING METADATA INTO :metadata;
        SELECT :answer;
        SET @guard_after_send=@pending_guards;
        SET copied=:answer;
        SET @guard_after_copy=@pending_guards;
        INSERT INTO fixture.capture_sink VALUES(:answer);
        SET @guard_after_insert=@pending_guards;
        SET answer=JSON_OBJECT('must_not_overwrite_pending',TRUE);
        SET @guard_after_write=@pending_guards;
        APPEND RESPONSE JSON_OBJECT('id',18446744073709551615,'nothing',NULL,'city','İstanbul') AS JSON;
        COMMIT;
        SELECT @guard_after_send,@guard_after_copy,@guard_after_insert,@guard_after_write,
          @pending_guards,JSON_TYPE(:answer),JSON_TYPE(:metadata),
          JSON_UNQUOTE(JSON_EXTRACT(:answer,'$.id')),
          JSON_UNQUOTE(JSON_EXTRACT(:answer,'$.city')),
          JSON_UNQUOTE(JSON_EXTRACT(:metadata,'$.source'));
      END//
      CREATE PROCEDURE fixture.local_text()
      AS
        LET answer LONGTEXT='stale';
        LET metadata JSON=NULL;
        START AI CONVERSATION fixture.c;
        APPEND RESPONSE AS TEXT INTO :answer RETURNING METADATA INTO :metadata;
        APPEND RESPONSE 'Şehir ve boşluk' AS TEXT;
        COMMIT;
        SELECT :answer,JSON_TYPE(:metadata);
      END//
      CREATE PROCEDURE fixture.expired_local()
      AS
        LET answer JSON=NULL;
        START TRANSACTION;
        INSERT INTO fixture.ledger VALUES(99);
        START AI CONVERSATION fixture.c;
        APPEND RESPONSE INTO :answer AS JSON;
        APPEND RESPONSE JSON_OBJECT('value',1) AS JSON;
      END//
      CREATE PROCEDURE fixture.bad_local_type()
      AS
        LET answer BIGINT=12;
        START AI CONVERSATION fixture.c;
        APPEND RESPONSE INTO :answer AS JSON;
      END//
      CREATE PROCEDURE fixture.bad_local_format()
      AS
        LET answer TEXT='stale';
        START AI CONVERSATION fixture.c;
        APPEND RESPONSE INTO :answer AS JSON;
      END//
      CREATE PROCEDURE fixture.async_local()
      AS
        LET answer JSON=NULL;
        START TRANSACTION;
        INSERT INTO fixture.ledger VALUES(100);
        START AI CONVERSATION fixture.c;
        APPEND RESPONSE INTO :answer AS JSON;
        APPEND RESPONSE JSON_OBJECT('value',1) AS JSON;
        COMMIT ASYNC;
      END//
      CREATE PROCEDURE fixture.native_fetch_cursor()
      BEGIN
        DECLARE value_number INT;
        DECLARE response CURSOR FOR SELECT 42;
        OPEN response;
        FETCH response INTO value_number;
        CLOSE response;
        SELECT value_number;
      END//
      CREATE PROCEDURE fixture.native_next_cursor()
      BEGIN
        DECLARE value_number INT;
        DECLARE next CURSOR FOR SELECT 43;
        OPEN next;
        FETCH next INTO value_number;
        CLOSE next;
        SELECT value_number;
      END//
      DELIMITER ;''')
    result=execute(instance,'CALL fixture.local_json();')
    require(result.splitlines()[-1]=='1\t2\t3\t4\t4\tOBJECT\tOBJECT\t18446744073709551615\tİstanbul\tapplication',
            'SP-local pending read/send/save guard or actual native JSON field publication failed: '+result)
    require(query(instance,'SELECT COUNT(*) FROM fixture.capture_sink')=='0','pending JSON was inserted before commit')
    require(execute(instance,'CALL fixture.local_text();').splitlines()[-1]=='Şehir ve boşluk\tOBJECT',
            'typed local TEXT or JSON metadata changed during native capture')
    execute(instance,'CALL fixture.bad_local_type();',61026)
    execute(instance,'CALL fixture.bad_local_format();',61026)
    execute(instance,'CALL fixture.async_local();',61005)
    require(query(instance,'SELECT COUNT(*) FROM fixture.ledger WHERE id=100')=='0','ASYNC local capture committed business data')
    env,command,_=instance
    result=run([*command,'--force'],env,60,script='CALL fixture.expired_local();\nCOMMIT;\nSELECT STARDATAPARADISE_TRANSACTION_ACTIVE();\nROLLBACK;\n')
    require('ERROR 61026 (' in result.stderr and result.stdout.strip().splitlines()[-1]=='1',
            'expired native frame was reused or rejected after committing: '+result.stdout+result.stderr)
    require(query(instance,'SELECT COUNT(*) FROM fixture.ledger WHERE id=99')=='0','expired capture escaped rollback')
    require(execute(instance,'CALL fixture.native_fetch_cursor();')=='42','AI FETCH shadowed native cursor named response')
    require(execute(instance,'CALL fixture.native_next_cursor();')=='43','FETCH factoring broke native cursor named next')
    # Kind/name JSON alone cannot manufacture a trusted native local offset.
    forged=dict(format='JSON',options={},capture=dict(format='JSON',value_target=dict(kind='local_variable',name='forged')))
    execute(instance,'START AI CONVERSATION fixture.c;'+call('response_options','conversation',payload=forged)+'COMMIT;',61026)
    print('PASS native AI-in-SP/local JSON/TEXT, send/save/value guards, lifetime/ASYNC precommit checks and native FETCH compatibility',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--openssl',type=Path,default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-local-capture') as root:
        certificates=backend_certificate(args.openssl.resolve(strict=True),root,'cert')
        with fixture.disposable(root/'server',tls_dir=certificates,server_options=('--sdp-ai-sql=ON',)) as instance:
            exercise(instance)


if __name__=='__main__':
    main()
