"""Real native role/action approval gates on a disposable TLS Server.

No provider or protected-row fixture stands in for execution. All actions are
prepared/approved/executed by the public native SQL grammar. The shared Fixture
owns maintenance bootstrap/restarts; this module never opens a user datadir.
"""
from contextlib import contextmanager
import argparse
import hashlib
import json
from pathlib import Path
import sys
import threading
import time

from .ai_catalog_acceptance import call,execute,literal,install
from .fixture import BASEDIR,BIN,MIGRATION,REPOSITORY,SHELL,Fixture,query,require
from .workbench_native import backend_certificate


@contextmanager
def connection(instance,user='root',isolation=None):
    import mysql.connector
    options={part[2:].split('=',1)[0]:part.split('=',1)[1] for part in instance[1] if part.startswith('--') and '=' in part}
    value=mysql.connector.connect(host='127.0.0.1',port=int(options['port']),user=user,password='',
        ssl_ca=options['ssl-ca'],ssl_verify_cert=True,ssl_verify_identity=True,autocommit=True,use_pure=True)
    try:
        if isolation:
            cursor=value.cursor();cursor.execute('SET SESSION TRANSACTION ISOLATION LEVEL '+isolation);cursor.close()
        yield value
    finally:value.close()


def run(connection,source,parameters=None,prepared=False):
    cursor=connection.cursor(prepared=prepared)
    try:
        cursor.execute(source,parameters)
        result=cursor.fetchall() if cursor.with_rows else []
        while cursor.nextset():
            if cursor.with_rows:cursor.fetchall()
        return result
    finally:cursor.close()


def document(connection,source,parameters=None,prepared=False):
    rows=run(connection,source,parameters,prepared)
    require(len(rows)==1 and len(rows[0])==1,'Action did not return exactly one document')
    value=rows[0][0]
    return json.loads(value) if isinstance(value,(str,bytes,bytearray)) else value


def model_visible(connection):
    # SHOW returns the public catalog rowset (id,name,schema,...), not an
    # Action JSON document. It lists authorized objects across schemas.
    return any(len(row)>=3 and row[1]=='action_model' and row[2]=='fixture'
        for row in run(connection,'SHOW MODELS'))


def expect(connection,source,codes,parameters=None,prepared=False):
    import mysql.connector
    try:run(connection,source,parameters,prepared)
    except mysql.connector.Error as error:
        require(error.errno in set(codes),'Unexpected native error '+str(error))
        return error
    raise RuntimeError('Native action unexpectedly accepted '+source)


