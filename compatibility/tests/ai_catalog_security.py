"""Managed AI definer-account collisions preserve existing account state."""
import argparse
import json
from pathlib import Path

from .ai_catalog_acceptance import execute
from .fixture import BASEDIR, BIN, MIGRATION, REPOSITORY, SHELL, Fixture, query, require, run
from .workbench_native import backend_certificate


def snapshot(instance):
    state=query(instance,"""SELECT CAST(JSON_OBJECT('locked',account_locked,'plugin',plugin,
      'authentication',authentication_string,'attributes',User_attributes) AS CHAR)
      FROM mysql.user WHERE User='sdp_ai_internal' AND Host='localhost'""")
    grants=query(instance,"SHOW GRANTS FOR 'sdp_ai_internal'@'localhost'")
    return state,tuple(sorted(grants.splitlines()))


def exercise(instance):
    source=(REPOSITORY/'compatibility/ai/catalog.sql').read_text(encoding='utf-8')
    execute(instance,'CREATE SCHEMA workflow;CREATE TABLE workflow.private_data(id INT,secret INT);CREATE ROLE ai_collision_role;')
    scenarios=[
      ("CREATE USER 'sdp_ai_internal'@'localhost' IDENTIFIED BY 'fixture-only';GRANT SELECT ON workflow.* TO 'sdp_ai_internal'@'localhost';",'unmanaged existing account'),
      ("CREATE USER 'sdp_ai_internal'@'localhost' ACCOUNT LOCK ATTRIBUTE '{\"sdp_ai_catalog\":\"v1\"}';GRANT FILE ON *.* TO 'sdp_ai_internal'@'localhost';",'managed account with global privilege'),
      ("CREATE USER 'sdp_ai_internal'@'localhost' ACCOUNT LOCK ATTRIBUTE '{\"sdp_ai_catalog\":\"v1\"}';GRANT ai_collision_role TO 'sdp_ai_internal'@'localhost';",'managed account with role'),
      ("CREATE USER 'sdp_ai_internal'@'localhost' ACCOUNT LOCK ATTRIBUTE '{\"sdp_ai_catalog\":\"v1\"}';GRANT SELECT(secret) ON workflow.private_data TO 'sdp_ai_internal'@'localhost';",'managed account with unrelated column grant'),
    ]
    for setup,label in scenarios:
        execute(instance,setup)
        before=snapshot(instance)
        result=run(instance[1],instance[0],60,script=source)
        require(result.returncode!=0 and 'AI_PRIVATE_DEFINER_ACCOUNT_COLLISION' in result.stderr.upper(),
                'installer did not reject '+label)
        after=snapshot(instance)
        changed=[key for key,value in json.loads(before[0]).items() if json.loads(after[0]).get(key)!=value]
        require(after==before,'installer changed '+repr(changed)+(' and grants' if before[1]!=after[1] else '')+' of '+label)
        require(query(instance,"SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA='sdp_ai'")=='0',
                'collision installed routines under an untrusted definer')
        execute(instance,"DROP USER 'sdp_ai_internal'@'localhost';")
    print('PASS AI installer account collision: existing authentication, lock, metadata and grants preserved')
    print('PASS AI installer rejects missing marker, unexpected global privilege, role and unrelated column grants')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--openssl',type=Path,default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-catalog-security') as root:
        certificates=backend_certificate(args.openssl.resolve(strict=True),root,'cert')
        with fixture.disposable(root/'server',tls_dir=certificates) as instance:
            exercise(instance)


if __name__=='__main__':
    main()
