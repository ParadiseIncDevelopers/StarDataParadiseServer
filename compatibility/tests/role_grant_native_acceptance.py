"""Exercise native TO ROLE grants and unchanged native account recipients.

Requires the integrated recipient parser/ACL mutation build. No AI/provider or
SQL source rewriting is used; all schemas/accounts belong to disposable TLS data.
"""
import argparse
from pathlib import Path

from .fixture import BASEDIR, BIN, SHELL, Fixture, query, require, run
from .sql_short_native_acceptance import execute


def exercise(instance):
    restart=instance.restart
    # stdin and --execute both carry UTF-8 SQL. Do not let the Windows client's
    # inferred console charset turn a non-ASCII role into another authID.
    instance=(instance[0],[*instance[1],'--default-character-set=utf8mb4'],instance[2])
    execute(instance, """CREATE SCHEMA role_fixture;
      CREATE TABLE role_fixture.values_table(id INT PRIMARY KEY);
      INSERT INTO role_fixture.values_table VALUES(17);
      CREATE FUNCTION role_fixture.answer() RETURNS INT DETERMINISTIC RETURN 42;
      CREATE PROCEDURE role_fixture.read_values() SELECT id FROM role_fixture.values_table;
      CREATE ROLE role_one,role_two@localhost,role_three,`Uygulama Yöneticisi`;
      CREATE USER role_account,role,role_reader@localhost REQUIRE SSL;
      GRANT EXECUTE ON FUNCTION role_fixture.answer TO ROLE role_one,role_two@localhost;
      GRANT EXECUTE ON PROCEDURE role_fixture.read_values TO ROLE role_one;
      GRANT SELECT ON role_fixture.values_table TO ROLE role_one;
      GRANT SELECT ON role_fixture.* TO ROLE role_two@localhost;
      GRANT EXECUTE ON FUNCTION role_fixture.answer TO ROLE `Uygulama Yöneticisi`;
      GRANT role_one TO role_reader@localhost;
      SET DEFAULT ROLE ALL TO role_reader@localhost;""")
    count = query(instance, "SELECT COUNT(*) FROM mysql.procs_priv WHERE Db='role_fixture' "
                  "AND User IN ('role_one','role_two','Uygulama Yöneticisi')")
    if count != '4':
        rows=query(instance,"SELECT GROUP_CONCAT(CONCAT(User,'@',Host,':',Routine_name,':',Routine_type,':',HEX(User)) ORDER BY User,Routine_name SEPARATOR '|') FROM mysql.procs_priv WHERE Db='role_fixture'")
        require(False, 'explicit function/procedure role grants did not use native ACL storage: '+count+'; '+rows)
    env, command, _ = instance
    reader = [part if not part.startswith('--user=') else '--user=role_reader'
              for part in command]
    result = run(reader, env, 60, script='SELECT role_fixture.answer();CALL role_fixture.read_values();')
    require(result.returncode == 0 and result.stdout.strip() == '42\n17',
            'native active-role function/procedure/table authorization failed: '+result.stdout+result.stderr)
    execute(instance, 'REVOKE EXECUTE ON FUNCTION role_fixture.answer FROM ROLE role_one')
    result=run(reader,env,60,script='SELECT role_fixture.answer();')
    require(result.returncode != 0 and 'denied' in result.stderr.lower(),
            'revoked active-role function privilege remained usable')
    execute(instance, 'GRANT EXECUTE ON FUNCTION role_fixture.answer TO ROLE role_one')

    # Roles and accounts share native MySQL authIDs. A fresh, unassigned CREATE
    # ROLE must work; so can an ordinary authID that may later be assigned as a
    # role. Every recipient must exist before the first privilege write.
    execute(instance, 'GRANT EXECUTE ON FUNCTION role_fixture.answer TO ROLE role_account')
    execute(instance, 'GRANT EXECUTE ON FUNCTION role_fixture.answer TO ROLE role_three,missing_role', 3523)
    execute(instance, 'GRANT EXECUTE ON FUNCTION role_fixture.answer TO ROLE missing_role', 3523)
    require(query(instance, "SELECT COUNT(*) FROM mysql.procs_priv WHERE Db='role_fixture' AND User='role_three'") == '0',
            'invalid mixed TO ROLE list partially granted a previous recipient')
    execute(instance, 'GRANT SELECT ON role_fixture.values_table TO ROLE role_three,missing_role', 3523)
    execute(instance, 'GRANT SELECT ON role_fixture.* TO ROLE role_three,missing_role', 3523)
    require(query(instance, "SELECT COUNT(*) FROM mysql.tables_priv WHERE Db='role_fixture' AND User='role_three'") == '0',
            'invalid table recipient list partially changed privileges')
    require(query(instance, "SELECT COUNT(*) FROM mysql.db WHERE Db='role_fixture' AND User='role_three'") == '0',
            'invalid schema recipient list partially changed privileges')

    # ROLE remains an ordinary unquoted account identifier where it was one;
    # the explicit marker resets between statements, including failed grants.
    execute(instance, """GRANT EXECUTE ON FUNCTION role_fixture.answer TO role;
      GRANT EXECUTE ON FUNCTION role_fixture.answer TO role_account;
      GRANT ALL ON FUNCTION role_fixture.answer TO ROLE role_three;""")
    require(query(instance, "SELECT COUNT(*) FROM mysql.procs_priv WHERE Db='role_fixture' "
                  "AND User IN ('role','role_account','role_three')") == '3',
            'TO ROLE grammar or marker changed native legacy account/ALL grants')

    # Native DROP clears old privileges. Re-executing a GRANT explicitly applies
    # a new privilege to the current live authID, with no stale existence cache.
    result=execute(instance, """PREPARE role_grant FROM 'GRANT EXECUTE ON FUNCTION role_fixture.answer TO ROLE role_three';
      EXECUTE role_grant;DROP ROLE role_three;CREATE USER role_three;
      SELECT COUNT(*) FROM mysql.procs_priv WHERE Db='role_fixture' AND User='role_three';
      EXECUTE role_grant;
      SELECT COUNT(*) FROM mysql.procs_priv WHERE Db='role_fixture' AND User='role_three';""")
    require(result=='0\n1','prepared native GRANT reused deleted privileges or a stale recipient: '+result)
    print('PASS native explicit role recipients, active-role execution, atomic existence checks and legacy account identifiers', flush=True)
    unicode_persistence(instance,restart)