def setup(instance):
    execute(instance,"""CREATE USER act_owner@localhost IDENTIFIED BY '' REQUIRE SSL;
      CREATE USER act_approver@localhost IDENTIFIED BY '' REQUIRE SSL;
      CREATE USER act_stranger@localhost IDENTIFIED BY '' REQUIRE SSL;
      CREATE USER act_rename_user@localhost IDENTIFIED BY '' REQUIRE SSL;
      CREATE ROLE act_outer,act_inner,act_inactive,act_approve,act_rename_from;
      GRANT act_inner TO act_outer;
      GRANT act_outer,act_inactive TO act_owner@localhost;
      GRANT act_approve TO act_approver@localhost;
      CREATE TABLE fixture.a_action_orders(id INT PRIMARY KEY,status VARCHAR(24) NOT NULL,effects INT NOT NULL) ENGINE=InnoDB;
      CREATE TABLE fixture.z_action_gate(id INT PRIMARY KEY,value INT NOT NULL) ENGINE=InnoDB;
      INSERT INTO fixture.a_action_orders VALUES(1,'open',0),(2,'open',0);
      INSERT INTO fixture.z_action_gate VALUES(1,0);
      DELIMITER $$
      CREATE PROCEDURE fixture.action_cancel(IN order_id INT) SQL SECURITY INVOKER
      BEGIN
        UPDATE fixture.a_action_orders SET status='cancelled',effects=effects+1 WHERE id=order_id;
        SELECT id,status,effects FROM fixture.a_action_orders WHERE id=order_id;
      END$$
      CREATE PROCEDURE fixture.action_race(IN order_id INT) SQL SECURITY INVOKER
      BEGIN
        UPDATE fixture.a_action_orders SET effects=effects+1 WHERE id=order_id;
        UPDATE fixture.z_action_gate SET value=value+1 WHERE id=1;
      END$$
      CREATE PROCEDURE fixture.action_fail(IN order_id INT) SQL SECURITY INVOKER
      BEGIN
        UPDATE fixture.a_action_orders SET effects=effects+1 WHERE id=order_id;
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='fixture action rollback';
      END$$
      CREATE FUNCTION fixture.action_bad_id() RETURNS CHAR(32) SQL SECURITY DEFINER MODIFIES SQL DATA
      BEGIN INSERT INTO fixture.ledger VALUES(861);RETURN REPEAT('a',32);END$$
      CREATE PROCEDURE fixture.action_late(IN order_id INT) SQL SECURITY INVOKER
      BEGIN INSERT INTO fixture.ledger VALUES(863);END$$
      CREATE PROCEDURE fixture.action_prepare_local(IN requested INT) SQL SECURITY INVOKER
      BEGIN
        DECLARE prepared CHAR(32) DEFAULT 'old';
        PREPARE ACTION CALL fixture.action_cancel(:requested) RETURNING ID INTO :prepared;
        SET @local_prepared_id=prepared;
      END$$
      CREATE PROCEDURE fixture.action_prepare_small() SQL SECURITY INVOKER
      BEGIN
        DECLARE prepared VARCHAR(8) DEFAULT 'old';DECLARE caught BOOLEAN DEFAULT FALSE;
        DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET caught=TRUE;
        PREPARE ACTION CALL fixture.action_cancel(2) RETURNING ID INTO :prepared;
        SELECT prepared,caught;
      END$$
      CREATE PROCEDURE fixture.action_prepare_pending() SQL SECURITY INVOKER
      BEGIN
        DECLARE prepared CHAR(32) DEFAULT 'old';DECLARE caught BOOLEAN DEFAULT FALSE;
        DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET caught=TRUE;
        START TRANSACTION;INSERT INTO fixture.ledger VALUES(862);
        PREPARE ACTION CALL fixture.action_cancel(2) RETURNING ID INTO :prepared;
        SELECT prepared,caught,(SELECT COUNT(*) FROM fixture.ledger WHERE id=862);
        ROLLBACK;
      END$$
      CREATE PROCEDURE fixture.action_private(IN requested INT) SQL SECURITY INVOKER
      BEGIN INSERT INTO fixture.ledger VALUES(requested);END$$
      CREATE PROCEDURE fixture.action_definer_prepare(IN requested INT) SQL SECURITY DEFINER
      BEGIN PREPARE ACTION CALL fixture.action_private(:requested) RETURNING ID INTO @forbidden_action;END$$
      DELIMITER ;
      GRANT SELECT,UPDATE ON fixture.a_action_orders TO act_owner@localhost;
      GRANT SELECT,UPDATE ON fixture.z_action_gate TO act_owner@localhost;
      GRANT SELECT,INSERT ON fixture.ledger TO act_owner@localhost;
      GRANT SELECT,UPDATE ON fixture.a_action_orders TO act_rename_user@localhost;
      GRANT EXECUTE ON PROCEDURE fixture.action_cancel TO act_rename_user@localhost;
      GRANT EXECUTE ON PROCEDURE fixture.action_cancel TO ROLE act_inner;
      GRANT EXECUTE ON PROCEDURE fixture.action_race TO ROLE act_inner;
      GRANT EXECUTE ON PROCEDURE fixture.action_fail TO ROLE act_inner;
      GRANT EXECUTE ON PROCEDURE fixture.action_cancel TO ROLE act_rename_from;
      GRANT EXECUTE ON FUNCTION fixture.action_bad_id TO act_owner@localhost;
      GRANT EXECUTE ON PROCEDURE fixture.action_late TO ROLE act_inner;
      GRANT EXECUTE ON PROCEDURE fixture.action_prepare_local TO ROLE act_inner;
      GRANT EXECUTE ON PROCEDURE fixture.action_prepare_small TO ROLE act_inner;
      GRANT EXECUTE ON PROCEDURE fixture.action_prepare_pending TO ROLE act_inner;
      GRANT EXECUTE ON PROCEDURE fixture.action_definer_prepare TO ROLE act_inner;
      REQUIRE APPROVAL ON PROCEDURE fixture.action_cancel;
      REQUIRE APPROVAL ON PROCEDURE fixture.action_race;
      REQUIRE APPROVAL ON PROCEDURE fixture.action_fail;
      GRANT APPROVE ON PROCEDURE fixture.action_cancel TO ROLE act_approve;
      GRANT APPROVE ON PROCEDURE fixture.action_race TO ROLE act_approve;
      GRANT APPROVE ON PROCEDURE fixture.action_fail TO ROLE act_approve;
      GRANT EXECUTE ON PROCEDURE sdp_ai.dispatch TO act_owner@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.object_document TO act_owner@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.dispatch TO act_stranger@localhost;
      GRANT EXECUTE ON PROCEDURE sdp_ai.object_document TO act_stranger@localhost;
    """)
    execute(instance,call('create','model','action_model',dict(provider='OPENAI',remote_model='fixture',settings=dict(secret_env='FIXTURE_KEY'))))
    execute(instance,'GRANT USE ON MODEL fixture.action_model TO ROLE act_inner;GRANT USE ON MODEL fixture.action_model TO ROLE act_rename_from;GRANT USE ON MODEL fixture.action_model TO ROLE act_stranger@localhost;GRANT USE ON MODEL fixture.action_model TO ROLE act_rename_user@localhost;')


def prepare(owner,name='action_cancel',value=1):
    result=document(owner,'PREPARE ACTION CALL fixture.'+name+'('+str(value)+') RETURNING ID INTO @action_id')
    require(result['state']=='prepared' and result['resource_scope']=='all_dependency_tables','prepare lost conservative resource scope')
    require(run(owner,'SELECT @action_id')==[(result['action_id'],)],'RETURNING ID did not update the main physical SQL session')
    return result['action_id']


