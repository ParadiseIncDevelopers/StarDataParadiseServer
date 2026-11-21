-- Native AI role ACL and durable approval/action catalog. Install after the AI
-- core/tool catalogs as the trusted administrator. Public callers receive only
-- EXECUTE; native Action_dispatch_scope is required before this RPC can run.
-- Product source is canonical in compatibility/ai after native13 acceptance.
-- Reinstall preserves exact per-routine grants and verifies that an account
-- was not replaced during installation. The helper lives in protected mysql.
SET @__ai_action_install_ok=EXISTS(SELECT 1 FROM mysql.user WHERE User='sdp_ai_internal' AND Host='localhost' AND account_locked='Y' AND JSON_UNQUOTE(JSON_EXTRACT(User_attributes,'$.metadata.sdp_ai_catalog'))='v1') AND NOT EXISTS(SELECT 1 FROM information_schema.ROUTINES WHERE (ROUTINE_SCHEMA='mysql' AND ROUTINE_NAME='__sdp_ai_restore_action_grants_v1' OR ROUTINE_SCHEMA='sdp_ai' AND ROUTINE_NAME='action_dispatch') AND DEFINER<>'sdp_ai_internal@localhost');
SET @__ai_action_install_check=IF(@__ai_action_install_ok,'DO 0','SELECT * FROM information_schema.AI_ACTION_DEFINER_ACCOUNT_COLLISION');
PREPARE __ai_action_install_assert FROM @__ai_action_install_check;EXECUTE __ai_action_install_assert;DEALLOCATE PREPARE __ai_action_install_assert;
SET @__ai_action_install_ok=NULL,@__ai_action_install_check=NULL;
CREATE TEMPORARY TABLE sdp_ai.__saved_action_grants_v1 AS
  SELECT p.User,p.Host,p.Routine_name,p.Routine_type,CAST(p.Proc_priv AS CHAR(128)) AS privileges,
    SHA2(CAST(JSON_OBJECT('plugin',u.plugin,'authentication',u.authentication_string,'locked',u.account_locked,'attributes',u.User_attributes) AS CHAR),256) AS account_fingerprint
  FROM mysql.procs_priv p JOIN mysql.user u ON BINARY u.User=BINARY p.User AND BINARY u.Host=BINARY p.Host WHERE p.Db='sdp_ai' AND Routine_type='PROCEDURE' AND Routine_name='action_dispatch';

