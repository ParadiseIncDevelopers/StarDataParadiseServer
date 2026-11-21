SET @__ai_private_grantee=CONCAT(CHAR(39),'sdp_ai_internal',CHAR(39),'@',CHAR(39),'localhost',CHAR(39));
SET @__ai_private_owner=(SELECT COUNT(*)=1 FROM mysql.user
  WHERE User='sdp_ai_internal' AND Host='localhost' AND account_locked='Y'
    AND JSON_UNQUOTE(JSON_EXTRACT(User_attributes,'$.metadata.sdp_ai_catalog'))='v1');
SET @__ai_private_owner=@__ai_private_owner AND NOT EXISTS(
  SELECT 1 FROM information_schema.USER_PRIVILEGES
  WHERE GRANTEE=@__ai_private_grantee AND (PRIVILEGE_TYPE<>'USAGE' OR IS_GRANTABLE<>'NO'));
SET @__ai_private_owner=@__ai_private_owner AND NOT EXISTS(
  SELECT 1 FROM mysql.global_grants WHERE USER='sdp_ai_internal' AND HOST='localhost');
SET @__ai_private_owner=@__ai_private_owner AND NOT EXISTS(
  SELECT 1 FROM information_schema.SCHEMA_PRIVILEGES WHERE GRANTEE=@__ai_private_grantee);
SET @__ai_private_owner=@__ai_private_owner AND NOT EXISTS(
  SELECT 1 FROM information_schema.COLUMN_PRIVILEGES WHERE GRANTEE=@__ai_private_grantee);