def approve(approver,action):
    result=document(approver,'APPROVE ACTION ?',parameters=(action,),prepared=True)
    require(result['action_id']==action and result['state']=='approved','approval did not bind the prepared identity')


def exercise(instance):
    setup(instance)
    native_dcl_modes(instance)
    execute(instance,'GRANT USE ON MODEL FiXtUrE.action_model TO ROLE act_inner;',61081)
    with connection(instance,'act_owner') as owner,connection(instance,'act_approver') as approver,connection(instance,'act_stranger') as stranger:
        run(owner,'SET ROLE act_outer');run(approver,'SET ROLE act_approve')
        principals=document(owner,'SELECT STARDATAPARADISE_PRINCIPALS()')
        require(principals['actor']=='act_owner@localhost','principal actor was replaced by a catalog definer')
        require('role@act_outer@%' in principals['principals'] and 'role@act_inner@%' in principals['principals'] and 'role@act_inactive@%' not in principals['principals'],'inactive or nested role authorization was incorrect')
        require(model_visible(owner),'active inherited role could not use its model grant')
        require(not model_visible(stranger),'role-only AI grant became a direct account grant')
        run(owner,'SET ROLE NONE')
        require('role@act_inner@%' not in document(owner,'SELECT STARDATAPARADISE_PRINCIPALS()')['principals'],'SET ROLE NONE retained an inherited principal')
        expect(owner,'PREPARE ACTION CALL fixture.action_cancel(1)',(1370,61081))
        run(owner,'SET ROLE act_outer')
        expect(owner,'CALL fixture.action_cancel(1)',(61083,))
        expect(owner,'CALL fixture.action_cancel(fixture.action_bad_id())',(61083,))
        expect(owner,'CALL fixture.ACTION_CANCEL(2)',(61083,))
        require(query(instance,'SELECT SUM(effects) FROM fixture.a_action_orders')=='0','ordinary CALL bypassed the approval policy')
        require(query(instance,'SELECT COUNT(*) FROM fixture.ledger WHERE id=861')=='0','ordinary CALL evaluated a writing function before approval enforcement')
        expect(owner,'CALL sdp_ai.action_dispatch(\'prepare\',JSON_OBJECT())',(1370,61081))
        print('PASS native active/nested/inactive role authorization, ordinary CALL case/pre-evaluation approval gate and raw private-RPC denial',flush=True)
        action=prepare(owner)
        expect(stranger,'DESCRIBE ACTION ?', (61081,), (action,),True)
        expect(owner,'EXECUTE ACTION ?', (61083,), (action,),True)
        expect(owner,'APPROVE ACTION ?', (61081,), (action,),True)
        approve(approver,action)
        preview=document(owner,'DESCRIBE ACTION ?', (action,),True)
        require(preview['target']==dict(schema_name='fixture',name='action_cancel') and 'resources' not in preview,'public action document leaked raw resource snapshots')
        result=document(owner,'EXECUTE ACTION ?', (action,),True)
        require(result['state']=='succeeded','approved action did not execute')
        replay=document(owner,'EXECUTE ACTION ?', (action,),True)
        require(replay==result and query(instance,'SELECT effects FROM fixture.a_action_orders WHERE id=1')=='1','action receipt replay duplicated a write')

        denied=prepare(owner,value=2)
        require(document(approver,'DENY ACTION ?', (denied,),True)['state']=='denied','deny was not durable')
        expect(owner,'EXECUTE ACTION ?', (61083,61084), (denied,),True)

        stale=prepare(owner,value=2);approve(approver,stale)
        execute(instance,"UPDATE fixture.a_action_orders SET status='reviewed' WHERE id=2;")
        expect(owner,'EXECUTE ACTION ?', (61084,), (stale,),True)
        require(query(instance,'SELECT effects FROM fixture.a_action_orders WHERE id=2')=='0','stale approval wrote changed resource state')

        expired=prepare(owner,value=2);approve(approver,expired)
        execute(instance,'UPDATE sdp_ai.actions SET expires_at=TIMESTAMPADD(SECOND,-1,UTC_TIMESTAMP(6)) WHERE action_id='+literal(expired))
        expect(owner,'EXECUTE ACTION ?', (61084,), (expired,),True)

        revoked=prepare(owner,value=2);approve(approver,revoked)
        execute(instance,'REVOKE APPROVE ON PROCEDURE fixture.action_cancel FROM ROLE act_approve;')
        expect(owner,'EXECUTE ACTION ?', (61081,61084), (revoked,),True)
        execute(instance,'GRANT APPROVE ON PROCEDURE fixture.action_cancel TO ROLE act_approve;')

        failed=prepare(owner,name='action_fail',value=2);approve(approver,failed)
        expect(owner,'EXECUTE ACTION ?', (1644,61085), (failed,),True)
        require(query(instance,'SELECT effects FROM fixture.a_action_orders WHERE id=2')=='0','application SIGNAL committed partial action writes')
        require(query(instance,'SELECT state FROM sdp_ai.actions WHERE action_id='+literal(failed))=='approved','failed action published a succeeded receipt')
        expect(owner,'EXECUTE ACTION fixture.action_bad_id()', (1064,61081,61006))
        expect(owner,'EXECUTE ACTION (fixture.action_bad_id())', (61081,61006))
        require(query(instance,'SELECT COUNT(*) FROM fixture.ledger WHERE id=861')=='0','action ID evaluated a writing routine before AST rejection')

        execute(instance,'REVOKE EXECUTE ON PROCEDURE fixture.action_cancel FROM ROLE act_inner;')
        current=prepare(owner,name='action_fail',value=2);approve(approver,current)
        execute(instance,'REVOKE UPDATE ON fixture.a_action_orders FROM act_owner@localhost;')
        expect(owner,'EXECUTE ACTION ?', (1142,1143,1370,61081), (current,),True)
        require(query(instance,'SELECT effects FROM fixture.a_action_orders WHERE id=2')=='0','current owner ACL revoke did not roll back the action')
        execute(instance,'GRANT UPDATE ON fixture.a_action_orders TO act_owner@localhost;GRANT EXECUTE ON PROCEDURE fixture.action_cancel TO ROLE act_inner;')
        count=query(instance,'SELECT COUNT(*) FROM sdp_ai.actions')
        run(owner,'CALL fixture.action_prepare_local(2)')
        local_id=run(owner,'SELECT @local_prepared_id')[0][0]
        require(len(local_id)==32 and document(owner,'DESCRIBE ACTION ?', (local_id,),True)['state']=='prepared','clean SP local PREPARE/RETURNING failed')
        require(run(owner,'CALL fixture.action_prepare_small()')==[('old',1)],'small local RETURNING target changed or did not reject')
        require(query(instance,'SELECT COUNT(*) FROM sdp_ai.actions')==str(int(count)+1),'invalid local target created a durable action')
        pending_rows=run(owner,'CALL fixture.action_prepare_pending()')
        require(pending_rows==[('old',1,1)],'pending business TX was not rejected without changing its local: '+repr(pending_rows))
        require(query(instance,'SELECT COUNT(*) FROM fixture.ledger WHERE id=862')=='0','PREPARE implicitly committed caller business DML')
        expect(owner,'CALL fixture.action_definer_prepare(864)',(1370,61081))
        require(query(instance,'SELECT COUNT(*) FROM fixture.ledger WHERE id=864')=='0','PREPARE borrowed its caller DEFINER authority')
        execute(instance,'GRANT act_rename_from TO act_owner@localhost;')
        run(owner,'SET ROLE act_rename_from')
        renamed=prepare(owner,value=2);approve(approver,renamed)
        with connection(instance,'act_rename_user') as rename_user:
            renamed_user=prepare(rename_user,value=2);approve(approver,renamed_user)
    print('PASS native role graph: active roots/nested inheritance, inactive role rejection and escaped actor separation',flush=True)
    print('PASS native actions: manual prepared grammar, ordinary CALL policy, explicit authorization, deny/expiry/stale/resource binding and idempotent receipt',flush=True)
    print('PASS native action atomicity: SIGNAL and current owner/approver ACL revocation leave application writes/receipt rolled back',flush=True)
    print('PASS clean SP PREPARE local input/RETURNING, capacity rejection before journal write, pending business TX preservation and invoker ACL',flush=True)
    late_policy(instance)
    race(instance)
    typed_bindings(instance)
    lifecycle_gates(instance)