def unicode_persistence(instance,restart):
    schema, function, procedure = 'yetki_şema', 'yanıt_ö', 'okuma_ı'
    role, reader, direct = 'rol_ğ', 'okuyucu_ö', 'doğrudan_ö'
    def quoted(value): return '`'+value.replace('`','``')+'`'
    def target(name): return quoted(schema)+'.'+quoted(name)
    execute(instance,'''CREATE SCHEMA %s;
      CREATE FUNCTION %s() RETURNS INT DETERMINISTIC RETURN 73;
      CREATE PROCEDURE %s() SELECT 91;
      CREATE ROLE %s@LOCALHOST;
      CREATE USER %s@localhost,%s@localhost REQUIRE SSL;
      GRANT EXECUTE ON FUNCTION %s TO ROLE %s@LOCALHOST;
      GRANT EXECUTE ON PROCEDURE %s TO ROLE %s@LOCALHOST;
      GRANT EXECUTE ON FUNCTION %s TO %s@LOCALHOST;
      GRANT %s@localhost TO %s@localhost;
      SET DEFAULT ROLE ALL TO %s@localhost;''' % (
        quoted(schema),target(function),target(procedure),quoted(role),quoted(reader),quoted(direct),
        target(function),quoted(role),target(procedure),quoted(role),target(function),quoted(direct),
        quoted(role),quoted(reader),quoted(reader)))
    hx=lambda value:value.encode('utf-8').hex().upper()
    storage="SELECT HEX(Host),HEX(Db),HEX(User),HEX(Routine_name),Routine_type FROM mysql.procs_priv WHERE Db=CONVERT(X'%s' USING utf8mb4)" % schema.encode().hex()
    expected={
        '\t'.join([hx('localhost'),hx(schema),hx(role),hx(function),'FUNCTION']),
        '\t'.join([hx('localhost'),hx(schema),hx(role),hx(procedure),'PROCEDURE']),
        '\t'.join([hx('localhost'),hx(schema),hx(direct),hx(function),'FUNCTION'])}
    def assert_storage():
        observed=set(query(instance,storage).splitlines())
        require(observed==expected,'Native routine grant persisted a different UTF8 identity: '+repr(observed))
    def connected(user,source):
        env,command,_=instance
        command=[part if not part.startswith('--user=') else '--user='+user for part in command]
        return run(command,env,60,script=source)
    def assert_execution():
        value=connected(reader,'SELECT %s();CALL %s();' % (target(function),target(procedure)))
        require(value.returncode==0 and value.stdout.strip()=='73\n91',
                'Unicode active-role routine authorization failed: '+value.stdout+value.stderr)
        value=connected(direct,'SELECT %s();' % target(function))
        require(value.returncode==0 and value.stdout.strip()=='73','Ordinary Unicode account grant regressed: '+value.stdout+value.stderr)
    assert_storage();assert_execution()
    restart(server_options=())
    assert_storage();assert_execution()
    execute(instance,'REVOKE EXECUTE ON FUNCTION %s FROM ROLE %s@LOCALHOST;' % (target(function),quoted(role)))
    expected.remove('\t'.join([hx('localhost'),hx(schema),hx(role),hx(function),'FUNCTION']))
    assert_storage()
    denied=connected(reader,'SELECT %s();' % target(function))
    require(denied.returncode!=0 and 'denied' in denied.stderr.lower(),'Persisted Unicode role grant could not be revoked')
    require(connected(direct,'SELECT %s();' % target(function)).stdout.strip()=='73','Role revoke altered another ordinary account grant')
    print('PASS UTF8 schema/function/procedure/authID physical grant rows, native host case, clean restart role/direct ACL and targeted revoke',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('native-role-grants') as root:
        with fixture.disposable(root/'server',allow_restart=True) as instance:
            exercise(instance)


if __name__=='__main__':
    main()