-- One-shot INVOKER helper uses the installer's existing GRANT authority. Its
-- private DEFINER has no GRANT authority and is never used for escalation.
-- The protected mysql namespace prevents an application namespace author from
-- replacing an INVOKER helper before the administrator calls it.
DELIMITER $$
DROP PROCEDURE IF EXISTS mysql.__sdp_ai_restore_action_grants_v1$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE mysql.__sdp_ai_restore_action_grants_v1()
SQL SECURITY INVOKER
BEGIN
  DECLARE user_value VARCHAR(128);DECLARE host_value VARCHAR(1024);DECLARE routine_value VARCHAR(64);
  DECLARE type_value VARCHAR(16);DECLARE privileges_value VARCHAR(128);DECLARE grant_value VARCHAR(128);
  DECLARE fingerprint_value CHAR(64);DECLARE current_fingerprint CHAR(64);
  DECLARE done BOOLEAN DEFAULT FALSE;
  DECLARE saved_grants CURSOR FOR SELECT User,Host,Routine_name,Routine_type,privileges,account_fingerprint FROM sdp_ai.__saved_action_grants_v1;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done=TRUE;
  OPEN saved_grants;
  grant_loop:LOOP
    FETCH saved_grants INTO user_value,host_value,routine_value,type_value,privileges_value,fingerprint_value;
    IF done THEN LEAVE grant_loop;END IF;
    SELECT MAX(SHA2(CAST(JSON_OBJECT('plugin',plugin,'authentication',authentication_string,'locked',account_locked,'attributes',User_attributes) AS CHAR),256))
      INTO current_fingerprint FROM mysql.user WHERE BINARY User=BINARY user_value AND BINARY Host=BINARY host_value;
    IF current_fingerprint IS NULL OR NOT(current_fingerprint<=>fingerprint_value) THEN
      SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='AI routine grant account changed during upgrade.';
    END IF;
    SET grant_value=CONCAT_WS(',',IF(FIND_IN_SET('Execute',privileges_value),'EXECUTE',NULL),
      IF(FIND_IN_SET('Alter Routine',privileges_value),'ALTER ROUTINE',NULL),IF(FIND_IN_SET('Grant',privileges_value),'GRANT OPTION',NULL));
    IF grant_value='' OR type_value NOT IN ('PROCEDURE','FUNCTION') THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Invalid saved AI routine grant.';END IF;
    IF FIND_IN_SET('NO_BACKSLASH_ESCAPES',@@sql_mode)=0 THEN
      SET user_value=REPLACE(user_value,CHAR(92),CONCAT(CHAR(92),CHAR(92))),host_value=REPLACE(host_value,CHAR(92),CONCAT(CHAR(92),CHAR(92)));
    END IF;
    SET @__ai_action_restore_grant=CONCAT('GRANT ',grant_value,' ON ',type_value,' `sdp_ai`.`',REPLACE(routine_value,'`','``'),'` TO ',
      CHAR(39),REPLACE(user_value,CHAR(39),CONCAT(CHAR(39),CHAR(39))),CHAR(39),'@',
      CHAR(39),REPLACE(host_value,CHAR(39),CONCAT(CHAR(39),CHAR(39))),CHAR(39));
    PREPARE __ai_action_restore_grant_stmt FROM @__ai_action_restore_grant;EXECUTE __ai_action_restore_grant_stmt;DEALLOCATE PREPARE __ai_action_restore_grant_stmt;
  END LOOP;
  CLOSE saved_grants;
END$$
DELIMITER ;