def native_dcl_modes(instance):
    execute(instance,'CREATE ROLE act_dcl;')
    with connection(instance) as admin:
        run(admin,'SET autocommit=0')
        try:
            run(admin,'INSERT INTO fixture.ledger VALUES(867)')
            run(admin,'GRANT USE ON MODEL fixture.action_model TO ROLE act_dcl')
            require(run(admin,'SELECT @@autocommit')==[(0,)],'custom native GRANT changed the caller autocommit setting')
            require(query(instance,'SELECT COUNT(*) FROM fixture.ledger WHERE id=867')=='1','native GRANT lost its ordinary DCL implicit-commit boundary')
            require(query(instance,"SELECT COUNT(*) FROM sdp_ai.object_grants WHERE principal='role@act_dcl@%'")=='1','autocommit-OFF custom native GRANT was not committed')
            run(admin,'REVOKE USE ON MODEL fixture.action_model FROM ROLE act_dcl')
            require(run(admin,'SELECT @@autocommit')==[(0,)],'custom native REVOKE changed the caller autocommit setting')
            require(query(instance,"SELECT COUNT(*) FROM sdp_ai.object_grants WHERE principal='role@act_dcl@%'")=='0','autocommit-OFF custom native REVOKE was not committed')
        finally:run(admin,'ROLLBACK');run(admin,'SET autocommit=1')
    print('PASS native custom GRANT/REVOKE under autocommit OFF preserves native DCL commit semantics and restores caller mode',flush=True)


