"""Disposable SQL-only AI catalog upgrade: rows, exact grants and additive columns.

Routine definitions may reference newer native builtins. This test does not
invoke those builtins and is not a native tool/approval acceptance test.
"""
import argparse
from pathlib import Path

from .ai_catalog_acceptance import execute, install
from .fixture import BASEDIR, BIN, MIGRATION, REPOSITORY, SHELL, Fixture, query, require
from .workbench_native import backend_certificate


def exercise(instance, catalog):
    install(instance, False)
    execute(instance, (catalog/'tool_catalog.sql').read_text(encoding='utf-8'))
    execute(instance, (catalog/'action_catalog.sql').read_text(encoding='utf-8'))
    execute(instance, """CREATE USER upgrade_app@localhost IDENTIFIED BY '' REQUIRE SSL;
      CREATE USER upgrade_worker@localhost IDENTIFIED BY '' REQUIRE SSL;
      GRANT EXECUTE,ALTER ROUTINE ON PROCEDURE sdp_ai.dispatch TO upgrade_app@localhost WITH GRANT OPTION;
      GRANT EXECUTE ON PROCEDURE sdp_ai.knowledge_document TO upgrade_app@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.action_dispatch TO upgrade_app@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.claim_run TO upgrade_worker@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.publish_tool_requests TO upgrade_worker@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.tool_call_status TO upgrade_worker@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.publish_tool_failure TO upgrade_worker@localhost;
      INSERT INTO sdp_ai.objects(object_id,kind,schema_name,name,owner,document) VALUES
        (REPEAT('a',32),'model','fixture','upgrade_model','root@localhost',JSON_OBJECT()),
        (REPEAT('b',32),'conversation','fixture','upgrade_conversation','root@localhost',JSON_OBJECT());
      INSERT INTO sdp_ai.object_versions(object_id,version,document,state) VALUES(REPEAT('a',32),1,JSON_OBJECT(),'active');
      INSERT INTO sdp_ai.conversations(conversation_id,model_id,model_version,messages,memory)
        VALUES(REPEAT('b',32),REPEAT('a',32),1,JSON_ARRAY(),JSON_OBJECT());
      INSERT INTO sdp_ai.runs(run_id,job_id,owner,conversation_id,conversation_revision,epoch,request_key,request_fingerprint,
        model,messages,instructions,options,state) VALUES(REPEAT('d',32),REPEAT('d',32),'root@localhost',REPEAT('b',32),0,0,
        REPEAT('d',32),REPEAT('0',64),JSON_OBJECT(),JSON_ARRAY(),'',JSON_OBJECT(),'failed');
      INSERT INTO sdp_ai.tool_rounds(run_id,round_id,round_number,publication_hash,assistant_text,provider_metadata)
        VALUES(REPEAT('d',32),REPEAT('e',32),1,REPEAT('0',64),'',JSON_OBJECT());
      INSERT INTO sdp_ai.tool_calls(call_id,run_id,round_id,ordinal,provider_call_id,tool_id,definition_hash,arguments,snapshot,state)
        VALUES(REPEAT('c',32),REPEAT('d',32),REPEAT('e',32),0,'upgrade-fixture',REPEAT('f',32),REPEAT('0',64),
          JSON_OBJECT('amount','12345678901234567890123456789.123456789'),JSON_OBJECT('kind','procedure'),'succeeded');
      -- Emulate the previous storage layout without deleting its legacy row.
      ALTER TABLE sdp_ai.tool_calls DROP COLUMN action_id,DROP COLUMN action_snapshot_hash;""")
    grants = {name: sorted(query(instance,"SHOW GRANTS FOR '"+name+"'@'localhost'").splitlines())
              for name in ('upgrade_app','upgrade_worker')}
    row = query(instance, "SELECT CONCAT(call_id,'|',run_id,'|',state,'|',CAST(arguments AS CHAR)) FROM sdp_ai.tool_calls")
    private = query(instance, """SELECT SHA2(CAST(JSON_OBJECT('plugin',plugin,'authentication',authentication_string,
      'locked',account_locked,'attributes',User_attributes) AS CHAR),256) FROM mysql.user
      WHERE User='sdp_ai_internal' AND Host='localhost'""")
    for _ in range(2):
        for name in ('catalog.sql','tool_catalog.sql','action_catalog.sql'):
            execute(instance, (catalog/name).read_text(encoding='utf-8'))
    require(query(instance, """SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='sdp_ai'
      AND TABLE_NAME='tool_calls' AND COLUMN_NAME IN ('action_id','action_snapshot_hash')""") == '2',
      'tool catalog reinstall did not add both nullable action-binding columns to an existing table')
    require(query(instance, "SELECT CONCAT(call_id,'|',run_id,'|',state,'|',CAST(arguments AS CHAR)) FROM sdp_ai.tool_calls") == row,
            'catalog upgrade changed an existing tool row or its exact Decimal argument')
    require(query(instance, 'SELECT COUNT(*) FROM sdp_ai.tool_calls WHERE action_id IS NULL AND action_snapshot_hash IS NULL') == '1',
            'upgrade invented an approval binding for an existing legacy tool call')
    for name, expected in grants.items():
        require(sorted(query(instance,"SHOW GRANTS FOR '"+name+"'@'localhost'").splitlines()) == expected,
                'managed routine reinstall changed existing '+name+' grants')
    require(query(instance, """SELECT SHA2(CAST(JSON_OBJECT('plugin',plugin,'authentication',authentication_string,
      'locked',account_locked,'attributes',User_attributes) AS CHAR),256) FROM mysql.user
      WHERE User='sdp_ai_internal' AND Host='localhost'""") == private,
      'catalog reinstall changed the existing private account')
    print('PASS SQL-only AI upgrade: additive nullable action columns, exact legacy rows, bounded app/worker routine grants and private account unchanged')
    print('NOT_RUN native13 role/tool/action execution: this test validates SQL installation only')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--catalog',type=Path,default=REPOSITORY/'compatibility/ai')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-upgrade-definitions') as root:
        cert=backend_certificate(MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe',root,'cert')
        with fixture.disposable(root/'server',tls_dir=cert,server_options=('--sdp-ai-sql=ON',)) as instance:
            exercise(instance,args.catalog.resolve(strict=True))


if __name__=='__main__':
    main()