SET @__ai_private_owner=@__ai_private_owner AND NOT EXISTS(
  SELECT 1 FROM information_schema.TABLE_PRIVILEGES WHERE GRANTEE=@__ai_private_grantee
    AND (IS_GRANTABLE<>'NO' OR NOT (
      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='catalog_version' AND PRIVILEGE_TYPE IN ('SELECT'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='objects' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='object_versions' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='dependencies' AND PRIVILEGE_TYPE IN ('SELECT','INSERT'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='object_grants' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE','DELETE'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='conversations' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='runs' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='model_imports' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='outbox' AND PRIVILEGE_TYPE IN ('SELECT','INSERT'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='worker_accounts' AND PRIVILEGE_TYPE IN ('SELECT'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='run_tools' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='tool_rounds' AND PRIVILEGE_TYPE IN ('SELECT','INSERT'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='tool_calls' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='action_policies' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='action_grants' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE','DELETE'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='actions' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE'))
      OR      (TABLE_SCHEMA='sdp_jobs' AND TABLE_NAME='compute_jobs' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE'))
    )));
SET @__ai_private_owner=@__ai_private_owner AND NOT EXISTS(
  SELECT 1 FROM mysql.procs_priv WHERE User='sdp_ai_internal' AND Host='localhost'
    AND (Db<>'sdp_ai' OR Proc_priv<>'Execute' OR NOT (
      (Routine_type='FUNCTION' AND Routine_name IN ('json_kind','can_use','has_pending_turn','run_receipt','merge_memory','boolean_value','knowledge_value')) OR
      (Routine_type='PROCEDURE' AND Routine_name IN ('raise_error','assert_worker','validate_options','check_response_schema','validate_response','object_document','dispatch','knowledge_document','before_commit','after_commit','poll_commit','rollback_turn','claim_run','renew_lease','publish_run','claim_model_import','renew_model_import','publish_model_import','check_capture','prepare_response','check_behaviors','apply_behaviors','stage_behavior','behavior_binding','named_behavior','publish_tool_requests','begin_tool_call','finish_tool_call','knowledge_tool_document','tool_call_status','publish_tool_failure','tool_owner_context','bind_tool_action','tool_action_document','resolve_tool_action','action_dispatch')))));
SET @__ai_private_owner=@__ai_private_owner AND NOT EXISTS(
  SELECT 1 FROM mysql.role_edges WHERE (TO_USER='sdp_ai_internal' AND TO_HOST='localhost')
    OR (FROM_USER='sdp_ai_internal' AND FROM_HOST='localhost'));
SET @__ai_private_owner=@__ai_private_owner AND NOT EXISTS(
  SELECT 1 FROM mysql.default_roles WHERE (USER='sdp_ai_internal' AND HOST='localhost')
    OR (DEFAULT_ROLE_USER='sdp_ai_internal' AND DEFAULT_ROLE_HOST='localhost'));
SET @__ai_private_owner=@__ai_private_owner AND NOT EXISTS(
  SELECT 1 FROM mysql.proxies_priv WHERE (User='sdp_ai_internal' AND Host='localhost')
    OR (Proxied_user='sdp_ai_internal' AND Proxied_host='localhost'));
SET @__ai_private_owner=@__ai_private_owner AND NOT EXISTS(
  SELECT 1 FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA='mysql'
    AND ROUTINE_NAME IN ('__sdp_ai_restore_tool_grants_v1','__sdp_ai_restore_action_grants_v1') AND DEFINER<>'sdp_ai_internal@localhost');
SET @__ai_private_owner=@__ai_private_owner AND NOT EXISTS(
  SELECT 1 FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA='sdp_ai' AND DEFINER<>'sdp_ai_internal@localhost' AND (
    (ROUTINE_TYPE='FUNCTION' AND ROUTINE_NAME IN ('json_kind','can_use','has_pending_turn','run_receipt','merge_memory','boolean_value','knowledge_value')) OR
    (ROUTINE_TYPE='PROCEDURE' AND ROUTINE_NAME IN ('raise_error','assert_worker','validate_options','check_response_schema','validate_response','object_document','dispatch','knowledge_document','before_commit','after_commit','poll_commit','rollback_turn','claim_run','renew_lease','publish_run','claim_model_import','renew_model_import','publish_model_import','check_capture','prepare_response','check_behaviors','apply_behaviors','stage_behavior','behavior_binding','named_behavior','publish_tool_requests','begin_tool_call','finish_tool_call','knowledge_tool_document','tool_call_status','publish_tool_failure','tool_owner_context','bind_tool_action','tool_action_document','resolve_tool_action','action_dispatch'))));
SET @__ai_private_assert=IF(@__ai_private_owner,'DO 0',
  'SELECT * FROM information_schema.AI_PRIVATE_DEFINER_ACCOUNT_COLLISION');
PREPARE __ai_private_check FROM @__ai_private_assert;
EXECUTE __ai_private_check;
DEALLOCATE PREPARE __ai_private_check;
-- The installer must stop on the assertion error above. Existing accounts are
-- never locked, altered, revoked or reassigned by this component.
SET @__ai_private_owner=NULL,@__ai_private_assert=NULL,@__ai_private_grantee=NULL;

-- Preserve bounded application/worker routine grants in this same installation
-- connection. The one-shot INVOKER restore helper lives in protected mysql,
-- outside the namespace that routine authors may replace.
CREATE TEMPORARY TABLE sdp_ai.__saved_tool_grants_v1 AS
  SELECT p.User,p.Host,p.Routine_name,p.Routine_type,CAST(p.Proc_priv AS CHAR(128)) AS privileges,
    SHA2(CAST(JSON_OBJECT('plugin',u.plugin,'authentication',u.authentication_string,'locked',u.account_locked,'attributes',u.User_attributes) AS CHAR),256) AS account_fingerprint
  FROM mysql.procs_priv p JOIN mysql.user u ON BINARY u.User=BINARY p.User AND BINARY u.Host=BINARY p.Host
  WHERE p.Db='sdp_ai' AND p.Routine_type='PROCEDURE' AND p.Routine_name IN ('publish_tool_requests','claim_run','begin_tool_call','finish_tool_call','knowledge_tool_document','tool_call_status','publish_tool_failure','tool_owner_context','bind_tool_action','tool_action_document','resolve_tool_action','action_dispatch');
DELIMITER $$
DROP PROCEDURE IF EXISTS mysql.__sdp_ai_restore_tool_grants_v1$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE mysql.__sdp_ai_restore_tool_grants_v1()
SQL SECURITY INVOKER
BEGIN
  DECLARE user_value VARCHAR(128);DECLARE host_value VARCHAR(1024);DECLARE routine_value VARCHAR(64);
  DECLARE type_value VARCHAR(16);DECLARE privileges_value VARCHAR(128);DECLARE grant_value VARCHAR(128);
  DECLARE fingerprint_value CHAR(64);DECLARE current_fingerprint CHAR(64);
  DECLARE done BOOLEAN DEFAULT FALSE;
  DECLARE saved_grants CURSOR FOR SELECT User,Host,Routine_name,Routine_type,privileges,account_fingerprint FROM sdp_ai.__saved_tool_grants_v1;
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
    IF grant_value='' OR type_value<>'PROCEDURE' OR routine_value NOT IN ('publish_tool_requests','claim_run','begin_tool_call','finish_tool_call','knowledge_tool_document','tool_call_status','publish_tool_failure','tool_owner_context','bind_tool_action','tool_action_document','resolve_tool_action','action_dispatch') THEN SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Invalid saved AI routine grant.';END IF;
    IF FIND_IN_SET('NO_BACKSLASH_ESCAPES',@@sql_mode)=0 THEN
      SET user_value=REPLACE(user_value,CHAR(92),CONCAT(CHAR(92),CHAR(92))),host_value=REPLACE(host_value,CHAR(92),CONCAT(CHAR(92),CHAR(92)));
    END IF;
    SET @__ai_restore_grant=CONCAT('GRANT ',grant_value,' ON ',type_value,' `sdp_ai`.`',REPLACE(routine_value,'`','``'),'` TO ',
      CHAR(39),REPLACE(user_value,CHAR(39),CONCAT(CHAR(39),CHAR(39))),CHAR(39),'@',
      CHAR(39),REPLACE(host_value,CHAR(39),CONCAT(CHAR(39),CHAR(39))),CHAR(39));
    PREPARE __ai_restore_grant_stmt FROM @__ai_restore_grant;EXECUTE __ai_restore_grant_stmt;DEALLOCATE PREPARE __ai_restore_grant_stmt;
  END LOOP;
  CLOSE saved_grants;
END$$
DELIMITER ;

-- Tool continuation storage and batch publication. Install after catalog.sql.
-- Native registration/execution owns routine/ACL inspection; SQL stores only
-- protected snapshots and never executes provider names or SQL source text.
CREATE TABLE IF NOT EXISTS sdp_ai.run_tools (
  run_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
  tools JSON NOT NULL,
  round_number INT UNSIGNED NOT NULL DEFAULT 0,
  phase ENUM('inference','tools','approval') NOT NULL DEFAULT 'inference',
  max_rounds INT UNSIGNED NOT NULL DEFAULT 8,
  max_calls INT UNSIGNED NOT NULL DEFAULT 64,
  claimed_by VARCHAR(576) COLLATE utf8mb4_bin NULL,
  history JSON NULL,
  FOREIGN KEY(run_id) REFERENCES sdp_ai.runs(run_id)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS sdp_ai.tool_rounds (
  run_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  round_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  round_number INT UNSIGNED NOT NULL,
  publication_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  assistant_text LONGTEXT NOT NULL, provider_metadata JSON NOT NULL,
  created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY(run_id,round_id), UNIQUE KEY round_order(run_id,round_number),
  FOREIGN KEY(run_id) REFERENCES sdp_ai.runs(run_id)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS sdp_ai.tool_calls (
  call_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
  run_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  round_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  ordinal INT UNSIGNED NOT NULL,
  provider_call_id VARCHAR(256) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,
  tool_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  definition_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  arguments JSON NOT NULL, snapshot JSON NOT NULL,
  state ENUM('ready','waiting_approval','running','succeeded','failed','denied') NOT NULL,
  result JSON NULL, error JSON NULL,
  execution_worker VARCHAR(576) COLLATE utf8mb4_bin NULL,
  action_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL,
  action_snapshot_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
  created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6), finished_at DATETIME(6) NULL,
  UNIQUE KEY round_call(run_id,round_id,provider_call_id), UNIQUE KEY call_order(run_id,round_id,ordinal),
  FOREIGN KEY(run_id,round_id) REFERENCES sdp_ai.tool_rounds(run_id,round_id)
) ENGINE=InnoDB;

-- Reinstall also upgrades an earlier tool catalog in place. Never infer an
-- approval for a legacy call or rebuild/discard its existing transcript.
SET @__ai_tool_action_column=IF(EXISTS(SELECT 1 FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='tool_calls' AND COLUMN_NAME='action_id'),
  'DO 0','ALTER TABLE sdp_ai.tool_calls ADD COLUMN action_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL');
PREPARE __ai_tool_action_statement FROM @__ai_tool_action_column;
EXECUTE __ai_tool_action_statement;DEALLOCATE PREPARE __ai_tool_action_statement;
SET @__ai_tool_action_column=IF(EXISTS(SELECT 1 FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='tool_calls' AND COLUMN_NAME='action_snapshot_hash'),
  'DO 0','ALTER TABLE sdp_ai.tool_calls ADD COLUMN action_snapshot_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL');
PREPARE __ai_tool_action_statement FROM @__ai_tool_action_column;
EXECUTE __ai_tool_action_statement;DEALLOCATE PREPARE __ai_tool_action_statement;
SET @__ai_tool_action_column=NULL;

DELIMITER $$
-- Private native Action RPC leaves. They share the caller's owned transaction;
-- no COMMIT, ROLLBACK, provider SQL or caller-selected target is accepted.
DROP PROCEDURE IF EXISTS sdp_ai.tool_action_document$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.tool_action_document(IN p_call CHAR(32))
SQL SECURITY DEFINER
BEGIN
  DECLARE run_value CHAR(32);DECLARE owner_value VARCHAR(576);DECLARE conversation_value CHAR(32);DECLARE model_value CHAR(32);
  DECLARE revision_value BIGINT UNSIGNED;DECLARE epoch_value BIGINT UNSIGNED;
  DECLARE snapshot_value JSON;DECLARE arguments_value JSON;DECLARE execution_value JSON;
  IF STARDATAPARADISE_ACTION_DISPATCH_ACTIVE('tool')<>1 OR STARDATAPARADISE_TRANSACTION_ACTIVE()<>1 OR
      p_call IS NULL OR NOT REGEXP_LIKE(p_call,'^[0-9a-f]{32}$','c') THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  SELECT t.run_id,r.owner,r.conversation_id,r.conversation_revision,r.epoch,t.snapshot,t.arguments,r.execution_context,c.model_id
    INTO run_value,owner_value,conversation_value,revision_value,epoch_value,snapshot_value,arguments_value,execution_value,model_value
    FROM sdp_ai.tool_calls t JOIN sdp_ai.runs r ON r.run_id=t.run_id JOIN sdp_ai.conversations c ON c.conversation_id=r.conversation_id
    WHERE t.call_id=p_call AND t.state='waiting_approval' AND r.state='queued' AND c.revision=r.conversation_revision AND c.epoch=r.epoch FOR UPDATE;
  IF owner_value IS NULL OR owner_value<>STARDATAPARADISE_ACTOR() OR NOT sdp_ai.can_use(conversation_value,'APPEND') OR
      NOT sdp_ai.can_use(model_value,'USE') OR EXISTS(SELECT 1 FROM sdp_jobs.compute_jobs WHERE job_id=run_value AND cancel_requested) THEN CALL sdp_ai.raise_error('approval_stale');END IF;
  SELECT JSON_OBJECT('call_id',p_call,'run_id',run_value,'snapshot',snapshot_value,'arguments',arguments_value,
    'execution_context',execution_value,'roles',JSON_EXTRACT(execution_value,'$.roles')) AS document;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.bind_tool_action$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.bind_tool_action(IN p_call CHAR(32),IN p_action CHAR(32),IN p_snapshot CHAR(64))
SQL SECURITY DEFINER
BEGIN
  DECLARE run_value CHAR(32);DECLARE owner_value VARCHAR(576);DECLARE state_value VARCHAR(24);DECLARE action_owner VARCHAR(576);
  DECLARE action_call CHAR(32);DECLARE hash_value CHAR(64);DECLARE revision_value BIGINT UNSIGNED;DECLARE epoch_value BIGINT UNSIGNED;
  DECLARE current_revision BIGINT UNSIGNED;DECLARE current_epoch BIGINT UNSIGNED;DECLARE conversation_value CHAR(32);
  IF STARDATAPARADISE_ACTION_DISPATCH_ACTIVE('prepare')<>1 OR STARDATAPARADISE_TRANSACTION_ACTIVE()<>1 OR
      p_call IS NULL OR p_action IS NULL OR p_snapshot IS NULL OR NOT REGEXP_LIKE(p_call,'^[0-9a-f]{32}$','c') OR
      NOT REGEXP_LIKE(p_action,'^[0-9a-f]{32}$','c') OR NOT REGEXP_LIKE(p_snapshot,'^[0-9a-f]{64}$','c') THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  SELECT t.run_id,t.state,r.owner,r.conversation_id,r.conversation_revision,r.epoch
    INTO run_value,state_value,owner_value,conversation_value,revision_value,epoch_value
    FROM sdp_ai.tool_calls t JOIN sdp_ai.runs r ON r.run_id=t.run_id WHERE t.call_id=p_call AND r.state='queued' FOR UPDATE;
  SELECT owner,tool_call_id,snapshot_hash INTO action_owner,action_call,hash_value FROM sdp_ai.actions WHERE action_id=p_action AND state='prepared' FOR UPDATE;
  SELECT revision,epoch INTO current_revision,current_epoch FROM sdp_ai.conversations WHERE conversation_id=conversation_value FOR SHARE;
  IF state_value IS NULL OR state_value<>'waiting_approval' OR owner_value<>STARDATAPARADISE_ACTOR() OR NOT(owner_value<=>action_owner) OR
      NOT(action_call<=>p_call) OR NOT(hash_value<=>p_snapshot) OR NOT(current_revision<=>revision_value) OR NOT(current_epoch<=>epoch_value) OR
      NOT sdp_ai.can_use(conversation_value,'APPEND') OR EXISTS(SELECT 1 FROM sdp_jobs.compute_jobs WHERE job_id=run_value AND cancel_requested) THEN CALL sdp_ai.raise_error('approval_stale');END IF;
  UPDATE sdp_ai.tool_calls SET action_id=p_action,action_snapshot_hash=p_snapshot WHERE call_id=p_call;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.resolve_tool_action$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.resolve_tool_action(IN p_call CHAR(32),IN p_action CHAR(32),IN p_approved BOOLEAN)
SQL SECURITY DEFINER
resolve_block: BEGIN
  DECLARE run_value CHAR(32);DECLARE owner_value VARCHAR(576);DECLARE state_value VARCHAR(24);DECLARE round_value CHAR(32);DECLARE provider_value VARCHAR(256);
  DECLARE bound_action CHAR(32);DECLARE bound_hash CHAR(64);DECLARE action_hash CHAR(64);DECLARE action_state VARCHAR(24);DECLARE action_call CHAR(32);
  DECLARE waiting_value INT;DECLARE pending_value INT;DECLARE conversation_value CHAR(32);DECLARE current_revision BIGINT UNSIGNED;DECLARE current_epoch BIGINT UNSIGNED;
  DECLARE revision_value BIGINT UNSIGNED;DECLARE epoch_value BIGINT UNSIGNED;DECLARE content_doc JSON;DECLARE history_doc JSON;
  IF STARDATAPARADISE_TRANSACTION_ACTIVE()<>1 OR p_call IS NULL OR p_action IS NULL OR
      NOT REGEXP_LIKE(p_call,'^[0-9a-f]{32}$','c') OR NOT REGEXP_LIKE(p_action,'^[0-9a-f]{32}$','c') OR
      p_approved IS NULL OR p_approved NOT IN (0,1) OR
      (p_approved AND STARDATAPARADISE_ACTION_DISPATCH_ACTIVE('approve')<>1) OR
      (NOT p_approved AND STARDATAPARADISE_ACTION_DISPATCH_ACTIVE('deny')<>1) THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  SELECT t.run_id,t.state,t.round_id,t.provider_call_id,t.action_id,t.action_snapshot_hash,r.owner,r.conversation_id,r.conversation_revision,r.epoch
    INTO run_value,state_value,round_value,provider_value,bound_action,bound_hash,owner_value,conversation_value,revision_value,epoch_value
    FROM sdp_ai.tool_calls t JOIN sdp_ai.runs r ON r.run_id=t.run_id WHERE t.call_id=p_call AND r.state='queued' FOR UPDATE;
  SELECT state,tool_call_id,snapshot_hash INTO action_state,action_call,action_hash FROM sdp_ai.actions WHERE action_id=p_action FOR SHARE;
  SELECT revision,epoch INTO current_revision,current_epoch FROM sdp_ai.conversations WHERE conversation_id=conversation_value FOR SHARE;
  IF state_value IS NULL OR action_state IS NULL OR NOT(bound_action<=>p_action) OR NOT(action_call<=>p_call) OR NOT(action_hash<=>bound_hash) OR
      action_state<>IF(p_approved,'approved','denied') OR NOT(current_revision<=>revision_value) OR NOT(current_epoch<=>epoch_value) OR
      EXISTS(SELECT 1 FROM sdp_jobs.compute_jobs WHERE job_id=run_value AND cancel_requested) THEN CALL sdp_ai.raise_error('approval_stale');END IF;
  IF state_value=IF(p_approved,'ready','denied') THEN LEAVE resolve_block;END IF;
  IF state_value<>'waiting_approval' THEN CALL sdp_ai.raise_error('approval_stale');END IF;
  IF p_approved THEN
    UPDATE sdp_ai.tool_calls SET state='ready' WHERE call_id=p_call;
  ELSE
    SET content_doc=JSON_OBJECT('code',510001,'message','Action was denied.','action_id',p_action,'action_commit_state','not_started');
    UPDATE sdp_ai.tool_calls SET state='denied',error=content_doc,finished_at=UTC_TIMESTAMP(6) WHERE call_id=p_call;
    SET content_doc=JSON_OBJECT('role','tool','tool_call_id',provider_value,'content',JSON_OBJECT('error',content_doc));
    UPDATE sdp_ai.runs SET messages=JSON_ARRAY_APPEND(messages,'$',content_doc) WHERE run_id=run_value;
    UPDATE sdp_ai.run_tools SET history=JSON_ARRAY_APPEND(COALESCE(history,JSON_ARRAY()),'$',content_doc) WHERE run_id=run_value;
  END IF;
  SELECT COUNT(*) INTO waiting_value FROM sdp_ai.tool_calls WHERE run_id=run_value AND round_id=round_value AND state='waiting_approval';
  SELECT COUNT(*) INTO pending_value FROM sdp_ai.tool_calls WHERE run_id=run_value AND round_id=round_value AND state IN ('ready','waiting_approval','running');
  IF pending_value=0 THEN
    SELECT history INTO history_doc FROM sdp_ai.run_tools WHERE run_id=run_value;
    UPDATE sdp_ai.conversations SET messages=JSON_MERGE_PRESERVE(messages,history_doc) WHERE conversation_id=conversation_value;
    UPDATE sdp_ai.run_tools SET phase='inference',history=JSON_ARRAY() WHERE run_id=run_value;
  ELSEIF waiting_value=0 THEN
    UPDATE sdp_ai.run_tools SET phase='tools' WHERE run_id=run_value;
  END IF;
  INSERT INTO sdp_ai.outbox(owner,aggregate_id,event_kind,document) VALUES(owner_value,run_value,'tool_approval_resolved',
    JSON_OBJECT('run_id',run_value,'call_id',p_call,'action_id',p_action,'approved',p_approved,'approver',STARDATAPARADISE_ACTOR()));
END$$


DROP PROCEDURE IF EXISTS sdp_ai.publish_tool_requests$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.publish_tool_requests(
  IN p_run CHAR(32),IN p_worker CHAR(32),IN p_round CHAR(32),
  IN p_assistant LONGTEXT,IN p_calls JSON,IN p_metadata JSON)
SQL SECURITY DEFINER
publish_block: BEGIN
  DECLARE owner_value VARCHAR(576); DECLARE conversation_value CHAR(32);
  DECLARE revision_value BIGINT UNSIGNED; DECLARE epoch_value BIGINT UNSIGNED;
  DECLARE current_revision BIGINT UNSIGNED; DECLARE current_epoch BIGINT UNSIGNED;
  DECLARE worker_value CHAR(32); DECLARE lease_value DATETIME(6); DECLARE state_value VARCHAR(24);
  DECLARE tools_doc JSON; DECLARE call_doc JSON; DECLARE tool_doc JSON; DECLARE messages_doc JSON;
  DECLARE transcript_calls JSON DEFAULT JSON_ARRAY(); DECLARE call_ids JSON DEFAULT JSON_ARRAY();
  DECLARE round_value INT; DECLARE maximum_rounds INT; DECLARE maximum_calls INT;
  DECLARE i INT DEFAULT 0; DECLARE j INT; DECLARE matched INT; DECLARE approval_value BOOLEAN DEFAULT FALSE;
  DECLARE call_value CHAR(32); DECLARE provider_id VARCHAR(256); DECLARE provider_name VARCHAR(64);
  DECLARE hash_value CHAR(64); DECLARE existing_hash CHAR(64); DECLARE owned_transaction BOOLEAN DEFAULT FALSE;
  DECLARE EXIT HANDLER FOR SQLEXCEPTION BEGIN IF owned_transaction THEN ROLLBACK; END IF; RESIGNAL; END;
  CALL sdp_ai.assert_worker();
  IF NOT REGEXP_LIKE(p_worker,'^[0-9a-f]{32}$','c') OR NOT REGEXP_LIKE(p_round,'^[0-9a-f]{32}$','c') OR
      sdp_ai.json_kind(p_calls)<>'ARRAY' OR JSON_LENGTH(p_calls) NOT BETWEEN 1 AND 32 OR
      sdp_ai.json_kind(p_metadata)<>'OBJECT' OR OCTET_LENGTH(CAST(p_metadata AS CHAR))>1048576 OR
      p_assistant IS NULL OR OCTET_LENGTH(p_assistant)>1048576 OR OCTET_LENGTH(CAST(p_calls AS CHAR))>4194304 THEN
    CALL sdp_ai.raise_error('tool_arguments');
  END IF;
  SET hash_value=SHA2(CAST(JSON_OBJECT('assistant',p_assistant,'calls',p_calls,'metadata',p_metadata) AS CHAR),256);
  START TRANSACTION; SET owned_transaction=TRUE;
  SELECT owner,conversation_id,conversation_revision,epoch,lease_owner,lease_until,state,messages
    INTO owner_value,conversation_value,revision_value,epoch_value,worker_value,lease_value,state_value,messages_doc
    FROM sdp_ai.runs WHERE run_id=p_run FOR UPDATE;
  IF owner_value IS NULL OR NOT(worker_value<=>p_worker) THEN CALL sdp_ai.raise_error('commit_unknown'); END IF;
  SELECT publication_hash INTO existing_hash FROM sdp_ai.tool_rounds WHERE run_id=p_run AND round_id=p_round;
  IF existing_hash IS NOT NULL THEN
    IF existing_hash<>hash_value THEN CALL sdp_ai.raise_error('request_key_conflict'); END IF;
    COMMIT; SET owned_transaction=FALSE;
    SELECT JSON_OBJECT('run_id',p_run,'round_id',p_round,'state',
      IF(EXISTS(SELECT 1 FROM sdp_ai.tool_calls WHERE run_id=p_run AND round_id=p_round AND state='waiting_approval'),'waiting_approval','waiting_tool'),
      'replayed',TRUE) AS document; LEAVE publish_block;
  END IF;
  IF state_value<>'running' OR lease_value<UTC_TIMESTAMP(6) THEN CALL sdp_ai.raise_error('commit_unknown'); END IF;
  SELECT revision,epoch INTO current_revision,current_epoch FROM sdp_ai.conversations WHERE conversation_id=conversation_value FOR UPDATE;
  IF current_revision<>revision_value THEN CALL sdp_ai.raise_error('revision_conflict'); END IF;
  IF current_epoch<>epoch_value THEN CALL sdp_ai.raise_error('memory_epoch_changed'); END IF;
  IF EXISTS(SELECT 1 FROM sdp_jobs.compute_jobs WHERE job_id=p_run AND cancel_requested) THEN CALL sdp_ai.raise_error('run_cancelled'); END IF;
  SELECT tools,round_number,max_rounds,max_calls INTO tools_doc,round_value,maximum_rounds,maximum_calls
    FROM sdp_ai.run_tools WHERE run_id=p_run AND phase='inference' FOR UPDATE;
  IF tools_doc IS NULL OR JSON_LENGTH(tools_doc)=0 THEN CALL sdp_ai.raise_error('tool_denied'); END IF;
  IF round_value>=maximum_rounds OR JSON_LENGTH(p_calls)+(SELECT COUNT(*) FROM sdp_ai.tool_calls WHERE run_id=p_run)>maximum_calls THEN
    CALL sdp_ai.raise_error('context_budget');
  END IF;
  INSERT INTO sdp_ai.tool_rounds(run_id,round_id,round_number,publication_hash,assistant_text,provider_metadata)
    VALUES(p_run,p_round,round_value+1,hash_value,p_assistant,p_metadata);
  WHILE i<JSON_LENGTH(p_calls) DO
    SET call_doc=JSON_EXTRACT(p_calls,CONCAT('$[',i,']'));
    IF sdp_ai.json_kind(call_doc)<>'OBJECT' OR JSON_LENGTH(call_doc)<>3 OR
        sdp_ai.json_kind(JSON_EXTRACT(call_doc,'$.provider_call_id'))<>'STRING' OR
        sdp_ai.json_kind(JSON_EXTRACT(call_doc,'$.provider_name'))<>'STRING' OR
        sdp_ai.json_kind(JSON_EXTRACT(call_doc,'$.arguments'))<>'OBJECT' THEN CALL sdp_ai.raise_error('tool_arguments'); END IF;
    SET provider_id=JSON_UNQUOTE(JSON_EXTRACT(call_doc,'$.provider_call_id')),
      provider_name=JSON_UNQUOTE(JSON_EXTRACT(call_doc,'$.provider_name'));
    IF CHAR_LENGTH(provider_id) NOT BETWEEN 1 AND 256 OR LOCATE(X'00',CAST(provider_id AS BINARY))>0 OR
        NOT REGEXP_LIKE(provider_name,'^tool_[0-9a-f]{32}$','c') THEN CALL sdp_ai.raise_error('tool_arguments'); END IF;
    SET j=0,matched=0,tool_doc=NULL;
    WHILE j<JSON_LENGTH(tools_doc) DO
      IF JSON_UNQUOTE(JSON_EXTRACT(tools_doc,CONCAT('$[',j,'].provider_name')))=provider_name THEN
        SET matched=matched+1,tool_doc=JSON_EXTRACT(tools_doc,CONCAT('$[',j,']'));
      END IF;
      SET j=j+1;
    END WHILE;
    IF matched<>1 OR JSON_UNQUOTE(JSON_EXTRACT(tool_doc,'$.tool_id'))<>SUBSTRING(provider_name,6) OR
        NOT REGEXP_LIKE(JSON_UNQUOTE(JSON_EXTRACT(tool_doc,'$.definition_hash')),'^[0-9a-f]{64}$','c') THEN
      CALL sdp_ai.raise_error('tool_denied');
    END IF;
    CALL sdp_ai.validate_response(JSON_EXTRACT(call_doc,'$.arguments'),JSON_EXTRACT(tool_doc,'$.parameters'));
    SET call_value=REPLACE(UUID(),'-',''),approval_value=approval_value OR sdp_ai.boolean_value(JSON_EXTRACT(tool_doc,'$.approval_required'),FALSE);
    INSERT INTO sdp_ai.tool_calls(call_id,run_id,round_id,ordinal,provider_call_id,tool_id,definition_hash,arguments,snapshot,state)
      VALUES(call_value,p_run,p_round,i,provider_id,JSON_UNQUOTE(JSON_EXTRACT(tool_doc,'$.tool_id')),
        JSON_UNQUOTE(JSON_EXTRACT(tool_doc,'$.definition_hash')),JSON_EXTRACT(call_doc,'$.arguments'),tool_doc,
        IF(sdp_ai.boolean_value(JSON_EXTRACT(tool_doc,'$.approval_required'),FALSE),'waiting_approval','ready'));
    SET transcript_calls=JSON_ARRAY_APPEND(transcript_calls,'$',JSON_OBJECT('id',provider_id,'name',provider_name,'arguments',JSON_EXTRACT(call_doc,'$.arguments'))),
        call_ids=JSON_ARRAY_APPEND(call_ids,'$',JSON_OBJECT('call_id',call_value,'state',IF(sdp_ai.boolean_value(JSON_EXTRACT(tool_doc,'$.approval_required'),FALSE),'waiting_approval','ready'))),i=i+1;
  END WHILE;
  SET call_doc=JSON_OBJECT('role','assistant','content',p_assistant,'tool_calls',transcript_calls);
  IF JSON_CONTAINS_PATH(p_metadata,'one','$.provider_continuation') THEN
    IF sdp_ai.json_kind(JSON_EXTRACT(p_metadata,'$.provider_continuation'))<>'ARRAY' THEN CALL sdp_ai.raise_error('provider_protocol'); END IF;
    SET call_doc=JSON_SET(call_doc,'$.provider_continuation',JSON_EXTRACT(p_metadata,'$.provider_continuation'));
  END IF;
  UPDATE sdp_ai.runs SET messages=JSON_ARRAY_APPEND(messages_doc,'$',call_doc),state='queued',lease_until=NULL WHERE run_id=p_run;
  UPDATE sdp_ai.run_tools SET phase=IF(approval_value,'approval','tools'),round_number=round_number+1,
    history=JSON_ARRAY_APPEND(COALESCE(history,JSON_ARRAY()),'$',call_doc) WHERE run_id=p_run;
  UPDATE sdp_jobs.compute_jobs SET state='queued',events=JSON_ARRAY_APPEND(events,'$',JSON_OBJECT('state',IF(approval_value,'waiting_approval','waiting_tool'),'at',UNIX_TIMESTAMP())) WHERE job_id=p_run AND job_kind='ai';
  INSERT INTO sdp_ai.outbox(owner,aggregate_id,event_kind,document) VALUES(owner_value,p_run,'tool_requests_published',
    JSON_OBJECT('run_id',p_run,'round_id',p_round,'calls',call_ids,'state',IF(approval_value,'waiting_approval','waiting_tool')));
  COMMIT; SET owned_transaction=FALSE;
  SELECT JSON_OBJECT('run_id',p_run,'round_id',p_round,'state',IF(approval_value,'waiting_approval','waiting_tool'),'tool_calls',call_ids,'replayed',FALSE) AS document;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.claim_run$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.claim_run(IN p_worker CHAR(32),IN p_lease_seconds INT)
SQL SECURITY DEFINER
claim_block: BEGIN
  DECLARE id_value CHAR(32); DECLARE doc JSON; DECLARE calls_doc JSON; DECLARE phase_value VARCHAR(16);
  DECLARE owned_transaction BOOLEAN DEFAULT FALSE;
  DECLARE EXIT HANDLER FOR SQLEXCEPTION BEGIN IF owned_transaction THEN ROLLBACK; END IF; RESIGNAL; END;
  CALL sdp_ai.assert_worker();
  IF NOT REGEXP_LIKE(p_worker,'^[0-9a-f]{32}$','c') OR p_lease_seconds NOT BETWEEN 1 AND 3720 THEN CALL sdp_ai.raise_error('option_range'); END IF;
  START TRANSACTION; SET owned_transaction=TRUE;
  -- SQL tools have a protected per-call receipt in the same transaction as
  -- their application writes. Recovery retains call IDs and executes only
  -- uncommitted calls; it never repeats an external provider invocation.
  UPDATE sdp_ai.runs r JOIN sdp_ai.run_tools t ON t.run_id=r.run_id
    SET r.state='queued',r.lease_owner=NULL,r.lease_until=NULL
    WHERE r.state='running' AND r.lease_until<UTC_TIMESTAMP(6) AND t.phase='tools';
  UPDATE sdp_jobs.compute_jobs q JOIN sdp_ai.runs r ON r.job_id=q.job_id JOIN sdp_ai.run_tools t ON t.run_id=r.run_id
    SET q.state='queued',q.events=JSON_ARRAY_APPEND(q.events,'$',JSON_OBJECT('state','queued','recovery','native_tool_receipts','at',UNIX_TIMESTAMP()))
    WHERE q.job_kind='ai' AND q.state='running' AND r.state='queued' AND t.phase='tools';
  UPDATE sdp_ai.runs SET state='failed',finished_at=UTC_TIMESTAMP(6),error=JSON_OBJECT('code',511002,'phase','recovery','commit_state','committed','unknown_outcome',TRUE,'run_state','failed') WHERE state='running' AND lease_until<UTC_TIMESTAMP(6);
  UPDATE sdp_jobs.compute_jobs q JOIN sdp_ai.runs r ON q.job_id=r.job_id SET q.state='failed',q.error_code='511002',q.events=JSON_ARRAY_APPEND(q.events,'$',JSON_OBJECT('state','failed','unknown_outcome',TRUE,'at',UNIX_TIMESTAMP())) WHERE q.job_kind='ai' AND q.state='running' AND r.state='failed';
  SELECT r.run_id,COALESCE(t.phase,'inference') INTO id_value,phase_value FROM sdp_ai.runs r
    JOIN sdp_jobs.compute_jobs q ON q.job_id=r.job_id LEFT JOIN sdp_ai.run_tools t ON t.run_id=r.run_id
    WHERE r.state='queued' AND q.state='queued' AND q.job_kind='ai' AND (t.phase IS NULL OR t.phase<>'approval' OR q.cancel_requested)
    ORDER BY q.sequence LIMIT 1 FOR UPDATE SKIP LOCKED;
  IF id_value IS NULL THEN COMMIT; SET owned_transaction=FALSE; LEAVE claim_block; END IF;
  IF EXISTS(SELECT 1 FROM sdp_jobs.compute_jobs WHERE job_id=id_value AND cancel_requested) THEN
    UPDATE sdp_ai.runs SET state='cancelled',finished_at=UTC_TIMESTAMP(6),error=JSON_OBJECT('code',511004,'run_state','cancelled') WHERE run_id=id_value;
    UPDATE sdp_jobs.compute_jobs SET state='cancelled',error_code='511004',events=JSON_ARRAY_APPEND(events,'$',JSON_OBJECT('state','cancelled','at',UNIX_TIMESTAMP())) WHERE job_id=id_value;
    COMMIT; SET owned_transaction=FALSE; LEAVE claim_block;
  END IF;
  UPDATE sdp_ai.runs SET state='running',lease_owner=p_worker,lease_until=TIMESTAMPADD(SECOND,p_lease_seconds,UTC_TIMESTAMP(6)) WHERE run_id=id_value;
  UPDATE sdp_ai.run_tools SET claimed_by=STARDATAPARADISE_ACTOR() WHERE run_id=id_value;
  UPDATE sdp_jobs.compute_jobs SET state='running',attempt=attempt+1,events=JSON_ARRAY_APPEND(events,'$',JSON_OBJECT('state','running','phase',phase_value,'at',UNIX_TIMESTAMP())) WHERE job_id=id_value;
  SELECT JSON_OBJECT('run_id',r.run_id,'job_id',r.job_id,'owner',r.owner,'epoch',r.epoch,'conversation_revision',r.conversation_revision,
    'phase',phase_value,'model',r.model,'messages',r.messages,'instructions',r.instructions,'options',r.options,
    'response_format',r.response_format,'response_schema',r.response_schema,'functions',CAST(IF(t.run_id IS NULL,'false','true') AS JSON),
    'tools',COALESCE(t.tools,JSON_ARRAY()),'tool_round',COALESCE(t.round_number,0))
    INTO doc FROM sdp_ai.runs r LEFT JOIN sdp_ai.run_tools t ON t.run_id=r.run_id WHERE r.run_id=id_value;
  IF phase_value='tools' THEN
    SELECT COALESCE(JSON_ARRAYAGG(JSON_OBJECT('call_id',c.call_id,'state',c.state,'ordinal',c.ordinal)),JSON_ARRAY()) INTO calls_doc
      FROM sdp_ai.tool_calls c JOIN sdp_ai.tool_rounds t ON t.run_id=c.run_id AND t.round_id=c.round_id
      JOIN sdp_ai.run_tools r ON r.run_id=t.run_id AND r.round_number=t.round_number
      WHERE c.run_id=id_value AND c.state IN ('ready','waiting_approval');
    SET doc=JSON_SET(doc,'$.tool_calls_to_execute',calls_doc);
  END IF;
  COMMIT; SET owned_transaction=FALSE;
  SELECT doc AS document;
END$$

-- These APIs are not a worker-authenticated alternative to the native executor.
-- The read-only native predicates bind the protected row to its current C++
-- dispatch/publication stack; direct SQL cannot manufacture either predicate.
DROP PROCEDURE IF EXISTS sdp_ai.begin_tool_call$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.begin_tool_call(IN p_call CHAR(32))
SQL SECURITY DEFINER
call_block: BEGIN
  DECLARE run_value CHAR(32); DECLARE owner_value VARCHAR(576); DECLARE state_value VARCHAR(24);
  DECLARE doc JSON; DECLARE ordinal_value INT; DECLARE round_value CHAR(32); DECLARE execution_actor VARCHAR(576);
  DECLARE revision_value BIGINT UNSIGNED; DECLARE epoch_value BIGINT UNSIGNED;
  IF STARDATAPARADISE_TOOL_DISPATCH_ACTIVE(p_call)<>1 OR STARDATAPARADISE_TOOL_EXECUTION_ACTIVE()<>0 OR
      STARDATAPARADISE_TRANSACTION_ACTIVE()<>1 OR
      NOT EXISTS(SELECT 1 FROM sdp_ai.worker_accounts WHERE actor=STARDATAPARADISE_ACTOR() AND enabled) THEN
    CALL sdp_ai.raise_error('tool_denied');
  END IF;
  SELECT run_id,state,ordinal,round_id,execution_worker INTO run_value,state_value,ordinal_value,round_value,execution_actor
    FROM sdp_ai.tool_calls WHERE call_id=p_call FOR UPDATE;
  IF run_value IS NULL THEN CALL sdp_ai.raise_error('tool_denied'); END IF;
  IF state_value IN ('succeeded','failed','denied') AND execution_actor=STARDATAPARADISE_ACTOR() THEN
    SELECT JSON_OBJECT('call_id',call_id,'run_id',run_id,'state',state,'result',result,'error',error,'replayed',TRUE) AS document
      FROM sdp_ai.tool_calls WHERE call_id=p_call;
    LEAVE call_block;
  END IF;
  SELECT r.owner,r.conversation_revision,r.epoch INTO owner_value,revision_value,epoch_value
    FROM sdp_ai.runs r JOIN sdp_ai.run_tools t ON t.run_id=r.run_id
    WHERE r.run_id=run_value AND r.state='running' AND r.lease_until>=UTC_TIMESTAMP(6)
      AND t.claimed_by=STARDATAPARADISE_ACTOR() AND t.phase='tools' FOR UPDATE;
  IF owner_value IS NULL THEN CALL sdp_ai.raise_error('commit_unknown'); END IF;
  IF NOT EXISTS(SELECT 1 FROM sdp_ai.runs r JOIN sdp_ai.conversations c ON c.conversation_id=r.conversation_id
      WHERE r.run_id=run_value AND c.revision=revision_value AND c.epoch=epoch_value) THEN CALL sdp_ai.raise_error('approval_stale'); END IF;
  IF EXISTS(SELECT 1 FROM sdp_jobs.compute_jobs WHERE job_id=run_value AND cancel_requested) THEN CALL sdp_ai.raise_error('run_cancelled'); END IF;
  IF state_value='waiting_approval' THEN
    SELECT JSON_OBJECT('call_id',p_call,'run_id',run_value,'state','waiting_approval') AS document;LEAVE call_block;
  END IF;
  IF state_value<>'ready' OR EXISTS(SELECT 1 FROM sdp_ai.tool_calls WHERE run_id=run_value AND round_id=round_value
      AND ordinal<ordinal_value AND state IN ('ready','waiting_approval','running')) THEN CALL sdp_ai.raise_error('tool_denied'); END IF;
  UPDATE sdp_ai.tool_calls SET state='running',execution_worker=STARDATAPARADISE_ACTOR() WHERE call_id=p_call;
  SELECT JSON_OBJECT('call_id',c.call_id,'run_id',c.run_id,'owner',owner_value,'transport_actor',STARDATAPARADISE_ACTOR(),
    'epoch',epoch_value,'conversation_revision',revision_value,'state','running','snapshot',c.snapshot,'arguments',c.arguments,
    'timeout_ms',CAST(COALESCE(JSON_EXTRACT(r.options,'$.run_timeout_ms'),60000) AS UNSIGNED),
    'execution_context',r.execution_context,'action_id',c.action_id,'action_snapshot_hash',c.action_snapshot_hash)
    INTO doc FROM sdp_ai.tool_calls c JOIN sdp_ai.runs r ON r.run_id=c.run_id WHERE c.call_id=p_call;
  SELECT doc AS document;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.tool_owner_context$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.tool_owner_context(IN p_call CHAR(32))
SQL SECURITY DEFINER
BEGIN
  DECLARE conversation_value CHAR(32);DECLARE model_value CHAR(32);DECLARE owner_value VARCHAR(576);
  IF STARDATAPARADISE_TOOL_DISPATCH_ACTIVE(p_call)<>1 OR STARDATAPARADISE_TOOL_EXECUTION_ACTIVE()<>1 THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  SELECT r.conversation_id,c.model_id,r.owner INTO conversation_value,model_value,owner_value
    FROM sdp_ai.tool_calls t JOIN sdp_ai.runs r ON r.run_id=t.run_id JOIN sdp_ai.conversations c ON c.conversation_id=r.conversation_id
    WHERE t.call_id=p_call AND t.state='running' AND r.state='running' AND r.conversation_revision=c.revision AND r.epoch=c.epoch FOR SHARE;
  IF owner_value IS NULL OR owner_value<>STARDATAPARADISE_ACTOR() OR NOT sdp_ai.can_use(conversation_value,'APPEND') OR
      NOT sdp_ai.can_use(model_value,'USE') THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  SELECT JSON_OBJECT('authorized',TRUE) AS document;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.knowledge_tool_document$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.knowledge_tool_document(
  IN p_id CHAR(32),IN p_version BIGINT UNSIGNED,IN p_hash CHAR(64))
SQL SECURITY DEFINER
BEGIN
  DECLARE doc JSON;
  IF NOT sdp_ai.can_use(p_id,'EXECUTE') THEN CALL sdp_ai.raise_error('tool_denied'); END IF;
  SELECT v.document INTO doc FROM sdp_ai.object_versions v JOIN sdp_ai.objects o ON o.object_id=v.object_id
    WHERE o.object_id=p_id AND o.kind='knowledge' AND o.state='active' AND v.version=p_version AND v.state='active' FOR SHARE;
  IF doc IS NULL OR NOT(SHA2(CAST(doc AS CHAR),256)<=>p_hash) THEN CALL sdp_ai.raise_error('approval_stale'); END IF;
  SELECT doc AS document;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.finish_tool_call$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.finish_tool_call(IN p_call CHAR(32),IN p_result JSON,IN p_error JSON)
SQL SECURITY DEFINER
BEGIN
  DECLARE run_value CHAR(32); DECLARE round_value CHAR(32); DECLARE owner_value VARCHAR(576);
  DECLARE provider_value VARCHAR(256); DECLARE conversation_value CHAR(32); DECLARE worker_value VARCHAR(576);
  DECLARE state_value VARCHAR(24); DECLARE messages_doc JSON; DECLARE history_doc JSON; DECLARE content_doc JSON;
  DECLARE remaining_value INT;
  DECLARE revision_value BIGINT UNSIGNED;DECLARE epoch_value BIGINT UNSIGNED;
  DECLARE current_revision BIGINT UNSIGNED;DECLARE current_epoch BIGINT UNSIGNED;
  IF STARDATAPARADISE_TOOL_PUBLICATION_ACTIVE(p_call)<>1 OR STARDATAPARADISE_TRANSACTION_ACTIVE()<>1 OR
      NOT EXISTS(SELECT 1 FROM sdp_ai.worker_accounts WHERE actor=STARDATAPARADISE_ACTOR() AND enabled) THEN
    CALL sdp_ai.raise_error('tool_denied');
  END IF;
  IF (sdp_ai.json_kind(p_error) NOT IN ('MISSING','NULL','OBJECT')) OR
      OCTET_LENGTH(CAST(p_result AS CHAR))>1048576 OR OCTET_LENGTH(CAST(p_error AS CHAR))>65536 THEN CALL sdp_ai.raise_error('tool_arguments'); END IF;
  SELECT run_id,round_id,provider_call_id,execution_worker,state INTO run_value,round_value,provider_value,worker_value,state_value
    FROM sdp_ai.tool_calls WHERE call_id=p_call FOR UPDATE;
  IF run_value IS NULL OR state_value<>'running' OR NOT(worker_value<=>STARDATAPARADISE_ACTOR()) THEN CALL sdp_ai.raise_error('tool_denied'); END IF;
  SELECT owner,conversation_id,messages,conversation_revision,epoch INTO owner_value,conversation_value,messages_doc,revision_value,epoch_value FROM sdp_ai.runs
    WHERE run_id=run_value AND state='running' AND lease_until>=UTC_TIMESTAMP(6) FOR UPDATE;
  IF owner_value IS NULL THEN CALL sdp_ai.raise_error('commit_unknown'); END IF;
  SELECT revision,epoch INTO current_revision,current_epoch FROM sdp_ai.conversations WHERE conversation_id=conversation_value FOR UPDATE;
  IF current_revision<>revision_value OR current_epoch<>epoch_value THEN CALL sdp_ai.raise_error('approval_stale');END IF;
  IF EXISTS(SELECT 1 FROM sdp_jobs.compute_jobs WHERE job_id=run_value AND cancel_requested) THEN CALL sdp_ai.raise_error('run_cancelled');END IF;
  SET state_value=IF(sdp_ai.json_kind(p_error)='OBJECT','failed','succeeded'),
    content_doc=IF(state_value='failed',JSON_OBJECT('error',p_error),p_result);
  UPDATE sdp_ai.tool_calls SET state=state_value,result=IF(state_value='succeeded',p_result,NULL),error=p_error,
      finished_at=UTC_TIMESTAMP(6) WHERE call_id=p_call;
  SET content_doc=JSON_OBJECT('role','tool','tool_call_id',provider_value,'content',content_doc);
  UPDATE sdp_ai.runs SET messages=JSON_ARRAY_APPEND(messages_doc,'$',content_doc) WHERE run_id=run_value;
  UPDATE sdp_ai.run_tools SET history=JSON_ARRAY_APPEND(COALESCE(history,JSON_ARRAY()),'$',content_doc) WHERE run_id=run_value;
  SELECT COUNT(*) INTO remaining_value FROM sdp_ai.tool_calls WHERE run_id=run_value AND round_id=round_value AND state IN ('ready','waiting_approval','running');
  IF remaining_value=0 THEN
    SELECT history INTO history_doc FROM sdp_ai.run_tools WHERE run_id=run_value;
    UPDATE sdp_ai.conversations SET messages=JSON_MERGE_PRESERVE(messages,history_doc) WHERE conversation_id=conversation_value;
    UPDATE sdp_ai.run_tools SET phase='inference',history=JSON_ARRAY() WHERE run_id=run_value;
    UPDATE sdp_ai.runs SET state='queued',lease_until=NULL WHERE run_id=run_value;
    UPDATE sdp_jobs.compute_jobs SET state='queued',events=JSON_ARRAY_APPEND(events,'$',JSON_OBJECT('state','queued','phase','inference','at',UNIX_TIMESTAMP())) WHERE job_id=run_value AND job_kind='ai';
  END IF;
  INSERT INTO sdp_ai.outbox(owner,aggregate_id,event_kind,document) VALUES(owner_value,run_value,'tool_call_published',
    JSON_OBJECT('run_id',run_value,'call_id',p_call,'state',state_value,'transport_actor',STARDATAPARADISE_ACTOR()));
  SELECT JSON_OBJECT('run_id',run_value,'call_id',p_call,'state',IF(remaining_value=0,'queued','running'),'tool_state',state_value,'replayed',FALSE) AS document;
END$$
DELIMITER ;

DELIMITER $$
DROP PROCEDURE IF EXISTS sdp_ai.tool_call_status$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.tool_call_status(IN p_call CHAR(32))
SQL SECURITY DEFINER
BEGIN
  CALL sdp_ai.assert_worker();
  IF p_call IS NULL OR NOT REGEXP_LIKE(p_call,'^[0-9a-f]{32}$','c') THEN CALL sdp_ai.raise_error('tool_arguments');END IF;
  IF NOT EXISTS(SELECT 1 FROM sdp_ai.tool_calls c JOIN sdp_ai.run_tools t ON t.run_id=c.run_id
      WHERE c.call_id=p_call AND (t.claimed_by=STARDATAPARADISE_ACTOR() OR c.execution_worker=STARDATAPARADISE_ACTOR())) THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  -- A plain MVCC ready/running observation cannot prove that an earlier
  -- invocation will not commit. Only immutable terminal receipts prove it.
  SELECT JSON_OBJECT('call_id',c.call_id,'run_id',c.run_id,'state',r.state,'tool_state',c.state,
    'action_commit_state',CASE c.state WHEN 'succeeded' THEN 'committed' WHEN 'failed' THEN 'rolled_back' WHEN 'denied' THEN 'not_started' ELSE 'unknown' END,
    'result',c.result,'error',c.error,'replayed',CAST(IF(c.state IN ('succeeded','failed','denied'),'true','false') AS JSON)) AS document
    FROM sdp_ai.tool_calls c JOIN sdp_ai.runs r ON r.run_id=c.run_id WHERE c.call_id=p_call;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.publish_tool_failure$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.publish_tool_failure(IN p_run CHAR(32),IN p_worker CHAR(32),IN p_call CHAR(32),IN p_error JSON)
SQL SECURITY DEFINER
failure_block: BEGIN
  DECLARE run_value CHAR(32);DECLARE call_state VARCHAR(24);DECLARE owner_value VARCHAR(576);DECLARE conversation_value CHAR(32);
  DECLARE ordinal_value INT;DECLARE round_value CHAR(32);DECLARE provider_value VARCHAR(256);DECLARE remaining_value INT;
  DECLARE history_doc JSON;DECLARE content_doc JSON;DECLARE revision_value BIGINT UNSIGNED;DECLARE epoch_value BIGINT UNSIGNED;
  DECLARE current_revision BIGINT UNSIGNED;DECLARE current_epoch BIGINT UNSIGNED;
  DECLARE owned_transaction BOOLEAN DEFAULT FALSE;
  DECLARE EXIT HANDLER FOR SQLEXCEPTION BEGIN IF owned_transaction THEN ROLLBACK;END IF;RESIGNAL;END;
  CALL sdp_ai.assert_worker();
  IF sdp_ai.json_kind(p_error)<>'OBJECT' OR p_run IS NULL OR p_call IS NULL OR p_worker IS NULL OR
      NOT REGEXP_LIKE(p_run,'^[0-9a-f]{32}$','c') OR NOT REGEXP_LIKE(p_call,'^[0-9a-f]{32}$','c') OR NOT REGEXP_LIKE(p_worker,'^[0-9a-f]{32}$','c') OR
      JSON_CONTAINS_PATH(p_error,'all','$.run_id','$.call_id','$.code','$.native_errno','$.action_commit_state','$.phase','$.commit_state','$.unknown_outcome')<>1 OR
      sdp_ai.json_kind(JSON_EXTRACT(p_error,'$.code'))<>'INTEGER' OR sdp_ai.json_kind(JSON_EXTRACT(p_error,'$.native_errno'))<>'INTEGER' OR
      JSON_UNQUOTE(JSON_EXTRACT(p_error,'$.run_id'))<>p_run OR JSON_UNQUOTE(JSON_EXTRACT(p_error,'$.call_id'))<>p_call OR
      JSON_EXTRACT(p_error,'$.code')<>510005 OR JSON_EXTRACT(p_error,'$.native_errno')<>61085 OR
      JSON_UNQUOTE(JSON_EXTRACT(p_error,'$.action_commit_state'))<>'rolled_back' OR JSON_UNQUOTE(JSON_EXTRACT(p_error,'$.phase'))<>'tool' OR
      JSON_UNQUOTE(JSON_EXTRACT(p_error,'$.commit_state'))<>'committed' OR sdp_ai.json_kind(JSON_EXTRACT(p_error,'$.unknown_outcome'))<>'BOOLEAN' OR
      JSON_UNQUOTE(JSON_EXTRACT(p_error,'$.unknown_outcome'))<>'false' OR OCTET_LENGTH(CAST(p_error AS CHAR))>8192 THEN CALL sdp_ai.raise_error('tool_arguments');END IF;
  START TRANSACTION;SET owned_transaction=TRUE;
  SELECT run_id,state,ordinal,round_id,provider_call_id INTO run_value,call_state,ordinal_value,round_value,provider_value
    FROM sdp_ai.tool_calls WHERE call_id=p_call FOR UPDATE;
  IF run_value IS NULL OR run_value<>p_run THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  IF call_state IN ('succeeded','failed','denied') THEN
    IF NOT EXISTS(SELECT 1 FROM sdp_ai.run_tools t JOIN sdp_ai.tool_calls c ON c.run_id=t.run_id WHERE c.call_id=p_call
      AND (t.claimed_by=STARDATAPARADISE_ACTOR() OR c.execution_worker=STARDATAPARADISE_ACTOR())) THEN CALL sdp_ai.raise_error('tool_denied');END IF;
    SELECT JSON_OBJECT('call_id',c.call_id,'run_id',c.run_id,'state',r.state,'tool_state',c.state,'result',c.result,'error',c.error,'replayed',TRUE) AS document
      FROM sdp_ai.tool_calls c JOIN sdp_ai.runs r ON r.run_id=c.run_id WHERE c.call_id=p_call;
    COMMIT;SET owned_transaction=FALSE;LEAVE failure_block;
  END IF;
  SELECT r.owner,r.conversation_id,r.conversation_revision,r.epoch INTO owner_value,conversation_value,revision_value,epoch_value
    FROM sdp_ai.runs r JOIN sdp_ai.run_tools t ON t.run_id=r.run_id
    WHERE r.run_id=p_run AND r.state='running' AND r.lease_owner=p_worker AND r.lease_until>=UTC_TIMESTAMP(6)
      AND t.phase='tools' AND t.claimed_by=STARDATAPARADISE_ACTOR() FOR UPDATE;
  IF owner_value IS NULL OR call_state<>'ready' THEN CALL sdp_ai.raise_error('commit_unknown');END IF;
  IF EXISTS(SELECT 1 FROM sdp_ai.tool_calls WHERE run_id=p_run AND round_id=round_value AND ordinal<ordinal_value AND state IN ('ready','waiting_approval','running')) THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  SELECT revision,epoch INTO current_revision,current_epoch FROM sdp_ai.conversations WHERE conversation_id=conversation_value FOR UPDATE;
  IF current_revision<>revision_value OR current_epoch<>epoch_value THEN CALL sdp_ai.raise_error('approval_stale');END IF;
  UPDATE sdp_ai.tool_calls SET state='failed',error=p_error,result=NULL,execution_worker=STARDATAPARADISE_ACTOR(),finished_at=UTC_TIMESTAMP(6) WHERE call_id=p_call;
  SET content_doc=JSON_OBJECT('role','tool','tool_call_id',provider_value,'content',JSON_OBJECT('error',p_error));
  UPDATE sdp_ai.runs SET messages=JSON_ARRAY_APPEND(messages,'$',content_doc) WHERE run_id=p_run;
  UPDATE sdp_ai.run_tools SET history=JSON_ARRAY_APPEND(COALESCE(history,JSON_ARRAY()),'$',content_doc) WHERE run_id=p_run;
  SELECT COUNT(*) INTO remaining_value FROM sdp_ai.tool_calls WHERE run_id=p_run AND round_id=round_value AND state IN ('ready','waiting_approval','running');
  IF remaining_value=0 THEN
    SELECT history INTO history_doc FROM sdp_ai.run_tools WHERE run_id=p_run;
    UPDATE sdp_ai.conversations SET messages=JSON_MERGE_PRESERVE(messages,history_doc) WHERE conversation_id=conversation_value;
    UPDATE sdp_ai.run_tools SET phase='inference',history=JSON_ARRAY() WHERE run_id=p_run;
    UPDATE sdp_ai.runs SET state='queued',lease_until=NULL WHERE run_id=p_run;
    UPDATE sdp_jobs.compute_jobs SET state='queued',events=JSON_ARRAY_APPEND(events,'$',JSON_OBJECT('state','queued','phase','inference','at',UNIX_TIMESTAMP())) WHERE job_id=p_run AND job_kind='ai';
  END IF;
  INSERT INTO sdp_ai.outbox(owner,aggregate_id,event_kind,document) VALUES(owner_value,p_run,'tool_call_failed',
    JSON_OBJECT('run_id',p_run,'call_id',p_call,'action_commit_state','rolled_back','transport_actor',STARDATAPARADISE_ACTOR()));
  COMMIT;SET owned_transaction=FALSE;
  SELECT JSON_OBJECT('run_id',p_run,'call_id',p_call,'state',IF(remaining_value=0,'queued','running'),'tool_state','failed','action_commit_state','rolled_back','replayed',FALSE) AS document;
END$$
DELIMITER ;

GRANT SELECT,INSERT,UPDATE ON sdp_ai.run_tools TO 'sdp_ai_internal'@'localhost';
GRANT SELECT,INSERT ON sdp_ai.tool_rounds TO 'sdp_ai_internal'@'localhost';
GRANT SELECT,INSERT,UPDATE ON sdp_ai.tool_calls TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.publish_tool_requests TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.claim_run TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.begin_tool_call TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.finish_tool_call TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.knowledge_tool_document TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.tool_call_status TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.publish_tool_failure TO 'sdp_ai_internal'@'localhost';

CALL mysql.__sdp_ai_restore_tool_grants_v1();
DROP PROCEDURE mysql.__sdp_ai_restore_tool_grants_v1;
DROP TEMPORARY TABLE sdp_ai.__saved_tool_grants_v1;
SET @__ai_restore_grant=NULL;

GRANT EXECUTE ON PROCEDURE sdp_ai.tool_owner_context TO 'sdp_ai_internal'@'localhost';

GRANT EXECUTE ON PROCEDURE sdp_ai.bind_tool_action TO 'sdp_ai_internal'@'localhost';

GRANT EXECUTE ON PROCEDURE sdp_ai.tool_action_document TO 'sdp_ai_internal'@'localhost';

GRANT EXECUTE ON PROCEDURE sdp_ai.resolve_tool_action TO 'sdp_ai_internal'@'localhost';