def typed_bindings(instance):
    execute(instance,"""CREATE TABLE fixture.action_bindings(id INT PRIMARY KEY,j JSON,b BLOB,n DECIMAL(6,2),u BIGINT UNSIGNED) ENGINE=InnoDB;
      DELIMITER $$
      CREATE PROCEDURE fixture.action_binding(IN k INT,IN document JSON,IN bytes_value BLOB,IN amount DECIMAL(6,2),IN unsigned_value BIGINT UNSIGNED) SQL SECURITY INVOKER
      BEGIN INSERT INTO fixture.action_bindings VALUES(k,document,bytes_value,amount,unsigned_value);END$$
      DELIMITER ;
      GRANT SELECT,INSERT,UPDATE ON fixture.action_bindings TO act_owner@localhost;
      GRANT EXECUTE ON PROCEDURE fixture.action_binding TO ROLE act_inner;
      REQUIRE APPROVAL ON PROCEDURE fixture.action_binding;
      GRANT APPROVE ON PROCEDURE fixture.action_binding TO ROLE act_approve;""")
    with connection(instance,'act_owner') as owner,connection(instance,'act_approver') as approver:
        run(owner,'SET ROLE act_outer');run(approver,'SET ROLE act_approve')
        locking=document(owner,"PREPARE ACTION CALL fixture.action_binding(4,NULL,X'',1.00,7)")['action_id']
        approve(approver,locking)
        execute(instance,'REVOKE UPDATE ON fixture.action_bindings FROM act_owner@localhost;')
        expect(owner,'EXECUTE ACTION ?', (1142,), (locking,),True)
        require(query(instance,'SELECT COUNT(*) FROM fixture.action_bindings')=='0','missing native locking privilege inserted an application row')
        require(query(instance,'SELECT state FROM sdp_ai.actions WHERE action_id='+literal(locking))=='approved','missing native locking privilege published a successful receipt')
        execute(instance,'GRANT UPDATE ON fixture.action_bindings TO act_owner@localhost;')
        cursor=owner.cursor(prepared=True)
        try:
            for key,text,amount in ((1,'null','12.25'),(2,'{"x":9007199254740993}','-3.50')):
                cursor.execute("PREPARE ACTION CALL fixture.action_binding(?,CAST(? AS JSON),X'00feff',CAST(? AS DECIMAL(6,2)),CAST('18446744073709551615' AS UNSIGNED))",(key,text,amount))
                rows=cursor.fetchall()
                while cursor.nextset():
                    if cursor.with_rows:cursor.fetchall()
                action=json.loads(rows[0][0])['action_id']
                approve(approver,action)
                require(document(owner,'EXECUTE ACTION ?', (action,),True)['state']=='succeeded','typed manual action did not execute')
        finally:cursor.close()
        action=document(owner,"PREPARE ACTION CALL fixture.action_binding(3,NULL,X'',1.00,7)")['action_id']
        approve(approver,action);document(owner,'EXECUTE ACTION ?', (action,),True)
        before=query(instance,'SELECT COUNT(*) FROM sdp_ai.actions')
        expect(owner,"PREPARE ACTION CALL fixture.action_binding(4,NULL,X'',1.234,7)",(1264,1265,61042))
        require(query(instance,'SELECT COUNT(*) FROM sdp_ai.actions')==before,'lossy typed parameter conversion created an approval record')
    expected="1\t0\tNULL\t00FEFF\t12.25\t18446744073709551615\n2\t0\tOBJECT\t00FEFF\t-3.50\t18446744073709551615\n3\t1\tNULL\t\t1.00\t7"
    actual=query(instance,'SELECT id,j IS NULL,JSON_TYPE(j),HEX(b),n,u FROM fixture.action_bindings ORDER BY id')
    print('ACTUAL_MANUAL_TYPED_ROWS '+repr(actual),flush=True)
    if actual!=expected:
        print('ACTUAL_MANUAL_TYPED_BINDINGS '+query(instance,"SELECT CAST(arguments AS CHAR),CAST(bindings AS CHAR) FROM sdp_ai.actions WHERE routine_name='action_binding' ORDER BY created_at"),flush=True)
    require(actual==expected,'manual JSON-null/SQLNULL/binary/decimal/U64 provenance or prepared rebind was lost: '+repr(actual))
    require(query(instance,"SELECT JSON_UNQUOTE(JSON_EXTRACT(j,'$.x')) FROM fixture.action_bindings WHERE id=2")=='9007199254740993','manual JSON integer precision was lost')
    print('PASS native manual action typed bindings: locking ACL revoke/noinsert/receipt then restored UPDATE; JSON-null versus SQLNULL, binary bytes/empty BLOB, exact Decimal/U64 and prepared rebinding',flush=True)