CREATE TABLE IF NOT EXISTS sdp_ai.action_policies (
  schema_name VARCHAR(64) COLLATE utf8mb4_bin NOT NULL,
  routine_name VARCHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL,
  version BIGINT UNSIGNED NOT NULL DEFAULT 1,
  required BOOLEAN NOT NULL DEFAULT TRUE,
  changed_by VARCHAR(576) COLLATE utf8mb4_bin NOT NULL,
  updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY(schema_name,routine_name)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS sdp_ai.action_grants (
  schema_name VARCHAR(64) COLLATE utf8mb4_bin NOT NULL,
  routine_name VARCHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL,
  principal VARCHAR(584) COLLATE utf8mb4_bin NOT NULL,
  privilege_name VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  granted_by VARCHAR(576) COLLATE utf8mb4_bin NOT NULL,
  created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY(schema_name,routine_name,principal,privilege_name),
  CHECK(privilege_name='APPROVE')
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS sdp_ai.actions (
  action_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
  owner VARCHAR(576) COLLATE utf8mb4_bin NOT NULL,
  schema_name VARCHAR(64) COLLATE utf8mb4_bin NOT NULL,
  routine_name VARCHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci NOT NULL,
  state ENUM('prepared','approved','denied','executing','succeeded','stale') NOT NULL DEFAULT 'prepared',
  policy_version BIGINT UNSIGNED NOT NULL,
  definition_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  arguments_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  resources_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  snapshot_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  descriptor JSON NOT NULL, arguments JSON NOT NULL, bindings JSON NOT NULL, roles JSON NOT NULL, owner_principals JSON NOT NULL,
  resource_snapshot JSON NOT NULL,
  tool_call_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL,
  approver VARCHAR(576) COLLATE utf8mb4_bin NULL, approver_roles JSON NULL, approver_principals JSON NULL,
  approved_at DATETIME(6) NULL, decided_at DATETIME(6) NULL,
  created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  expires_at DATETIME(6) NOT NULL,
  result JSON NULL,
  UNIQUE KEY tool_action(tool_call_id), KEY owner_actions(owner,created_at)
) ENGINE=InnoDB;

DELIMITER $$
-- New installation has no app EXECUTE grants; the core catalog installer
-- snapshots/restores action_dispatch grants on subsequent upgrades.
DROP PROCEDURE IF EXISTS sdp_ai.action_dispatch$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.action_dispatch(IN p_action VARCHAR(32),IN p JSON)
SQL SECURITY DEFINER
main:BEGIN
  DECLARE actor VARCHAR(576) DEFAULT STARDATAPARADISE_ACTOR();
  DECLARE principals JSON DEFAULT NULL;
  DECLARE schema_value VARCHAR(64);DECLARE name_value VARCHAR(64) CHARACTER SET utf8mb3 COLLATE utf8mb3_general_ci;DECLARE object_value CHAR(32);
  DECLARE id_value CHAR(32);DECLARE owner_value VARCHAR(576);DECLARE state_value VARCHAR(16);
  DECLARE principal_value VARCHAR(584);DECLARE kind_value VARCHAR(32);DECLARE privilege_value VARCHAR(32);
  DECLARE policy_value BIGINT UNSIGNED DEFAULT 0;DECLARE current_policy BIGINT UNSIGNED DEFAULT 0;
  DECLARE required_value BOOLEAN DEFAULT FALSE;DECLARE expires_value DATETIME(6);
  DECLARE descriptor_value JSON;DECLARE arguments_value JSON;DECLARE bindings_value JSON;DECLARE roles_value JSON;
  DECLARE snapshot_value JSON;DECLARE result_value JSON;DECLARE call_value CHAR(32);
  DECLARE definition_value CHAR(64);DECLARE arguments_hash_value CHAR(64);DECLARE resource_hash_value CHAR(64);
  DECLARE snapshot_hash_value CHAR(64);
  DECLARE old_actor VARCHAR(576);DECLARE old_role VARCHAR(584);DECLARE new_actor VARCHAR(576);DECLARE new_role VARCHAR(584);
  DECLARE approver_value VARCHAR(576);DECLARE approver_roles_value JSON;DECLARE created_value DATETIME(6);DECLARE can_approve BOOLEAN DEFAULT FALSE;
  DECLARE i INT DEFAULT 0;
  IF NOT STARDATAPARADISE_ACTION_DISPATCH_ACTIVE(p_action) OR sdp_ai.json_kind(p)<>'OBJECT' THEN
    CALL sdp_ai.raise_error('tool_denied');
  END IF;
  IF p_action='tool' THEN
    CALL sdp_ai.tool_action_document(JSON_UNQUOTE(JSON_EXTRACT(p,'$.call_id')));
    LEAVE main;
  END IF;
  IF p_action IN ('drop_principal','rename_principal') THEN
    SET old_actor=JSON_UNQUOTE(JSON_EXTRACT(p,'$.actor')),old_role=JSON_UNQUOTE(JSON_EXTRACT(p,'$.role'));
    IF old_actor IS NULL OR old_role IS NULL THEN CALL sdp_ai.raise_error('tool_denied');END IF;
    -- Keep immutable receipts/audit; no pending human approval survives a
    -- dropped/renamed actor or any root/nested role in its original proof.
    UPDATE sdp_ai.actions SET state='stale',decided_at=UTC_TIMESTAMP(6)
      WHERE state IN ('prepared','approved','executing') AND (owner=old_actor OR approver=old_actor OR
        JSON_CONTAINS(owner_principals,JSON_QUOTE(old_role)) OR JSON_CONTAINS(approver_principals,JSON_QUOTE(old_role)));
    IF p_action='drop_principal' THEN
      DELETE FROM sdp_ai.object_grants WHERE principal IN (old_actor,old_role);
      DELETE FROM sdp_ai.action_grants WHERE principal IN (old_actor,old_role);
    ELSE
      SET new_actor=JSON_UNQUOTE(JSON_EXTRACT(p,'$.new_actor')),new_role=JSON_UNQUOTE(JSON_EXTRACT(p,'$.new_role'));
      IF new_actor IS NULL OR new_role IS NULL THEN CALL sdp_ai.raise_error('tool_denied');END IF;
      UPDATE sdp_ai.object_grants SET principal=IF(principal=old_actor,new_actor,new_role) WHERE principal IN (old_actor,old_role);
      UPDATE sdp_ai.action_grants SET principal=IF(principal=old_actor,new_actor,new_role) WHERE principal IN (old_actor,old_role);
    END IF;
    SELECT JSON_OBJECT('operation',p_action,'cleaned',TRUE) AS document;LEAVE main;
  END IF;
  SET principals=STARDATAPARADISE_PRINCIPALS();
  SET schema_value=JSON_UNQUOTE(JSON_EXTRACT(p,'$.schema_name')),name_value=JSON_UNQUOTE(JSON_EXTRACT(p,'$.name'));
  IF p_action IN ('policy','require','grant_approve','revoke_approve') THEN
    IF schema_value IS NULL OR name_value IS NULL OR CHAR_LENGTH(schema_value) NOT BETWEEN 1 AND 64 OR CHAR_LENGTH(name_value) NOT BETWEEN 1 AND 64 THEN
      CALL sdp_ai.raise_error('tool_denied');
    END IF;
    IF p_action='policy' THEN
      BEGIN
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET policy_value=0,required_value=FALSE;
        SELECT version,required INTO policy_value,required_value
          FROM sdp_ai.action_policies WHERE schema_name=schema_value AND routine_name=name_value FOR SHARE;
      END;
      SELECT JSON_OBJECT('required',required_value,'version',policy_value) AS document;LEAVE main;
    END IF;
    -- Native producer checked the current caller's ALTER ROUTINE authority;
    -- this marker cannot be constructed by a raw CALL or SQL variable.
    IF p_action='require' THEN
      INSERT INTO sdp_ai.action_policies(schema_name,routine_name,version,required,changed_by)
        VALUES(schema_value,name_value,1,TRUE,actor)
        ON DUPLICATE KEY UPDATE version=version+1,required=TRUE,changed_by=actor,updated_at=UTC_TIMESTAMP(6);
    ELSE
      SET principal_value=JSON_UNQUOTE(JSON_EXTRACT(p,'$.principal'));
      IF principal_value IS NULL OR CHAR_LENGTH(principal_value) NOT BETWEEN 3 AND 584 THEN CALL sdp_ai.raise_error('tool_denied');END IF;
      IF p_action='grant_approve' THEN
        INSERT INTO sdp_ai.action_grants(schema_name,routine_name,principal,privilege_name,granted_by)
          VALUES(schema_value,name_value,principal_value,'APPROVE',actor)
          ON DUPLICATE KEY UPDATE granted_by=actor,created_at=UTC_TIMESTAMP(6);
      ELSE
        DELETE FROM sdp_ai.action_grants WHERE schema_name=schema_value AND routine_name=name_value AND principal=principal_value AND privilege_name='APPROVE';
      END IF;
    END IF;
    SELECT JSON_OBJECT('schema_name',schema_value,'name',name_value,'operation',p_action) AS document;LEAVE main;
  END IF;
  IF p_action IN ('grant_object','revoke_object') THEN
    SET kind_value=JSON_UNQUOTE(JSON_EXTRACT(p,'$.kind')),principal_value=JSON_UNQUOTE(JSON_EXTRACT(p,'$.principal'));
    SELECT MAX(object_id) INTO object_value FROM sdp_ai.objects WHERE kind=kind_value AND schema_name=schema_value AND name=name_value AND state<>'retired' AND owner=actor;
    IF object_value IS NULL OR principal_value IS NULL OR sdp_ai.json_kind(JSON_EXTRACT(p,'$.privileges'))<>'ARRAY' OR JSON_LENGTH(JSON_EXTRACT(p,'$.privileges')) NOT BETWEEN 1 AND 2 THEN CALL sdp_ai.raise_error('tool_denied');END IF;
    WHILE i<JSON_LENGTH(JSON_EXTRACT(p,'$.privileges')) DO
      SET privilege_value=JSON_UNQUOTE(JSON_EXTRACT(p,CONCAT('$.privileges[',i,']')));
      IF NOT ((kind_value IN ('model','behavior','response_schema') AND privilege_value='USE') OR
        (kind_value='knowledge' AND privilege_value='EXECUTE') OR
        (kind_value='conversation' AND privilege_value IN ('SELECT','APPEND'))) THEN CALL sdp_ai.raise_error('tool_denied');END IF;
      IF p_action='grant_object' THEN
        INSERT INTO sdp_ai.object_grants(object_id,principal,privilege_name,granted_by) VALUES(object_value,principal_value,privilege_value,actor)
          ON DUPLICATE KEY UPDATE granted_by=actor,created_at=UTC_TIMESTAMP(6);
      ELSE DELETE FROM sdp_ai.object_grants WHERE object_id=object_value AND principal=principal_value AND privilege_name=privilege_value;
      END IF;
      SET i=i+1;
    END WHILE;
    SELECT JSON_OBJECT('object_id',object_value,'operation',p_action) AS document;LEAVE main;
  END IF;
  IF p_action='prepare' THEN
    SET descriptor_value=JSON_EXTRACT(p,'$.descriptor'),arguments_value=JSON_EXTRACT(p,'$.arguments'),bindings_value=JSON_EXTRACT(p,'$.bindings'),roles_value=JSON_EXTRACT(p,'$.roles'),snapshot_value=JSON_EXTRACT(p,'$.resources');
    SET schema_value=JSON_UNQUOTE(JSON_EXTRACT(descriptor_value,'$.schema_name')),name_value=JSON_UNQUOTE(JSON_EXTRACT(descriptor_value,'$.name'));
    SET definition_value=JSON_UNQUOTE(JSON_EXTRACT(descriptor_value,'$.definition_hash'));
    IF sdp_ai.json_kind(descriptor_value)<>'OBJECT' OR sdp_ai.json_kind(arguments_value)<>'OBJECT' OR sdp_ai.json_kind(bindings_value)<>'OBJECT' OR sdp_ai.json_kind(roles_value)<>'ARRAY' OR sdp_ai.json_kind(snapshot_value)<>'ARRAY' OR
      definition_value IS NULL OR definition_value NOT REGEXP '^[0-9a-f]{64}$' OR schema_value IS NULL OR name_value IS NULL THEN CALL sdp_ai.raise_error('tool_denied');END IF;
    BEGIN
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET policy_value=0;
      SELECT version INTO policy_value FROM sdp_ai.action_policies WHERE schema_name=schema_value AND routine_name=name_value FOR SHARE;
    END;
    SET id_value=LOWER(REPLACE(UUID(),'-','')),expires_value=TIMESTAMPADD(SECOND,300,UTC_TIMESTAMP(6));
    SET call_value=JSON_UNQUOTE(JSON_EXTRACT(p,'$.tool_call_id'));
    IF sdp_ai.json_kind(JSON_EXTRACT(p,'$.tool_call_id')) IN ('MISSING','NULL') THEN SET call_value=NULL;END IF;
    IF call_value IS NOT NULL AND call_value NOT REGEXP '^[0-9a-f]{32}$' THEN CALL sdp_ai.raise_error('tool_denied');END IF;
    SET arguments_hash_value=SHA2(CAST(arguments_value AS CHAR CHARACTER SET utf8mb4),256),resource_hash_value=SHA2(CAST(snapshot_value AS CHAR CHARACTER SET utf8mb4),256);
    SET snapshot_hash_value=SHA2(CAST(JSON_OBJECT('descriptor',descriptor_value,'arguments',arguments_value,'bindings',bindings_value,'roles',roles_value,'resources',snapshot_value,'policy_version',policy_value) AS CHAR CHARACTER SET utf8mb4),256);
    INSERT INTO sdp_ai.actions(action_id,owner,schema_name,routine_name,policy_version,definition_hash,arguments_hash,resources_hash,snapshot_hash,descriptor,arguments,bindings,roles,owner_principals,resource_snapshot,tool_call_id,expires_at)
      VALUES(id_value,actor,schema_value,name_value,policy_value,definition_value,arguments_hash_value,resource_hash_value,snapshot_hash_value,descriptor_value,arguments_value,bindings_value,roles_value,JSON_EXTRACT(principals,'$.principals'),snapshot_value,call_value,expires_value);
    IF call_value IS NOT NULL THEN CALL sdp_ai.bind_tool_action(call_value,id_value,snapshot_hash_value);END IF;
    SELECT JSON_OBJECT('action_id',id_value,'state','prepared','snapshot_hash',snapshot_hash_value,'expires_at',expires_value,'resource_scope','all_dependency_tables','resource_count',JSON_LENGTH(snapshot_value)) AS document;LEAVE main;
  END IF;
  SET id_value=JSON_UNQUOTE(JSON_EXTRACT(p,'$.action_id'));
  IF id_value IS NULL OR id_value NOT REGEXP '^[0-9a-f]{32}$' THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  SELECT MAX(owner) INTO owner_value FROM sdp_ai.actions WHERE action_id=id_value;
  IF owner_value IS NULL THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  SELECT owner,state,schema_name,routine_name,policy_version,expires_at,descriptor,arguments,bindings,roles,resource_snapshot,tool_call_id,definition_hash,arguments_hash,resources_hash,approver,approver_roles,created_at,result
    INTO owner_value,state_value,schema_value,name_value,policy_value,expires_value,descriptor_value,arguments_value,bindings_value,roles_value,snapshot_value,call_value,definition_value,arguments_hash_value,resource_hash_value,approver_value,approver_roles_value,created_value,result_value
    FROM sdp_ai.actions WHERE action_id=id_value FOR UPDATE;
  SET principal_value=NULL;
  BEGIN
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET principal_value=NULL;
    SELECT g.principal INTO principal_value FROM sdp_ai.action_grants g WHERE g.schema_name=schema_value AND g.routine_name=name_value AND g.privilege_name='APPROVE' AND JSON_CONTAINS(JSON_EXTRACT(principals,'$.principals'),JSON_QUOTE(g.principal)) ORDER BY g.principal LIMIT 1 FOR SHARE;
  END;
  SET can_approve=principal_value IS NOT NULL;
  IF p_action='describe' THEN
    IF actor<>owner_value AND NOT can_approve THEN CALL sdp_ai.raise_error('tool_denied');END IF;
    SELECT JSON_OBJECT('action_id',id_value,'state',IF(expires_value<UTC_TIMESTAMP(6) AND state_value IN ('prepared','approved'),'expired',state_value),'target',JSON_OBJECT('schema_name',schema_value,'name',name_value),'arguments',arguments_value,'definition_hash',definition_value,'resource_scope','all_dependency_tables','resource_count',JSON_LENGTH(snapshot_value),'created_at',created_value,'expires_at',expires_value,'approver',approver_value,'tool_call_id',call_value,'result',result_value) AS document;LEAVE main;
  END IF;
  IF p_action='consume' AND state_value='succeeded' THEN
    IF actor<>owner_value THEN CALL sdp_ai.raise_error('tool_denied');END IF;
    -- A completed immutable receipt is replayed; expiry/policy changes cannot
    -- turn it back into a new application execution or hide its outcome.
    SELECT JSON_OBJECT('action_id',id_value,'state','succeeded','tool_call_id',call_value,'descriptor',descriptor_value,'arguments',arguments_value,'result',result_value) AS document;LEAVE main;
  END IF;
  BEGIN
    DECLARE CONTINUE HANDLER FOR NOT FOUND SET current_policy=0;
    SELECT version INTO current_policy FROM sdp_ai.action_policies WHERE schema_name=schema_value AND routine_name=name_value FOR SHARE;
  END;
  IF expires_value<UTC_TIMESTAMP(6) OR current_policy<>policy_value THEN CALL sdp_ai.raise_error('approval_stale');END IF;
  IF p_action IN ('approve','deny') THEN
    IF NOT can_approve THEN CALL sdp_ai.raise_error('tool_denied');END IF;
    IF state_value NOT IN ('prepared','approved') THEN CALL sdp_ai.raise_error('approval_stale');END IF;
    UPDATE sdp_ai.actions SET state=IF(p_action='approve','approved','denied'),approver=actor,approver_roles=JSON_EXTRACT(principals,'$.roles'),approver_principals=JSON_EXTRACT(principals,'$.principals'),approved_at=IF(p_action='approve',UTC_TIMESTAMP(6),NULL),decided_at=UTC_TIMESTAMP(6) WHERE action_id=id_value;
    IF call_value IS NOT NULL THEN CALL sdp_ai.resolve_tool_action(call_value,id_value,p_action='approve');END IF;
    SELECT JSON_OBJECT('action_id',id_value,'state',IF(p_action='approve','approved','denied')) AS document;LEAVE main;
  END IF;
  IF p_action='validate_approval' THEN
    SET principal_value=NULL;
    BEGIN
      DECLARE CONTINUE HANDLER FOR NOT FOUND SET principal_value=NULL;
      SELECT g.principal INTO principal_value FROM sdp_ai.action_grants g WHERE g.schema_name=schema_value AND g.routine_name=name_value AND g.privilege_name='APPROVE' AND JSON_CONTAINS(JSON_EXTRACT(p,'$.principals'),JSON_QUOTE(g.principal)) ORDER BY g.principal LIMIT 1 FOR SHARE;
    END;
    IF actor<>owner_value OR state_value<>'approved' OR sdp_ai.json_kind(JSON_EXTRACT(p,'$.principals'))<>'ARRAY' OR principal_value IS NULL THEN
      CALL sdp_ai.raise_error('approval_stale');
    END IF;
    SELECT JSON_OBJECT('valid',TRUE) AS document;LEAVE main;
  END IF;
  IF p_action IN ('consume','finish') THEN
    IF actor<>owner_value THEN CALL sdp_ai.raise_error('tool_denied');END IF;
    -- Recheck the approver's original active-root authority natively before
    -- consume; native payload carries only the exact approved record, never
    -- a caller-selected alternate target or arguments.
    IF p_action='consume' THEN
      IF state_value='stale' THEN CALL sdp_ai.raise_error('approval_stale');END IF;
      IF state_value<>'approved' THEN CALL sdp_ai.raise_error('approval_required');END IF;
      SELECT JSON_OBJECT('action_id',id_value,'state',state_value,'owner',owner_value,'descriptor',descriptor_value,'arguments',arguments_value,'bindings',bindings_value,'roles',roles_value,'resources',snapshot_value,'tool_call_id',call_value,'definition_hash',definition_value,'approver',approver_value,'approver_roles',approver_roles_value,'arguments_hash',arguments_hash_value,'resources_hash',resource_hash_value) AS document;LEAVE main;
    ELSE
      IF state_value<>'approved' THEN CALL sdp_ai.raise_error('approval_stale');END IF;
      UPDATE sdp_ai.actions SET state='succeeded',result=JSON_EXTRACT(p,'$.result'),decided_at=UTC_TIMESTAMP(6) WHERE action_id=id_value;
      SELECT JSON_OBJECT('action_id',id_value,'state','succeeded','result',JSON_EXTRACT(p,'$.result')) AS document;LEAVE main;
    END IF;
  END IF;
  CALL sdp_ai.raise_error('tool_denied');
END$$
DELIMITER ;

GRANT SELECT,INSERT,UPDATE ON sdp_ai.action_policies TO 'sdp_ai_internal'@'localhost';
GRANT SELECT,INSERT,UPDATE,DELETE ON sdp_ai.action_grants TO 'sdp_ai_internal'@'localhost';
GRANT SELECT,INSERT,UPDATE ON sdp_ai.actions TO 'sdp_ai_internal'@'localhost';
GRANT INSERT,UPDATE,DELETE ON sdp_ai.object_grants TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.action_dispatch TO 'sdp_ai_internal'@'localhost';

CALL mysql.__sdp_ai_restore_action_grants_v1();
DROP PROCEDURE mysql.__sdp_ai_restore_action_grants_v1;
DROP TEMPORARY TABLE sdp_ai.__saved_action_grants_v1;