def lifecycle_gates(instance):
    """Exercise the native authID lifecycle, not a synthetic role registry."""
    multibyte='ğ'*32
    role_name="'"+multibyte+"'"  # Fixed fixture authID, not an SQL expression.
    execute(instance,"CREATE ROLE act_case@'localhost',act_self,act_replaced,act_drift,"+role_name+";"
        "GRANT USE ON MODEL fixture.action_model TO ROLE act_case@'LOCALHOST',act_self,act_replaced,act_drift,"+role_name+";"
        "GRANT act_case@'localhost',act_replaced,"+role_name+" TO act_owner@localhost;GRANT act_self TO root@localhost;")
    with connection(instance,'act_owner') as owner:
        run(owner,'SET ROLE '+role_name)
        require('role@'+multibyte+'@%' in document(owner,'SELECT STARDATAPARADISE_PRINCIPALS()')['principals'],'32-character UTF8 role was rejected by a byte-only authID limit')
        run(owner,"SET ROLE act_case@'LOCALHOST'")
        require('role@act_case@localhost' in document(owner,'SELECT STARDATAPARADISE_PRINCIPALS()')['principals'],'host-case variant was not resolved to the stored native authID')
        require(model_visible(owner),'canonical host-case role lost its custom grant')
        run(owner,'SET ROLE NONE')
    execute(instance,"DROP ROLE act_case@'LOCALHOST';DROP ROLE act_replaced;CREATE USER act_replaced;GRANT act_replaced TO act_owner@localhost;")
    require(query(instance,"SELECT COUNT(*) FROM sdp_ai.object_grants WHERE principal='role@act_case@localhost'")=='0','host-case DROP missed its canonical role grant')
    with connection(instance,'act_owner') as owner:
        run(owner,'SET ROLE act_replaced')
        require(not model_visible(owner),'same-authID CREATE USER revived a dropped role grant')
    with connection(instance) as admin:
        run(admin,'SET ROLE act_self')
        run(admin,'DROP ROLE act_self')
    require(query(instance,"SELECT COUNT(*) FROM sdp_ai.object_grants WHERE principal='role@act_self@%'")=='0','dropping the current active role resnapshotted an invalid role proof')
    execute(instance,'ALTER PROCEDURE sdp_ai.action_dispatch SQL SECURITY INVOKER;')
    try:
        with connection(instance) as admin:expect(admin,'DROP ROLE act_drift',(61081,))
        require(query(instance,"SELECT COUNT(*) FROM mysql.user WHERE User='act_drift' AND Host='%'")=='1','helper metadata drift partially dropped the native authID')
        require(query(instance,"SELECT COUNT(*) FROM sdp_ai.object_grants WHERE principal='role@act_drift@%'")=='1','helper metadata drift partially removed its custom grants')
    finally:execute(instance,'ALTER PROCEDURE sdp_ai.action_dispatch SQL SECURITY DEFINER;')
    execute(instance,'DROP ROLE act_drift;')
    print('PASS native lifecycle: UTF8 authID, canonical host case, active-role DROP, same-name USER replacement and tampered-helper atomic rejection',flush=True)


def late_policy(instance):
    # Binary PREPARE must not pin data/MDL across an idle handle. A policy
    # installed after PREPARE must still reject before argument evaluation.
    from .classic_binary import binary_connection
    command=['--user=act_owner' if part.startswith('--user=') else part for part in instance[1]]
    with binary_connection((instance[0],command,instance[2])) as owner:
        owner.query('SET ROLE act_outer')
        statement,parameters,_=owner.prepare('CALL fixture.action_late(fixture.action_bad_id())')
        require(parameters==0,'late-policy fixture changed its prepared binding contract')
        try:
            execute(instance,'SET SESSION innodb_lock_wait_timeout=2;SET SESSION lock_wait_timeout=2;REQUIRE APPROVAL ON PROCEDURE fixture.ACTION_LATE;')
            try:owner.execute(statement)
            except RuntimeError as error:require('native error 61083:' in str(error),'late policy failed with '+str(error))
            else:raise RuntimeError('cached prepared CALL bypassed a newly installed approval policy')
        finally:owner.close_statement(statement)
    require(query(instance,'SELECT COUNT(*) FROM fixture.ledger WHERE id IN (861,863)')=='0','prepared CALL evaluated an argument/body after policy installation')
    print('PASS native CALL gate: case variants, pre-evaluation denial and late policy recheck on an idle binary prepared handle',flush=True)


def race(instance):
    # Hold the LAST dependency before action execution. Its deterministic scan
    # first pins every a_action_orders row and supremum gap, then waits on gate.
    # Independent INSERT and UPDATE must both wait until the entire action TX
    # commits. No sleeping application routine or test-only native pause exists.
    execute(instance,'CREATE ROLE act_drop_race;')
    with connection(instance,'act_owner','READ COMMITTED') as owner,connection(instance,'act_approver') as approver,connection(instance) as blocker,connection(instance) as dropper,connection(instance) as observer:
        run(owner,'SET ROLE act_outer');run(approver,'SET ROLE act_approve')
        action=prepare(owner,'action_race',2);approve(approver,action)
        run(blocker,'START TRANSACTION');run(blocker,'SELECT id FROM fixture.z_action_gate WHERE id=1 FOR UPDATE')
        errors=[];finished=threading.Event();inserted=threading.Event();updated=threading.Event();dropped=threading.Event()
        drop_thread=None
        owner_id=run(owner,'SELECT CONNECTION_ID()')[0][0]
        def execute_action():
            try:
                result=document(owner,'EXECUTE ACTION ?', (action,),True)
                require(result['state']=='succeeded','race action did not commit')
            except BaseException as error:errors.append(error)
            finally:finished.set()
        action_thread=threading.Thread(target=execute_action,daemon=True);action_thread.start()
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            count=query(instance,"SELECT COUNT(*) FROM performance_schema.data_lock_waits w JOIN performance_schema.threads t ON t.THREAD_ID=w.REQUESTING_THREAD_ID WHERE t.PROCESSLIST_ID="+str(owner_id))
            if count!='0':break
            require(not finished.is_set(),'action failed before the controlled dependency lock: '+repr(errors));time.sleep(.05)
        else:raise RuntimeError('action never reached the native dependency range-lock wait')
        def write(source,event):
            try:
                with connection(instance) as other:run(other,source)
            except BaseException as error:errors.append(error)
            finally:event.set()
        insert_thread=threading.Thread(target=write,args=("INSERT INTO fixture.a_action_orders VALUES(999,'new',0)",inserted),daemon=True)
        update_thread=threading.Thread(target=write,args=("UPDATE fixture.a_action_orders SET status='after-action' WHERE id=1",updated),daemon=True)
        insert_thread.start();update_thread.start()
        try:
            deadline=time.monotonic()+5
            while time.monotonic()<deadline:
                count=int(query(instance,'SELECT COUNT(*) FROM performance_schema.data_lock_waits'))
                if count>=3:break
                require(not inserted.is_set() and not updated.is_set(),'phantom/update escaped the approval range lock');time.sleep(.05)
            else:raise RuntimeError('independent INSERT/UPDATE did not reach observable native waits')
            require(not finished.is_set() and not inserted.is_set() and not updated.is_set(),'approval locks ended before application execution')
            def drop_role():
                try:run(dropper,'DROP ROLE act_drop_race')
                except BaseException as error:errors.append(error)
                finally:dropped.set()
            drop_thread=threading.Thread(target=drop_role,daemon=True);drop_thread.start()
            deadline=time.monotonic()+5
            while time.monotonic()<deadline:
                waits=run(observer,"SELECT COUNT(*) FROM performance_schema.metadata_locks WHERE OBJECT_TYPE='ACL CACHE' AND LOCK_STATUS='PENDING'")[0][0]
                if waits:break
                require(not dropped.is_set(),'native role DROP crossed an action authority lock');time.sleep(.05)
            else:raise RuntimeError('concurrent native DROP did not reach its observable ACL lock wait')
        finally:run(blocker,'ROLLBACK')
        for thread in (action_thread,insert_thread,update_thread,drop_thread):
            if thread is not None:thread.join(15);require(not thread.is_alive(),'native race operation did not terminate')
        require(not errors,repr(errors))
        require(run(owner,'SELECT @@session.transaction_isolation')==[('READ-COMMITTED',)],'owned action changed the caller session isolation')
        require(query(instance,'SELECT effects FROM fixture.a_action_orders WHERE id=2')=='1','race action was omitted or duplicated')
    print('PASS native approval race: RR full-PK/supremum locks block independent INSERT/UPDATE and ACL-before-row ordering serializes concurrent DROP; READ COMMITTED session restored',flush=True)


def maintenance_off(instance):
    """Run on the SAME fixture datadir restarted with actions=OFF."""
    receipts=query(instance,"SELECT COUNT(*) FROM sdp_ai.actions WHERE state='succeeded'")
    execute(instance,'DROP ROLE act_inner;CREATE ROLE act_inner;GRANT act_inner TO act_outer;')
    execute(instance,'RENAME USER act_rename_from TO act_rename_to;',3532)
    require(query(instance,"SELECT COUNT(*) FROM mysql.user WHERE User='act_rename_from' AND Host='%'")=='1' and query(instance,"SELECT COUNT(*) FROM mysql.user WHERE User='act_rename_to'")=='0','forbidden native role rename changed its account')
    require(query(instance,"SELECT COUNT(*) FROM sdp_ai.object_grants WHERE principal='role@act_rename_from@%'")=='1' and query(instance,"SELECT COUNT(*) FROM sdp_ai.object_grants WHERE principal='role@act_rename_to@%'")=='0','forbidden native role rename changed its custom grant')
    require(int(query(instance,"SELECT COUNT(*) FROM sdp_ai.actions WHERE state='approved' AND JSON_CONTAINS(owner_principals,JSON_QUOTE('role@act_rename_from@%'))"))>=1,'forbidden native role rename invalidated an unchanged approval')
    execute(instance,'RENAME USER act_rename_user@localhost TO act_renamed_user@localhost;')
    require(query(instance,"SELECT COUNT(*) FROM sdp_ai.object_grants WHERE principal='role@act_inner@%'")=='0','maintenance DROP resurrected custom object grants')
    require(query(instance,"SELECT COUNT(*) FROM sdp_ai.actions WHERE state IN ('prepared','approved') AND JSON_CONTAINS(owner_principals,JSON_QUOTE('role@act_inner@%'))")=='0','maintenance DROP preserved a pending approval over a replaced nested role')
    require(query(instance,"SELECT COUNT(*) FROM sdp_ai.actions WHERE state IN ('prepared','approved') AND owner='act_rename_user@localhost'")=='0' and query(instance,"SELECT COUNT(*) FROM sdp_ai.actions WHERE state='stale' AND owner='act_rename_user@localhost'")=='1','maintenance USER rename preserved a pending approval over its old owner')
    require(query(instance,"SELECT COUNT(*) FROM sdp_ai.object_grants WHERE principal='role@act_rename_user@localhost'")=='0' and query(instance,"SELECT COUNT(*) FROM sdp_ai.object_grants WHERE principal='role@act_renamed_user@localhost'")=='1','maintenance USER rename did not move the exact custom grant')
    require(query(instance,"SELECT COUNT(*) FROM sdp_ai.actions WHERE state='succeeded'")==receipts,'lifecycle deleted immutable action receipts')
    execute(instance,'GRANT act_renamed_user@localhost TO act_owner@localhost;')
    print('PASS native maintenance OFF lifecycle: DROP cleanup, native role rename3532 unchanged, valid USER rename grant migration/pending approval stale and immutable receipts',flush=True)


def syntax_off_enforcement(instance):
    """Same managed catalog, startup actions=ON and ai_sql=OFF."""
    with connection(instance,'act_owner') as owner:
        # The earlier maintenance DROP deliberately removed act_inner's
        # native EXECUTE grants. Activate the validly renamed user's grant so
        # approval enforcement for an otherwise authorized ordinary CALL.
        run(owner,'SET ROLE act_renamed_user@localhost')
        expect(owner,'CALL fixture.action_cancel(1)',(61083,))
    print('PASS readonly action enforcement remains active when AI SQL syntax is OFF',flush=True)


def maintenance_verify(instance):
    with connection(instance,'act_owner') as owner:
        run(owner,'SET ROLE act_outer')
        require(not model_visible(owner),'recreated role inherited dropped custom AI grants')
        run(owner,'SET ROLE act_renamed_user@localhost')
        require(model_visible(owner),'renamed native role lost its explicit custom AI grant')
    print('PASS native restart lifecycle: replacement stays unauthorized; validly renamed USER grant works only through explicit active-role membership',flush=True)


def main():
    dependencies=MIGRATION/'wire-v1/dependencies'
    require((dependencies/'mysql/connector/__init__.py').is_file(),'Provision the pinned native fixture connector dependencies first')
    sys.path.insert(1,str(dependencies))
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--openssl',type=Path,default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    parser.add_argument('--catalog',type=Path,default=REPOSITORY/'compatibility/ai/catalog.sql')
    parser.add_argument('--tool-catalog',type=Path,default=REPOSITORY/'compatibility/ai/tool_catalog.sql')
    parser.add_argument('--action-catalog',type=Path,default=REPOSITORY/'compatibility/ai/action_catalog.sql')
    args=parser.parse_args()
    source_paths=[path.resolve(strict=True) for path in (args.catalog,args.tool_catalog,args.action_catalog)]
    sources=[path.read_text(encoding='utf-8') for path in source_paths]
    binary=args.mysqld.resolve(strict=True)
    def fingerprints():
        return {str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in (binary,*source_paths)}
    proof=fingerprints()
    print('ACTION_ACCEPTANCE_INPUTS '+json.dumps(proof,sort_keys=True),flush=True)
    fixture=Fixture(binary,args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    def bootstrap(instance):
        install(instance,False)
        for source in sources:execute(instance,source)
    with fixture.scratch('ai-actions-native') as root:
        cert=backend_certificate(args.openssl.resolve(strict=True),root,'cert')
        with fixture.disposable(root/'server',tls_dir=cert,bootstrap=bootstrap,
          bootstrap_options=('--sdp-ai-sql=ON','--sdp-ai-actions=OFF'),
          server_options=('--sdp-ai-sql=ON','--sdp-ai-actions=ON')) as instance:
            exercise(instance)
            instance.restart(server_options=('--sdp-ai-sql=ON','--sdp-ai-actions=OFF'))
            maintenance_off(instance)
            instance.restart(server_options=('--sdp-ai-sql=ON','--sdp-ai-actions=ON'))
            maintenance_verify(instance)
            instance.restart(server_options=('--sdp-ai-sql=OFF','--sdp-ai-actions=ON'))
            syntax_off_enforcement(instance)
    require(fingerprints()==proof,'Action binary/catalog sources changed during native acceptance')
    print('PASS native action binary/catalog fingerprint stability',flush=True)


if __name__=='__main__':main()
