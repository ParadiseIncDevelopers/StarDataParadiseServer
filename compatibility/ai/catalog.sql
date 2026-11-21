-- AI catalog v1. Install as the trusted schema administrator after the shared
-- sdp_jobs.compute_jobs migration. No application gets direct catalog DML.
-- Public entry points check the authenticated native actor, not CURRENT_USER()
-- (which is the private internal account in a SQL SECURITY DEFINER routine).
CREATE SCHEMA IF NOT EXISTS sdp_ai DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin;

-- The installer is trusted, while namespace/application routine authors are
-- not. No catalog routine inherits the installer's global/root authority.
CREATE USER IF NOT EXISTS 'sdp_ai_internal'@'localhost' ACCOUNT LOCK
  ATTRIBUTE '{"sdp_ai_catalog":"v1"}';
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
    AND ROUTINE_NAME IN ('__sdp_ai_restore_routine_grants_v1','__sdp_ai_restore_tool_grants_v1') AND DEFINER<>'sdp_ai_internal@localhost');
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

-- DROP/CREATE removes per-routine ACL rows. Preserve existing exact application
-- and worker grants before replacing managed routines. A temporary-name
-- collision fails before the first DROP; an existing user's temp is untouched.
CREATE TEMPORARY TABLE sdp_ai.__saved_routine_grants_v1 AS
  SELECT p.User,p.Host,p.Routine_name,p.Routine_type,CAST(p.Proc_priv AS CHAR(128)) AS privileges,
    SHA2(CAST(JSON_OBJECT('plugin',u.plugin,'authentication',u.authentication_string,'locked',u.account_locked,'attributes',u.User_attributes) AS CHAR),256) AS account_fingerprint
  FROM mysql.procs_priv p JOIN mysql.user u ON BINARY u.User=BINARY p.User AND BINARY u.Host=BINARY p.Host WHERE p.Db='sdp_ai' AND (
    (Routine_type='FUNCTION' AND Routine_name IN ('json_kind','can_use','has_pending_turn','run_receipt','merge_memory','boolean_value','knowledge_value')) OR
    (Routine_type='PROCEDURE' AND Routine_name IN ('raise_error','assert_worker','validate_options','check_response_schema','validate_response','object_document','dispatch','knowledge_document','before_commit','after_commit','poll_commit','rollback_turn','claim_run','renew_lease','publish_run','claim_model_import','renew_model_import','publish_model_import','check_capture','prepare_response','check_behaviors','apply_behaviors','stage_behavior','behavior_binding','named_behavior')));

-- One-shot INVOKER helper uses the installer's existing GRANT authority. Its
-- private DEFINER has no GRANT authority and is never used for escalation.
-- The protected mysql namespace prevents an application namespace author from
-- replacing an INVOKER helper before the administrator calls it.
DELIMITER $$
DROP PROCEDURE IF EXISTS mysql.__sdp_ai_restore_routine_grants_v1$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE mysql.__sdp_ai_restore_routine_grants_v1()
SQL SECURITY INVOKER
BEGIN
  DECLARE user_value VARCHAR(128);DECLARE host_value VARCHAR(1024);DECLARE routine_value VARCHAR(64);
  DECLARE type_value VARCHAR(16);DECLARE privileges_value VARCHAR(128);DECLARE grant_value VARCHAR(128);
  DECLARE fingerprint_value CHAR(64);DECLARE current_fingerprint CHAR(64);
  DECLARE done BOOLEAN DEFAULT FALSE;
  DECLARE saved_grants CURSOR FOR SELECT User,Host,Routine_name,Routine_type,privileges,account_fingerprint FROM sdp_ai.__saved_routine_grants_v1;
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
    SET @__ai_restore_grant=CONCAT('GRANT ',grant_value,' ON ',type_value,' `sdp_ai`.`',REPLACE(routine_value,'`','``'),'` TO ',
      CHAR(39),REPLACE(user_value,CHAR(39),CONCAT(CHAR(39),CHAR(39))),CHAR(39),'@',
      CHAR(39),REPLACE(host_value,CHAR(39),CONCAT(CHAR(39),CHAR(39))),CHAR(39));
    PREPARE __ai_restore_grant_stmt FROM @__ai_restore_grant;EXECUTE __ai_restore_grant_stmt;DEALLOCATE PREPARE __ai_restore_grant_stmt;
  END LOOP;
  CLOSE saved_grants;
END$$
DELIMITER ;

CREATE TABLE IF NOT EXISTS sdp_ai.catalog_version (
  singleton TINYINT PRIMARY KEY CHECK (singleton = 1), version INT NOT NULL
) ENGINE=InnoDB;
INSERT IGNORE INTO sdp_ai.catalog_version VALUES (1,1);

CREATE TABLE IF NOT EXISTS sdp_ai.objects (
  object_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
  kind VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  schema_name VARCHAR(64) NOT NULL, name VARCHAR(64) NOT NULL,
  owner VARCHAR(576) COLLATE utf8mb4_bin NOT NULL,
  version BIGINT UNSIGNED NOT NULL DEFAULT 1,
  state ENUM('active','pending_import','invalid','retired','archived') NOT NULL DEFAULT 'active',
  document JSON NOT NULL, created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  live_name VARCHAR(64) GENERATED ALWAYS AS (IF(state='retired',NULL,name)) STORED,
  UNIQUE KEY object_name(kind,schema_name,live_name), KEY owner_objects(owner,kind)
) ENGINE=InnoDB;
-- Add the terminal UI archive state without changing existing enum ordinals.
SET @__ai_archive_migration=IF((SELECT COLUMN_TYPE LIKE '%''archived''%' FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='objects' AND COLUMN_NAME='state'),'DO 0',
  'ALTER TABLE sdp_ai.objects MODIFY state ENUM(''active'',''pending_import'',''invalid'',''retired'',''archived'') NOT NULL DEFAULT ''active''');
PREPARE __ai_archive_state FROM @__ai_archive_migration;EXECUTE __ai_archive_state;DEALLOCATE PREPARE __ai_archive_state;
SET @__ai_archive_migration=NULL;
CREATE TABLE IF NOT EXISTS sdp_ai.object_versions (
  object_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  version BIGINT UNSIGNED NOT NULL, document JSON NOT NULL,
  state VARCHAR(24) NOT NULL, created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY(object_id,version), FOREIGN KEY(object_id) REFERENCES sdp_ai.objects(object_id)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS sdp_ai.dependencies (
  object_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  object_version BIGINT UNSIGNED NOT NULL,
  target_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  target_version BIGINT UNSIGNED NOT NULL, kind VARCHAR(32) NOT NULL,
  PRIMARY KEY(object_id,object_version,target_id,target_version),
  FOREIGN KEY(object_id) REFERENCES sdp_ai.objects(object_id),
  FOREIGN KEY(target_id,target_version) REFERENCES sdp_ai.object_versions(object_id,version)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS sdp_ai.object_grants (
  object_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  principal VARCHAR(584) COLLATE utf8mb4_bin NOT NULL,
  privilege_name VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  granted_by VARCHAR(576) COLLATE utf8mb4_bin NOT NULL,
  created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY(object_id,principal,privilege_name),
  FOREIGN KEY(object_id) REFERENCES sdp_ai.objects(object_id)
) ENGINE=InnoDB;
SET @__ai_principal_migration=IF((SELECT CHARACTER_MAXIMUM_LENGTH FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='object_grants' AND COLUMN_NAME='principal')<584,
  'ALTER TABLE sdp_ai.object_grants MODIFY COLUMN principal VARCHAR(584) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL','DO 0');
PREPARE __ai_principal_statement FROM @__ai_principal_migration;
EXECUTE __ai_principal_statement;DEALLOCATE PREPARE __ai_principal_statement;
SET @__ai_principal_migration=NULL;
CREATE TABLE IF NOT EXISTS sdp_ai.conversations (
  conversation_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
  model_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  model_version BIGINT UNSIGNED NOT NULL,
  revision BIGINT UNSIGNED NOT NULL DEFAULT 0, epoch BIGINT UNSIGNED NOT NULL DEFAULT 0,
  messages JSON NOT NULL, memory JSON NOT NULL,
  functions_enabled BOOLEAN NOT NULL DEFAULT FALSE,
  auto_append BOOLEAN NOT NULL DEFAULT TRUE, persistent_memory BOOLEAN NOT NULL DEFAULT TRUE,
  FOREIGN KEY(conversation_id) REFERENCES sdp_ai.objects(object_id),
  FOREIGN KEY(model_id,model_version) REFERENCES sdp_ai.object_versions(object_id,version)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS sdp_ai.runs (
  run_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
  job_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL UNIQUE,
  owner VARCHAR(576) COLLATE utf8mb4_bin NOT NULL,
  conversation_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  conversation_revision BIGINT UNSIGNED NOT NULL, epoch BIGINT UNSIGNED NOT NULL,
  request_key CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  request_fingerprint CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  state ENUM('queued','running','succeeded','refused','failed','cancelled') NOT NULL DEFAULT 'queued',
  model JSON NOT NULL, messages JSON NOT NULL, instructions LONGTEXT NOT NULL,
  options JSON NOT NULL, response_format VARCHAR(8) NOT NULL DEFAULT 'TEXT', response_schema JSON NULL,
  execution_context JSON NULL,
  response JSON NULL, metadata JSON NULL, error JSON NULL,
  lease_owner CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL, lease_until DATETIME(6) NULL,
  publication_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
  created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6), finished_at DATETIME(6) NULL,
  UNIQUE KEY request_once(owner,request_key), KEY claim_order(state,created_at),
  KEY conversation_runs(conversation_id,state),
  FOREIGN KEY(conversation_id) REFERENCES sdp_ai.conversations(conversation_id)
) ENGINE=InnoDB;
SET @__ai_execution_context_migration=IF(EXISTS(SELECT 1 FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='runs' AND COLUMN_NAME='execution_context'),
  'DO 0','ALTER TABLE sdp_ai.runs ADD COLUMN execution_context JSON NULL');
PREPARE __ai_execution_context_statement FROM @__ai_execution_context_migration;
EXECUTE __ai_execution_context_statement;DEALLOCATE PREPARE __ai_execution_context_statement;
SET @__ai_execution_context_migration=NULL;
CREATE TABLE IF NOT EXISTS sdp_ai.model_imports (
  job_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,
  model_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  version BIGINT UNSIGNED NOT NULL, owner VARCHAR(576) COLLATE utf8mb4_bin NOT NULL,
  relative_source VARCHAR(1024) NOT NULL, sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  context_tokens INT UNSIGNED NOT NULL DEFAULT 4096,
  state ENUM('queued','running','succeeded','failed') NOT NULL DEFAULT 'queued',
  lease_owner CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL, lease_until DATETIME(6) NULL,
  artifact JSON NULL, error JSON NULL, publication_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
  created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  KEY import_claim(state,created_at), FOREIGN KEY(model_id,version) REFERENCES sdp_ai.object_versions(object_id,version)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS sdp_ai.outbox (
  event_id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  owner VARCHAR(576) COLLATE utf8mb4_bin NOT NULL,
  aggregate_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  event_kind VARCHAR(40) NOT NULL, document JSON NOT NULL,
  created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6), KEY aggregate_events(aggregate_id,event_id)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS sdp_ai.worker_accounts (
  actor VARCHAR(576) COLLATE utf8mb4_bin PRIMARY KEY,
  enabled BOOLEAN NOT NULL DEFAULT TRUE
) ENGINE=InnoDB;

DELIMITER $$
-- Generated directly from contracts/sql/ai-catalog-v1.json; kept in sync by tests.
-- AI_ERROR_CATALOG_BEGIN
DROP PROCEDURE IF EXISTS sdp_ai.raise_error$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.raise_error(IN p_name VARCHAR(64)) SQL SECURITY DEFINER
BEGIN
  IF p_name='unknown_option' THEN
    SIGNAL SQLSTATE '22023' SET MYSQL_ERRNO=61001, MESSAGE_TEXT='502001: Unknown response option.';
  ELSEIF p_name='duplicate_option' THEN
    SIGNAL SQLSTATE '22023' SET MYSQL_ERRNO=61002, MESSAGE_TEXT='502002: Duplicate response option.';
  ELSEIF p_name='option_type' THEN
    SIGNAL SQLSTATE '22023' SET MYSQL_ERRNO=61003, MESSAGE_TEXT='502003: Invalid response option type.';
  ELSEIF p_name='option_range' THEN
    SIGNAL SQLSTATE '22023' SET MYSQL_ERRNO=61004, MESSAGE_TEXT='502004: Response option is out of range.';
  ELSEIF p_name='option_conflict' THEN
    SIGNAL SQLSTATE '22023' SET MYSQL_ERRNO=61005, MESSAGE_TEXT='502005: Incompatible response options.';
  ELSEIF p_name='unsupported_option' THEN
    SIGNAL SQLSTATE '0A000' SET MYSQL_ERRNO=61006, MESSAGE_TEXT='502006: Model does not support the requested option.';
  ELSEIF p_name='conversation_not_found' THEN
    SIGNAL SQLSTATE '42000' SET MYSQL_ERRNO=61011, MESSAGE_TEXT='503001: Conversation is unavailable.';
  ELSEIF p_name='no_active_conversation' THEN
    SIGNAL SQLSTATE '25000' SET MYSQL_ERRNO=61012, MESSAGE_TEXT='503002: No active conversation.';
  ELSEIF p_name='pending_changes' THEN
    SIGNAL SQLSTATE '25000' SET MYSQL_ERRNO=61013, MESSAGE_TEXT='503003: Conversation has pending changes.';
  ELSEIF p_name='revision_conflict' THEN
    SIGNAL SQLSTATE '40001' SET MYSQL_ERRNO=61014, MESSAGE_TEXT='503004: Conversation revision changed.';
  ELSEIF p_name='empty_response' THEN
    SIGNAL SQLSTATE '22000' SET MYSQL_ERRNO=61021, MESSAGE_TEXT='504001: Provider returned no final response.';
  ELSEIF p_name='invalid_json' THEN
    SIGNAL SQLSTATE '22032' SET MYSQL_ERRNO=61022, MESSAGE_TEXT='504002: Response is not valid JSON.';
  ELSEIF p_name='schema_mismatch' THEN
    SIGNAL SQLSTATE '22000' SET MYSQL_ERRNO=61023, MESSAGE_TEXT='504003: Response does not match its schema.';
  ELSEIF p_name='truncated_response' THEN
    SIGNAL SQLSTATE '22000' SET MYSQL_ERRNO=61024, MESSAGE_TEXT='504004: Provider response is incomplete.';
  ELSEIF p_name='response_not_ready' THEN
    SIGNAL SQLSTATE '55000' SET MYSQL_ERRNO=61025, MESSAGE_TEXT='504005: Response is not ready.';
  ELSEIF p_name='capture_type' THEN
    SIGNAL SQLSTATE '22000' SET MYSQL_ERRNO=61026, MESSAGE_TEXT='504006: Response capture type is incompatible.';
  ELSEIF p_name='invalid_unicode' THEN
    SIGNAL SQLSTATE '22021' SET MYSQL_ERRNO=61027, MESSAGE_TEXT='504007: Response contains invalid Unicode.';
  ELSEIF p_name='empty_context' THEN
    SIGNAL SQLSTATE '22000' SET MYSQL_ERRNO=61031, MESSAGE_TEXT='505001: Required context is empty.';
  ELSEIF p_name='context_budget' THEN
    SIGNAL SQLSTATE '54000' SET MYSQL_ERRNO=61032, MESSAGE_TEXT='505002: Context budget exceeded.';
  ELSEIF p_name='invalid_dependency' THEN
    SIGNAL SQLSTATE '55000' SET MYSQL_ERRNO=61041, MESSAGE_TEXT='506001: Knowledge dependency is unavailable.';
  ELSEIF p_name='knowledge_shape' THEN
    SIGNAL SQLSTATE '22000' SET MYSQL_ERRNO=61042, MESSAGE_TEXT='506002: Knowledge result schema changed.';
  ELSEIF p_name='behavior_not_found' THEN
    SIGNAL SQLSTATE '42000' SET MYSQL_ERRNO=61051, MESSAGE_TEXT='507001: Behavior version is unavailable.';
  ELSEIF p_name='behavior_conflict' THEN
    SIGNAL SQLSTATE '22023' SET MYSQL_ERRNO=61052, MESSAGE_TEXT='507002: Behavior settings conflict.';
  ELSEIF p_name='memory_budget' THEN
    SIGNAL SQLSTATE '54000' SET MYSQL_ERRNO=61061, MESSAGE_TEXT='508001: Memory budget exceeded.';
  ELSEIF p_name='memory_epoch_changed' THEN
    SIGNAL SQLSTATE '40001' SET MYSQL_ERRNO=61062, MESSAGE_TEXT='508002: Memory epoch changed before publication.';
  ELSEIF p_name='model_unavailable' THEN
    SIGNAL SQLSTATE '55000' SET MYSQL_ERRNO=61071, MESSAGE_TEXT='509001: Model or runtime is unavailable.';
  ELSEIF p_name='model_capability' THEN
    SIGNAL SQLSTATE '0A000' SET MYSQL_ERRNO=61072, MESSAGE_TEXT='509002: Model capability is unavailable.';
  ELSEIF p_name='provider_rate_limit' THEN
    SIGNAL SQLSTATE 'HY000' SET MYSQL_ERRNO=61073, MESSAGE_TEXT='509003: Provider quota or rate limit reached.';
  ELSEIF p_name='provider_timeout' THEN
    SIGNAL SQLSTATE 'HYT00' SET MYSQL_ERRNO=61074, MESSAGE_TEXT='509004: Provider deadline expired.';
  ELSEIF p_name='provider_auth' THEN
    SIGNAL SQLSTATE '28000' SET MYSQL_ERRNO=61075, MESSAGE_TEXT='509005: Provider credential was rejected.';
  ELSEIF p_name='provider_transport' THEN
    SIGNAL SQLSTATE 'HY000' SET MYSQL_ERRNO=61076, MESSAGE_TEXT='509006: Provider transport failed.';
  ELSEIF p_name='provider_protocol' THEN
    SIGNAL SQLSTATE 'HY000' SET MYSQL_ERRNO=61077, MESSAGE_TEXT='509007: Provider returned an invalid response.';
  ELSEIF p_name='unsafe_endpoint' THEN
    SIGNAL SQLSTATE '22023' SET MYSQL_ERRNO=61078, MESSAGE_TEXT='509008: Provider endpoint is not allowed.';
  ELSEIF p_name='model_integrity' THEN
    SIGNAL SQLSTATE '22000' SET MYSQL_ERRNO=61079, MESSAGE_TEXT='509009: Local model integrity verification failed.';
  ELSEIF p_name='tool_denied' THEN
    SIGNAL SQLSTATE '42000' SET MYSQL_ERRNO=61081, MESSAGE_TEXT='510001: Tool is outside the authorized allowlist.';
  ELSEIF p_name='tool_arguments' THEN
    SIGNAL SQLSTATE '22023' SET MYSQL_ERRNO=61082, MESSAGE_TEXT='510002: Tool arguments violate their contract.';
  ELSEIF p_name='approval_required' THEN
    SIGNAL SQLSTATE '42000' SET MYSQL_ERRNO=61083, MESSAGE_TEXT='510003: Action requires approval.';
  ELSEIF p_name='approval_stale' THEN
    SIGNAL SQLSTATE '40001' SET MYSQL_ERRNO=61084, MESSAGE_TEXT='510004: Action approval is no longer valid.';
  ELSEIF p_name='tool_transaction_failed' THEN
    SIGNAL SQLSTATE '45000' SET MYSQL_ERRNO=61085, MESSAGE_TEXT='510005: Tool transaction failed.';
  ELSEIF p_name='request_key_conflict' THEN
    SIGNAL SQLSTATE '23000' SET MYSQL_ERRNO=61091, MESSAGE_TEXT='511001: Request key identifies a different request.';
  ELSEIF p_name='commit_unknown' THEN
    SIGNAL SQLSTATE 'HY000' SET MYSQL_ERRNO=61092, MESSAGE_TEXT='511002: Commit outcome must be queried.';
  ELSEIF p_name='wait_timeout' THEN
    SIGNAL SQLSTATE 'HYT00' SET MYSQL_ERRNO=61093, MESSAGE_TEXT='511003: Client wait deadline expired; run may still be active.';
  ELSEIF p_name='run_cancelled' THEN
    SIGNAL SQLSTATE '57014' SET MYSQL_ERRNO=61094, MESSAGE_TEXT='511004: Run was cancelled.';
  ELSEIF p_name='decision_denied' THEN
    SIGNAL SQLSTATE '42000' SET MYSQL_ERRNO=61101, MESSAGE_TEXT='512001: Decision rejected the action.';
  ELSEIF p_name='decision_changed' THEN
    SIGNAL SQLSTATE '40001' SET MYSQL_ERRNO=61102, MESSAGE_TEXT='512002: Decision dependency changed.';
  ELSE SIGNAL SQLSTATE '45000' SET MYSQL_ERRNO=61999, MESSAGE_TEXT='AI contract error.';
  END IF;
END$$
-- AI_ERROR_CATALOG_END

DROP FUNCTION IF EXISTS sdp_ai.json_kind$$
CREATE DEFINER='sdp_ai_internal'@'localhost' FUNCTION sdp_ai.json_kind(p_value JSON) RETURNS VARCHAR(16)
DETERMINISTIC NO SQL SQL SECURITY DEFINER
RETURN CASE JSON_TYPE(p_value) WHEN 'UNSIGNED INTEGER' THEN 'INTEGER' ELSE COALESCE(JSON_TYPE(p_value),'MISSING') END$$

DROP FUNCTION IF EXISTS sdp_ai.can_use$$
DROP FUNCTION IF EXISTS sdp_ai.boolean_value$$
CREATE DEFINER='sdp_ai_internal'@'localhost' FUNCTION sdp_ai.boolean_value(p_value JSON,p_default BOOLEAN) RETURNS BOOLEAN
DETERMINISTIC NO SQL SQL SECURITY DEFINER
BEGIN
  IF sdp_ai.json_kind(p_value) IN ('MISSING','NULL') THEN RETURN p_default; END IF;
  IF sdp_ai.json_kind(p_value)='BOOLEAN' THEN RETURN JSON_UNQUOTE(p_value)='true'; END IF;
  IF sdp_ai.json_kind(p_value)='INTEGER' THEN
    IF JSON_UNQUOTE(p_value) NOT IN ('0','1') THEN CALL sdp_ai.raise_error('option_range'); END IF;
    RETURN JSON_UNQUOTE(p_value)='1';
  END IF;
  CALL sdp_ai.raise_error('option_type'); RETURN NULL;
END$$

CREATE DEFINER='sdp_ai_internal'@'localhost' FUNCTION sdp_ai.can_use(p_id CHAR(32), p_privilege VARCHAR(32)) RETURNS BOOLEAN
READS SQL DATA SQL SECURITY DEFINER
BEGIN
  DECLARE actor VARCHAR(576) DEFAULT STARDATAPARADISE_ACTOR();
  RETURN EXISTS(SELECT 1 FROM sdp_ai.objects o WHERE o.object_id=p_id AND o.state<>'retired'
    AND (o.owner=actor OR EXISTS(SELECT 1 FROM sdp_ai.object_grants g
      WHERE g.object_id=p_id AND JSON_CONTAINS(JSON_EXTRACT(STARDATAPARADISE_PRINCIPALS(),'$.principals'),JSON_QUOTE(g.principal))
        AND g.privilege_name IN (p_privilege,'OWNER'))));
END$$

DROP FUNCTION IF EXISTS sdp_ai.merge_memory$$
CREATE DEFINER='sdp_ai_internal'@'localhost' FUNCTION sdp_ai.merge_memory(p_base JSON,p_changes JSON) RETURNS JSON
DETERMINISTIC NO SQL SQL SECURITY DEFINER
BEGIN
  DECLARE result_doc JSON; DECLARE keys_doc JSON; DECLARE k VARCHAR(256); DECLARE i INT DEFAULT 0;
  SET result_doc=COALESCE(p_base,JSON_OBJECT()),p_changes=COALESCE(p_changes,JSON_OBJECT());
  IF sdp_ai.json_kind(result_doc)<>'OBJECT' OR sdp_ai.json_kind(p_changes)<>'OBJECT' THEN CALL sdp_ai.raise_error('option_type'); END IF;
  SET keys_doc=JSON_KEYS(p_changes);
  WHILE i<JSON_LENGTH(keys_doc) DO
    SET k=JSON_UNQUOTE(JSON_EXTRACT(keys_doc,CONCAT('$[',i,']')));
    IF CHAR_LENGTH(k) NOT BETWEEN 1 AND 128 OR LOCATE(X'00',CAST(k AS BINARY))>0 THEN CALL sdp_ai.raise_error('option_range'); END IF;
    SET result_doc=JSON_SET(result_doc,CONCAT('$.',JSON_QUOTE(k)),JSON_EXTRACT(p_changes,CONCAT('$.',JSON_QUOTE(k))));
    SET i=i+1;
  END WHILE;
  IF JSON_LENGTH(result_doc)>128 OR OCTET_LENGTH(CAST(result_doc AS CHAR))>65536 THEN CALL sdp_ai.raise_error('memory_budget'); END IF;
  RETURN result_doc;
END$$

DROP FUNCTION IF EXISTS sdp_ai.has_pending_turn$$
CREATE DEFINER='sdp_ai_internal'@'localhost' FUNCTION sdp_ai.has_pending_turn() RETURNS BOOLEAN
NO SQL SQL SECURITY INVOKER
BEGIN
  RETURN STARDATAPARADISE_TRANSACTION_DIRTY()=1 OR (@sdp_ai_session IS NOT NULL AND (
    COALESCE(JSON_LENGTH(JSON_EXTRACT(@sdp_ai_session,'$.pending')),0)>0 OR
    COALESCE(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.flush'))='true',FALSE) OR
    COALESCE(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.request_response'))='true',FALSE) OR
    COALESCE(CHAR_LENGTH(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.instructions'))),0)>0 OR
    COALESCE(JSON_LENGTH(JSON_EXTRACT(@sdp_ai_session,'$.response')),0)>0 OR
    COALESCE(JSON_LENGTH(JSON_EXTRACT(@sdp_ai_session,'$.memory_pending')),0)>0 OR
    COALESCE(JSON_LENGTH(JSON_EXTRACT(@sdp_ai_session,'$.temporary_pending')),0)>0 OR
    JSON_CONTAINS_PATH(@sdp_ai_session,'one','$.behaviors_pending')));
END$$

DROP PROCEDURE IF EXISTS sdp_ai.assert_worker$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.assert_worker()
SQL SECURITY DEFINER
BEGIN
  IF STARDATAPARADISE_TOOL_EXECUTION_ACTIVE()<>0 OR NOT EXISTS(SELECT 1 FROM sdp_ai.worker_accounts WHERE actor=STARDATAPARADISE_ACTOR() AND enabled) THEN
    SIGNAL SQLSTATE '42000' SET MYSQL_ERRNO=1142, MESSAGE_TEXT='AI worker operation is not authorized.';
  END IF;
  IF @@autocommit<>1 OR STARDATAPARADISE_TRANSACTION_ACTIVE()<>0 THEN
    CALL sdp_ai.raise_error('pending_changes');
  END IF;
END$$

-- AI_OPTION_VALIDATION_BEGIN
DROP PROCEDURE IF EXISTS sdp_ai.validate_options$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.validate_options(IN p_options JSON, IN p_format VARCHAR(8), IN p_capabilities JSON, OUT p_merged JSON)
SQL SECURITY DEFINER
BEGIN
  DECLARE keys_doc JSON; DECLARE value_doc JSON; DECLARE k VARCHAR(256); DECLARE i INT DEFAULT 0; DECLARE j INT;
  IF sdp_ai.json_kind(p_options)<>'OBJECT' THEN CALL sdp_ai.raise_error('option_type'); END IF;
  SET p_merged=CAST('{"encoding":"UTF-8","pretty":false,"max_output_bytes":1048576,"max_output_tokens":1500,"stream":false,"provider_timeout_ms":20000,"run_timeout_ms":60000,"max_attempts":1,"retry_backoff_ms":1000}' AS JSON),keys_doc=JSON_KEYS(p_options);
  WHILE i<JSON_LENGTH(keys_doc) DO
    SET k=JSON_UNQUOTE(JSON_EXTRACT(keys_doc,CONCAT('$[',i,']'))),value_doc=JSON_EXTRACT(p_options,CONCAT('$.',JSON_QUOTE(k)));
    CASE k
      WHEN 'encoding' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('STRING') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF JSON_UNQUOTE(value_doc) NOT IN ('UTF-8') THEN CALL sdp_ai.raise_error('option_range'); END IF;
        IF CHAR_LENGTH(JSON_UNQUOTE(value_doc)) NOT BETWEEN 1 AND 256 THEN CALL sdp_ai.raise_error('option_range'); END IF;
      WHEN 'schema' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('STRING') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF CHAR_LENGTH(JSON_UNQUOTE(value_doc)) NOT BETWEEN 1 AND 256 THEN CALL sdp_ai.raise_error('option_range'); END IF;
      WHEN 'schema_version' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('INTEGER') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF value_doc<1 OR value_doc>2147483647 THEN CALL sdp_ai.raise_error('option_range'); END IF;
      WHEN 'pretty' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('BOOLEAN') THEN CALL sdp_ai.raise_error('option_type'); END IF;
      WHEN 'max_output_bytes' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('INTEGER') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF value_doc<1 OR value_doc>16777216 THEN CALL sdp_ai.raise_error('option_range'); END IF;
      WHEN 'max_output_tokens' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('INTEGER') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF value_doc<1 OR value_doc>131072 THEN CALL sdp_ai.raise_error('option_range'); END IF;
      WHEN 'stream' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('BOOLEAN') THEN CALL sdp_ai.raise_error('option_type'); END IF;
      WHEN 'provider_timeout_ms' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('INTEGER') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF value_doc<1 OR value_doc>600000 THEN CALL sdp_ai.raise_error('option_range'); END IF;
      WHEN 'run_timeout_ms' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('INTEGER') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF value_doc<1 OR value_doc>3600000 THEN CALL sdp_ai.raise_error('option_range'); END IF;
      WHEN 'max_attempts' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('INTEGER') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF value_doc<1 OR value_doc>5 THEN CALL sdp_ai.raise_error('option_range'); END IF;
      WHEN 'retry_backoff_ms' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('INTEGER') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF value_doc<0 OR value_doc>600000 THEN CALL sdp_ai.raise_error('option_range'); END IF;
      WHEN 'temperature' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('INTEGER','DOUBLE','DECIMAL') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF value_doc<0 OR value_doc>2 THEN CALL sdp_ai.raise_error('option_range'); END IF;
        IF NOT JSON_CONTAINS(COALESCE(p_capabilities,JSON_ARRAY()),JSON_QUOTE('temperature')) THEN CALL sdp_ai.raise_error('unsupported_option'); END IF;
      WHEN 'top_p' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('INTEGER','DOUBLE','DECIMAL') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF value_doc<0 OR value_doc>1 THEN CALL sdp_ai.raise_error('option_range'); END IF;
        IF NOT JSON_CONTAINS(COALESCE(p_capabilities,JSON_ARRAY()),JSON_QUOTE('top_p')) THEN CALL sdp_ai.raise_error('unsupported_option'); END IF;
      WHEN 'seed' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('INTEGER') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF value_doc<0 OR value_doc>2147483647 THEN CALL sdp_ai.raise_error('option_range'); END IF;
        IF NOT JSON_CONTAINS(COALESCE(p_capabilities,JSON_ARRAY()),JSON_QUOTE('seed')) THEN CALL sdp_ai.raise_error('unsupported_option'); END IF;
      WHEN 'stop' THEN
        IF sdp_ai.json_kind(value_doc) NOT IN ('ARRAY') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        IF NOT JSON_CONTAINS(COALESCE(p_capabilities,JSON_ARRAY()),JSON_QUOTE('stop')) THEN CALL sdp_ai.raise_error('unsupported_option'); END IF;
        IF JSON_LENGTH(value_doc)>4 THEN CALL sdp_ai.raise_error('option_range'); END IF;
        SET j=0;
        WHILE j<JSON_LENGTH(value_doc) DO
          IF sdp_ai.json_kind(JSON_EXTRACT(value_doc,CONCAT('$[',j,']')))<>'STRING' OR CHAR_LENGTH(JSON_UNQUOTE(JSON_EXTRACT(value_doc,CONCAT('$[',j,']')))) NOT BETWEEN 1 AND 256 THEN CALL sdp_ai.raise_error('option_type'); END IF;
          SET j=j+1;
        END WHILE;
      ELSE CALL sdp_ai.raise_error('unknown_option');
    END CASE;
    SET p_merged=JSON_SET(p_merged,CONCAT('$.',JSON_QUOTE(k)),value_doc),i=i+1;
  END WHILE;
  IF JSON_EXTRACT(p_merged,'$.stream')=TRUE THEN CALL sdp_ai.raise_error('unsupported_option'); END IF;
  IF JSON_CONTAINS_PATH(p_merged,'one','$.schema')<>JSON_CONTAINS_PATH(p_merged,'one','$.schema_version') OR (JSON_CONTAINS_PATH(p_merged,'one','$.schema') AND p_format<>'JSON') THEN CALL sdp_ai.raise_error('option_conflict'); END IF;
  IF JSON_EXTRACT(p_merged,'$.provider_timeout_ms')>JSON_EXTRACT(p_merged,'$.run_timeout_ms') THEN CALL sdp_ai.raise_error('option_conflict'); END IF;
END$$
-- AI_OPTION_VALIDATION_END

DROP PROCEDURE IF EXISTS sdp_ai.check_response_schema$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.check_response_schema(IN p_schema JSON)
SQL SECURITY DEFINER
BEGIN
  DECLARE stack JSON DEFAULT JSON_ARRAY(); DECLARE node JSON; DECLARE item JSON; DECLARE keys_doc JSON;
  DECLARE child JSON; DECLARE depth INT; DECLARE visited INT DEFAULT 0; DECLARE i INT; DECLARE j INT;
  DECLARE key_name VARCHAR(256); DECLARE schema_names_doc JSON;
  IF OCTET_LENGTH(CAST(p_schema AS CHAR))>65536 THEN CALL sdp_ai.raise_error('option_range'); END IF;
  SET stack=JSON_ARRAY(JSON_OBJECT('node',p_schema,'depth',0));
  WHILE JSON_LENGTH(stack)>0 DO
    SET item=JSON_EXTRACT(stack,'$[0]'), stack=JSON_REMOVE(stack,'$[0]');
    SET node=JSON_EXTRACT(item,'$.node'), depth=JSON_EXTRACT(item,'$.depth'), visited=visited+1;
    IF depth>16 OR visited>1024 OR sdp_ai.json_kind(node) NOT IN ('OBJECT','BOOLEAN') THEN CALL sdp_ai.raise_error('option_range'); END IF;
    IF sdp_ai.json_kind(node)='OBJECT' THEN
      SET keys_doc=JSON_KEYS(node),i=0;
      WHILE i<JSON_LENGTH(keys_doc) DO
        SET key_name=JSON_UNQUOTE(JSON_EXTRACT(keys_doc,CONCAT('$[',i,']')));
        IF key_name NOT IN ('$schema','title','description','type','properties','required','additionalProperties',
          'items','minItems','maxItems','minLength','maxLength','minimum','maximum','enum','const') THEN
          CALL sdp_ai.raise_error('unsupported_option');
        END IF;
        SET i=i+1;
      END WHILE;
      IF JSON_CONTAINS_PATH(node,'one','$.$schema') AND JSON_UNQUOTE(JSON_EXTRACT(node,'$.$schema'))<>'https://json-schema.org/draft/2020-12/schema' THEN CALL sdp_ai.raise_error('unsupported_option'); END IF;
      SET i=0;
      WHILE i<2 DO
        SET key_name=IF(i=0,'title','description');
        IF JSON_CONTAINS_PATH(node,'one',CONCAT('$.',key_name)) AND sdp_ai.json_kind(JSON_EXTRACT(node,CONCAT('$.',key_name)))<>'STRING' THEN CALL sdp_ai.raise_error('option_type'); END IF;
        SET i=i+1;
      END WHILE;
      IF JSON_CONTAINS_PATH(node,'one','$.type') THEN
        SET schema_names_doc=JSON_EXTRACT(node,'$.type');
        IF sdp_ai.json_kind(schema_names_doc)<>'ARRAY' THEN SET schema_names_doc=JSON_ARRAY(schema_names_doc); END IF;
        IF JSON_LENGTH(schema_names_doc)=0 THEN CALL sdp_ai.raise_error('option_type'); END IF;
        SET i=0;
        WHILE i<JSON_LENGTH(schema_names_doc) DO
          SET child=JSON_EXTRACT(schema_names_doc,CONCAT('$[',i,']'));
          IF sdp_ai.json_kind(child)<>'STRING' OR JSON_UNQUOTE(child) NOT IN ('null','boolean','object','array','number','integer','string') THEN CALL sdp_ai.raise_error('option_type'); END IF;
          SET j=0;
          WHILE j<i DO
            IF child=JSON_EXTRACT(schema_names_doc,CONCAT('$[',j,']')) THEN CALL sdp_ai.raise_error('option_type'); END IF;
            SET j=j+1;
          END WHILE;
          SET i=i+1;
        END WHILE;
      END IF;
      IF JSON_CONTAINS_PATH(node,'one','$.properties') THEN
        SET child=JSON_EXTRACT(node,'$.properties');
        IF sdp_ai.json_kind(child)<>'OBJECT' OR JSON_LENGTH(child)>256 THEN CALL sdp_ai.raise_error('option_range'); END IF;
        SET schema_names_doc=JSON_KEYS(child), i=0;
        WHILE i<JSON_LENGTH(schema_names_doc) DO
          SET key_name=JSON_UNQUOTE(JSON_EXTRACT(schema_names_doc,CONCAT('$[',i,']')));
          SET stack=JSON_ARRAY_APPEND(stack,'$',JSON_OBJECT('node',JSON_EXTRACT(child,CONCAT('$.',JSON_QUOTE(key_name))),'depth',depth+1)),i=i+1;
        END WHILE;
      END IF;
      IF JSON_CONTAINS_PATH(node,'one','$.required') THEN
        SET schema_names_doc=JSON_EXTRACT(node,'$.required');
        IF sdp_ai.json_kind(schema_names_doc)<>'ARRAY' THEN CALL sdp_ai.raise_error('option_type'); END IF;
        SET i=0;
        WHILE i<JSON_LENGTH(schema_names_doc) DO
          SET child=JSON_EXTRACT(schema_names_doc,CONCAT('$[',i,']'));
          IF sdp_ai.json_kind(child)<>'STRING' THEN CALL sdp_ai.raise_error('option_type'); END IF;
          SET j=0;
          WHILE j<i DO
            IF child=JSON_EXTRACT(schema_names_doc,CONCAT('$[',j,']')) THEN CALL sdp_ai.raise_error('option_type'); END IF;
            SET j=j+1;
          END WHILE;
          SET i=i+1;
        END WHILE;
      END IF;
      SET i=0;
      WHILE i<2 DO
        SET key_name=IF(i=0,'additionalProperties','items');
        IF JSON_CONTAINS_PATH(node,'one',CONCAT('$.',key_name)) THEN
          SET stack=JSON_ARRAY_APPEND(stack,'$',JSON_OBJECT('node',JSON_EXTRACT(node,CONCAT('$.',key_name)),'depth',depth+1));
        END IF;
        SET i=i+1;
      END WHILE;
      SET i=0;
      WHILE i<4 DO
        SET key_name=ELT(i+1,'minItems','maxItems','minLength','maxLength');
        IF JSON_CONTAINS_PATH(node,'one',CONCAT('$.',key_name)) AND
          (sdp_ai.json_kind(JSON_EXTRACT(node,CONCAT('$.',key_name)))<>'INTEGER' OR
           JSON_EXTRACT(node,CONCAT('$.',key_name))<0 OR JSON_EXTRACT(node,CONCAT('$.',key_name))>1000000) THEN CALL sdp_ai.raise_error('option_range'); END IF;
        SET i=i+1;
      END WHILE;
      IF JSON_EXTRACT(node,'$.minItems')>JSON_EXTRACT(node,'$.maxItems') OR JSON_EXTRACT(node,'$.minLength')>JSON_EXTRACT(node,'$.maxLength') THEN CALL sdp_ai.raise_error('option_range'); END IF;
      SET i=0;
      WHILE i<2 DO
        SET key_name=IF(i=0,'minimum','maximum');
        IF JSON_CONTAINS_PATH(node,'one',CONCAT('$.',key_name)) AND sdp_ai.json_kind(JSON_EXTRACT(node,CONCAT('$.',key_name))) NOT IN ('INTEGER','DOUBLE','DECIMAL') THEN CALL sdp_ai.raise_error('option_type'); END IF;
        SET i=i+1;
      END WHILE;
      IF JSON_EXTRACT(node,'$.minimum')>JSON_EXTRACT(node,'$.maximum') THEN CALL sdp_ai.raise_error('option_range'); END IF;
      IF JSON_CONTAINS_PATH(node,'one','$.enum') AND (sdp_ai.json_kind(JSON_EXTRACT(node,'$.enum'))<>'ARRAY' OR JSON_LENGTH(JSON_EXTRACT(node,'$.enum')) NOT BETWEEN 1 AND 256) THEN CALL sdp_ai.raise_error('option_range'); END IF;
    END IF;
  END WHILE;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.object_document$$
DROP PROCEDURE IF EXISTS sdp_ai.validate_response$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.validate_response(IN p_value JSON,IN p_schema JSON)
SQL SECURITY DEFINER
BEGIN
  DECLARE stack JSON DEFAULT JSON_ARRAY(); DECLARE entry_doc JSON; DECLARE value_doc JSON; DECLARE schema_doc JSON;
  DECLARE types_doc JSON; DECLARE keys_doc JSON; DECLARE properties_doc JSON; DECLARE child_schema JSON;
  DECLARE enum_doc JSON; DECLARE required_doc JSON; DECLARE value_kind VARCHAR(16); DECLARE type_name VARCHAR(16); DECLARE key_name VARCHAR(256);
  DECLARE matched BOOLEAN; DECLARE enum_match BOOLEAN; DECLARE i INT; DECLARE depth INT; DECLARE visited INT DEFAULT 0;
  CALL sdp_ai.check_response_schema(p_schema);
  SET stack=JSON_ARRAY(JSON_OBJECT('value',p_value,'schema',p_schema,'depth',0));
  WHILE JSON_LENGTH(stack)>0 DO
    SET entry_doc=JSON_EXTRACT(stack,'$[0]'),stack=JSON_REMOVE(stack,'$[0]'),visited=visited+1;
    SET value_doc=JSON_EXTRACT(entry_doc,'$.value'),schema_doc=JSON_EXTRACT(entry_doc,'$.schema'),depth=JSON_EXTRACT(entry_doc,'$.depth');
    IF visited>100000 OR depth>64 THEN CALL sdp_ai.raise_error('schema_mismatch'); END IF;
    IF sdp_ai.json_kind(schema_doc)='BOOLEAN' THEN
      IF JSON_UNQUOTE(schema_doc)='false' THEN CALL sdp_ai.raise_error('schema_mismatch'); END IF;
    ELSE
      IF JSON_CONTAINS_PATH(schema_doc,'one','$.const') AND JSON_EXTRACT(schema_doc,'$.const')<>value_doc THEN CALL sdp_ai.raise_error('schema_mismatch'); END IF;
      IF JSON_CONTAINS_PATH(schema_doc,'one','$.enum') THEN
        SET enum_doc=JSON_EXTRACT(schema_doc,'$.enum'),enum_match=FALSE,i=0;
        WHILE i<JSON_LENGTH(enum_doc) DO
          IF value_doc=JSON_EXTRACT(enum_doc,CONCAT('$[',i,']')) THEN SET enum_match=TRUE; END IF;
          SET i=i+1;
        END WHILE;
        IF NOT enum_match THEN CALL sdp_ai.raise_error('schema_mismatch'); END IF;
      END IF;
      SET value_kind=sdp_ai.json_kind(value_doc);
      IF JSON_CONTAINS_PATH(schema_doc,'one','$.type') THEN
        SET types_doc=JSON_EXTRACT(schema_doc,'$.type'),matched=FALSE,i=0;
        IF sdp_ai.json_kind(types_doc)<>'ARRAY' THEN SET types_doc=JSON_ARRAY(types_doc); END IF;
        WHILE i<JSON_LENGTH(types_doc) DO
          SET type_name=JSON_UNQUOTE(JSON_EXTRACT(types_doc,CONCAT('$[',i,']')));
          IF (type_name='null' AND value_kind='NULL') OR (type_name='boolean' AND value_kind='BOOLEAN') OR
             (type_name='object' AND value_kind='OBJECT') OR (type_name='array' AND value_kind='ARRAY') OR
             (type_name='string' AND value_kind='STRING') OR (type_name='number' AND value_kind IN ('INTEGER','DOUBLE','DECIMAL')) OR
             (type_name='integer' AND value_kind IN ('INTEGER','DOUBLE','DECIMAL') AND FLOOR(CAST(JSON_UNQUOTE(value_doc) AS DOUBLE))=CAST(JSON_UNQUOTE(value_doc) AS DOUBLE)) THEN SET matched=TRUE; END IF;
          SET i=i+1;
        END WHILE;
        IF NOT matched THEN CALL sdp_ai.raise_error('schema_mismatch'); END IF;
      END IF;
      IF value_kind='OBJECT' THEN
        SET required_doc=COALESCE(JSON_EXTRACT(schema_doc,'$.required'),JSON_ARRAY()),i=0;
        WHILE i<JSON_LENGTH(required_doc) DO
          SET key_name=JSON_UNQUOTE(JSON_EXTRACT(required_doc,CONCAT('$[',i,']')));
          IF NOT JSON_CONTAINS_PATH(value_doc,'one',CONCAT('$.',JSON_QUOTE(key_name))) THEN CALL sdp_ai.raise_error('schema_mismatch'); END IF;
          SET i=i+1;
        END WHILE;
        SET properties_doc=COALESCE(JSON_EXTRACT(schema_doc,'$.properties'),JSON_OBJECT()),keys_doc=JSON_KEYS(value_doc),i=0;
        WHILE i<JSON_LENGTH(keys_doc) DO
          SET key_name=JSON_UNQUOTE(JSON_EXTRACT(keys_doc,CONCAT('$[',i,']')));
          IF JSON_CONTAINS_PATH(properties_doc,'one',CONCAT('$.',JSON_QUOTE(key_name))) THEN
            SET child_schema=JSON_EXTRACT(properties_doc,CONCAT('$.',JSON_QUOTE(key_name)));
          ELSE
            SET child_schema=COALESCE(JSON_EXTRACT(schema_doc,'$.additionalProperties'),CAST('true' AS JSON));
          END IF;
          SET stack=JSON_ARRAY_APPEND(stack,'$',JSON_OBJECT('value',JSON_EXTRACT(value_doc,CONCAT('$.',JSON_QUOTE(key_name))),'schema',child_schema,'depth',depth+1)),i=i+1;
        END WHILE;
      ELSEIF value_kind='ARRAY' THEN
        IF JSON_LENGTH(value_doc)<COALESCE(JSON_EXTRACT(schema_doc,'$.minItems'),0) OR JSON_LENGTH(value_doc)>COALESCE(JSON_EXTRACT(schema_doc,'$.maxItems'),1000000) THEN CALL sdp_ai.raise_error('schema_mismatch'); END IF;
        SET child_schema=COALESCE(JSON_EXTRACT(schema_doc,'$.items'),CAST('true' AS JSON)),i=0;
        WHILE i<JSON_LENGTH(value_doc) DO
          SET stack=JSON_ARRAY_APPEND(stack,'$',JSON_OBJECT('value',JSON_EXTRACT(value_doc,CONCAT('$[',i,']')),'schema',child_schema,'depth',depth+1)),i=i+1;
        END WHILE;
      ELSEIF value_kind='STRING' THEN
        IF CHAR_LENGTH(JSON_UNQUOTE(value_doc))<COALESCE(JSON_EXTRACT(schema_doc,'$.minLength'),0) OR CHAR_LENGTH(JSON_UNQUOTE(value_doc))>COALESCE(JSON_EXTRACT(schema_doc,'$.maxLength'),1000000) THEN CALL sdp_ai.raise_error('schema_mismatch'); END IF;
      ELSEIF value_kind IN ('INTEGER','DOUBLE','DECIMAL') THEN
        IF value_doc<JSON_EXTRACT(schema_doc,'$.minimum') OR value_doc>JSON_EXTRACT(schema_doc,'$.maximum') THEN CALL sdp_ai.raise_error('schema_mismatch'); END IF;
      END IF;
    END IF;
  END WHILE;
END$$

CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.object_document(IN p_id CHAR(32)) SQL SECURITY DEFINER
BEGIN
  DECLARE doc JSON; DECLARE deps JSON; DECLARE versions_doc JSON; DECLARE tool_summary JSON;
  SELECT JSON_SET(document,'$.kind',kind) INTO doc FROM sdp_ai.objects WHERE object_id=p_id;
  IF NOT sdp_ai.can_use(p_id,CASE JSON_UNQUOTE(JSON_EXTRACT(doc,'$.kind')) WHEN 'conversation' THEN 'SELECT' WHEN 'knowledge' THEN 'EXECUTE' ELSE 'USE' END) THEN CALL sdp_ai.raise_error('invalid_dependency'); END IF;
  IF JSON_UNQUOTE(JSON_EXTRACT(doc,'$.kind'))='conversation' THEN
    SELECT JSON_SET(doc,'$.messages',c.messages,'$.memory',c.memory,'$.revision',c.revision,'$.epoch',c.epoch) INTO doc
      FROM sdp_ai.conversations c WHERE c.conversation_id=p_id;
    SET tool_summary=JSON_ARRAY();
    IF EXISTS(SELECT 1 FROM sdp_ai.runs WHERE conversation_id=p_id AND JSON_EXTRACT(execution_context,'$.has_tools')=TRUE) THEN
      SELECT COALESCE(JSON_ARRAYAGG(JSON_OBJECT('run_id',recent.run_id,'call_id',recent.call_id,'state',recent.state,
        'kind',JSON_EXTRACT(recent.snapshot,'$.kind'),'schema_name',JSON_EXTRACT(recent.snapshot,'$.schema_name'),
        'name',JSON_EXTRACT(recent.snapshot,'$.name'),'action_id',recent.action_id)),JSON_ARRAY()) INTO tool_summary
        FROM (SELECT t.run_id,t.call_id,t.state,t.snapshot,t.action_id FROM sdp_ai.tool_calls t
          JOIN sdp_ai.runs r ON r.run_id=t.run_id WHERE r.conversation_id=p_id
          ORDER BY r.created_at DESC,r.run_id,t.round_id,t.ordinal LIMIT 100) AS recent;
    END IF;
    SET doc=JSON_SET(doc,'$.tool_calls',tool_summary);
  END IF;
  SELECT COALESCE(JSON_ARRAYAGG(JSON_OBJECT('id',o.object_id,'name',o.name,'schema_name',o.schema_name,'kind',d.kind,'version',d.target_version)),JSON_ARRAY())
    INTO deps FROM sdp_ai.dependencies d JOIN sdp_ai.objects o ON o.object_id=d.target_id
    WHERE d.object_id=p_id AND d.object_version=JSON_EXTRACT(doc,'$.version');
  SET deps=JSON_MERGE_PRESERVE(COALESCE(JSON_EXTRACT(doc,'$.dependencies'),JSON_ARRAY()),deps);
  SELECT COALESCE(JSON_ARRAYAGG(JSON_OBJECT('version',v.version,'state',IF(v.version=JSON_EXTRACT(doc,'$.version'),v.state,'previous'),'created_at',v.created_at)),JSON_ARRAY())
    INTO versions_doc FROM sdp_ai.object_versions v WHERE v.object_id=p_id;
  SELECT JSON_SET(doc,'$.dependencies',deps,'$.versions',versions_doc) AS document;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.check_behaviors$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.check_behaviors(IN p_bindings JSON,OUT p_normalized JSON,OUT p_instructions LONGTEXT)
SQL SECURITY DEFINER
BEGIN
  DECLARE i INT DEFAULT 0; DECLARE item_doc JSON; DECLARE seen_doc JSON DEFAULT JSON_OBJECT();
  DECLARE id_value CHAR(32); DECLARE version_value BIGINT UNSIGNED; DECLARE priority_value INT;
  DECLARE name_value VARCHAR(64); DECLARE definition_value LONGTEXT; DECLARE done BOOLEAN DEFAULT FALSE;
  DECLARE bindings_cursor CURSOR FOR SELECT b.id,b.version,b.priority FROM JSON_TABLE(p_bindings,'$[*]' COLUMNS(
    id VARCHAR(32) PATH '$.id',version BIGINT UNSIGNED PATH '$.version',priority INT PATH '$.priority')) AS b ORDER BY b.priority,b.id;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done=TRUE;
  SET p_bindings=COALESCE(p_bindings,JSON_ARRAY()),p_normalized=JSON_ARRAY(),p_instructions='';
  IF sdp_ai.json_kind(p_bindings)<>'ARRAY' OR JSON_LENGTH(p_bindings)>64 THEN CALL sdp_ai.raise_error('behavior_conflict'); END IF;
  WHILE i<JSON_LENGTH(p_bindings) DO
    SET item_doc=JSON_EXTRACT(p_bindings,CONCAT('$[',i,']'));
    IF sdp_ai.json_kind(item_doc)<>'OBJECT' OR JSON_LENGTH(item_doc)<>3 OR
      sdp_ai.json_kind(JSON_EXTRACT(item_doc,'$.id'))<>'STRING' OR NOT REGEXP_LIKE(JSON_UNQUOTE(JSON_EXTRACT(item_doc,'$.id')),'^[0-9a-f]{32}$','c') OR
      sdp_ai.json_kind(JSON_EXTRACT(item_doc,'$.version'))<>'INTEGER' OR JSON_EXTRACT(item_doc,'$.version')<1 OR
      sdp_ai.json_kind(JSON_EXTRACT(item_doc,'$.priority'))<>'INTEGER' OR JSON_EXTRACT(item_doc,'$.priority') NOT BETWEEN -2147483648 AND 2147483647 THEN CALL sdp_ai.raise_error('behavior_conflict'); END IF;
    SET id_value=JSON_UNQUOTE(JSON_EXTRACT(item_doc,'$.id'));
    IF JSON_CONTAINS_PATH(seen_doc,'one',CONCAT('$.',JSON_QUOTE(id_value))) THEN CALL sdp_ai.raise_error('behavior_conflict'); END IF;
    SET seen_doc=JSON_SET(seen_doc,CONCAT('$.',JSON_QUOTE(id_value)),TRUE),i=i+1;
  END WHILE;
  OPEN bindings_cursor;
  bindings_loop: LOOP
    FETCH bindings_cursor INTO id_value,version_value,priority_value;
    IF done THEN LEAVE bindings_loop; END IF;
    SET name_value=NULL,definition_value=NULL;
    SELECT name INTO name_value FROM sdp_ai.objects WHERE object_id=id_value AND kind='behavior' AND state='active' FOR SHARE;
    IF name_value IS NULL OR NOT sdp_ai.can_use(id_value,'USE') THEN CALL sdp_ai.raise_error('behavior_not_found'); END IF;
    SELECT JSON_UNQUOTE(JSON_EXTRACT(document,'$.definition')) INTO definition_value FROM sdp_ai.object_versions WHERE object_id=id_value AND version=version_value AND state='active' FOR SHARE;
    IF definition_value IS NULL THEN CALL sdp_ai.raise_error('behavior_not_found'); END IF;
    SET p_normalized=JSON_ARRAY_APPEND(p_normalized,'$',JSON_OBJECT('id',id_value,'version',version_value,'priority',priority_value));
    SET p_instructions=CONCAT(p_instructions,IF(p_instructions='','',CHAR(10)),'[Behavior ',name_value,' - v',version_value,' - priority ',priority_value,']',CHAR(10),definition_value);
    IF OCTET_LENGTH(p_instructions)>65536 THEN CALL sdp_ai.raise_error('context_budget'); END IF;
  END LOOP;
  CLOSE bindings_cursor;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.apply_behaviors$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.apply_behaviors(IN p_conversation CHAR(32),IN p_bindings JSON)
SQL SECURITY DEFINER
BEGIN
  DECLARE doc JSON; DECLARE next_version BIGINT UNSIGNED; DECLARE i INT DEFAULT 0; DECLARE item_doc JSON;
  DECLARE normalized_doc JSON; DECLARE instructions_value LONGTEXT;
  IF STARDATAPARADISE_TRANSACTION_ACTIVE()<>1 THEN CALL sdp_ai.raise_error('pending_changes'); END IF;
  IF NOT EXISTS(SELECT 1 FROM sdp_ai.objects WHERE object_id=p_conversation AND kind='conversation' AND state='active' AND owner=STARDATAPARADISE_ACTOR()) THEN CALL sdp_ai.raise_error('conversation_not_found'); END IF;
  CALL sdp_ai.check_behaviors(p_bindings,normalized_doc,instructions_value);
  SET p_bindings=normalized_doc;
  SELECT document,version+1 INTO doc,next_version FROM sdp_ai.objects WHERE object_id=p_conversation AND kind='conversation' FOR UPDATE;
  SET doc=JSON_SET(doc,'$.version',next_version,'$.settings.behaviors',p_bindings);
  UPDATE sdp_ai.objects SET version=next_version,document=doc,updated_at=UTC_TIMESTAMP(6) WHERE object_id=p_conversation;
  INSERT INTO sdp_ai.object_versions(object_id,version,document,state) VALUES(p_conversation,next_version,doc,'active');
  INSERT INTO sdp_ai.dependencies(object_id,object_version,target_id,target_version,kind)
    SELECT object_id,next_version,target_id,target_version,kind FROM sdp_ai.dependencies WHERE object_id=p_conversation AND object_version=next_version-1 AND kind<>'behavior';
  WHILE i<JSON_LENGTH(p_bindings) DO
    SET item_doc=JSON_EXTRACT(p_bindings,CONCAT('$[',i,']'));
    INSERT INTO sdp_ai.dependencies VALUES(p_conversation,next_version,JSON_UNQUOTE(JSON_EXTRACT(item_doc,'$.id')),JSON_EXTRACT(item_doc,'$.version'),'behavior');
    SET i=i+1;
  END WHILE;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.behavior_binding$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.behavior_binding(IN p_conversation CHAR(32),IN p_bindings JSON,IN p_payload JSON,IN p_add BOOLEAN,OUT p_normalized JSON)
SQL SECURITY DEFINER
BEGIN
  DECLARE conversation_value CHAR(32); DECLARE behavior_value CHAR(32); DECLARE schema_value VARCHAR(64); DECLARE name_value VARCHAR(64);
  DECLARE version_value BIGINT UNSIGNED; DECLARE priority_value INT DEFAULT 100; DECLARE slot_value INT; DECLARE bindings_doc JSON; DECLARE normalized_doc JSON; DECLARE instructions_value LONGTEXT;
  DECLARE keys_doc JSON;DECLARE key_value VARCHAR(128);DECLARE index_value INT DEFAULT 0;
  IF sdp_ai.json_kind(p_payload)<>'OBJECT' THEN CALL sdp_ai.raise_error('option_type');END IF;
  SET keys_doc=JSON_KEYS(p_payload);
  WHILE index_value<JSON_LENGTH(keys_doc) DO
    SET key_value=JSON_UNQUOTE(JSON_EXTRACT(keys_doc,CONCAT('$[',index_value,']')));
    IF key_value NOT IN ('behavior_schema','behavior_name','version','priority') THEN CALL sdp_ai.raise_error('unknown_option');END IF;
    SET index_value=index_value+1;
  END WHILE;
  SET conversation_value=p_conversation,bindings_doc=p_bindings;
  IF NOT EXISTS(SELECT 1 FROM sdp_ai.objects WHERE object_id=conversation_value AND kind='conversation' AND state='active' AND owner=STARDATAPARADISE_ACTOR()) THEN CALL sdp_ai.raise_error('conversation_not_found'); END IF;
  SELECT schema_name INTO schema_value FROM sdp_ai.objects WHERE object_id=conversation_value;
  IF sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.behavior_schema'))='STRING' THEN SET schema_value=JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.behavior_schema')); END IF;
  IF sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.behavior_name'))<>'STRING' THEN CALL sdp_ai.raise_error('option_type'); END IF;
  SET name_value=JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.behavior_name'));
  SELECT MAX(object_id),MAX(version) INTO behavior_value,version_value FROM sdp_ai.objects WHERE kind='behavior' AND schema_name=schema_value AND name=name_value AND state='active';
  IF behavior_value IS NULL THEN CALL sdp_ai.raise_error('behavior_not_found'); END IF;
  IF JSON_CONTAINS_PATH(p_payload,'one','$.version') THEN
    IF sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.version'))<>'INTEGER' OR JSON_EXTRACT(p_payload,'$.version')<1 THEN CALL sdp_ai.raise_error('behavior_conflict'); END IF;
    SET version_value=JSON_EXTRACT(p_payload,'$.version');
  END IF;
  IF JSON_CONTAINS_PATH(p_payload,'one','$.priority') THEN
    IF sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.priority'))<>'INTEGER' OR JSON_EXTRACT(p_payload,'$.priority') NOT BETWEEN -2147483648 AND 2147483647 THEN CALL sdp_ai.raise_error('behavior_conflict'); END IF;
    SET priority_value=JSON_EXTRACT(p_payload,'$.priority');
  END IF;
  SELECT MAX(b.ordinal) INTO slot_value FROM JSON_TABLE(bindings_doc,'$[*]' COLUMNS(ordinal FOR ORDINALITY,id VARCHAR(32) PATH '$.id')) AS b WHERE b.id=behavior_value;
  IF p_add THEN
    IF slot_value IS NULL THEN SET bindings_doc=JSON_ARRAY_APPEND(bindings_doc,'$',JSON_OBJECT('id',behavior_value,'version',version_value,'priority',priority_value));
    ELSE SET bindings_doc=JSON_SET(bindings_doc,CONCAT('$[',slot_value-1,']'),JSON_OBJECT('id',behavior_value,'version',version_value,'priority',priority_value)); END IF;
  ELSEIF slot_value IS NOT NULL THEN SET bindings_doc=JSON_REMOVE(bindings_doc,CONCAT('$[',slot_value-1,']')); END IF;
  CALL sdp_ai.check_behaviors(bindings_doc,p_normalized,instructions_value);
END$$

DROP PROCEDURE IF EXISTS sdp_ai.stage_behavior$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.stage_behavior(IN p_payload JSON,IN p_add BOOLEAN)
SQL SECURITY DEFINER
BEGIN
  DECLARE conversation_value CHAR(32);DECLARE bindings_doc JSON;DECLARE normalized_doc JSON;
  IF @sdp_ai_session IS NULL THEN CALL sdp_ai.raise_error('no_active_conversation');END IF;
  SET conversation_value=JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.conversation_id'));
  SELECT COALESCE(JSON_EXTRACT(@sdp_ai_session,'$.behaviors_pending'),JSON_EXTRACT(document,'$.settings.behaviors'),JSON_ARRAY())
    INTO bindings_doc FROM sdp_ai.objects WHERE object_id=conversation_value;
  CALL sdp_ai.behavior_binding(conversation_value,bindings_doc,p_payload,p_add,normalized_doc);
  SET @sdp_ai_session=JSON_SET(@sdp_ai_session,'$.behaviors_pending',normalized_doc);
  SELECT JSON_OBJECT('conversation_id',conversation_value,'behaviors',normalized_doc,'staged',TRUE) AS document;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.named_behavior$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.named_behavior(IN p_schema VARCHAR(64),IN p_name VARCHAR(64),IN p_payload JSON,IN p_add BOOLEAN)
SQL SECURITY DEFINER
BEGIN
  DECLARE conversation_value CHAR(32);DECLARE bindings_doc JSON;DECLARE normalized_doc JSON;DECLARE revision_value BIGINT UNSIGNED;
  DECLARE owned_transaction BOOLEAN DEFAULT FALSE;
  DECLARE EXIT HANDLER FOR SQLEXCEPTION BEGIN IF owned_transaction THEN ROLLBACK;END IF;RESIGNAL;END;
  -- A named metadata command owns a short transaction. An active conversation
  -- target is staged instead; neither path implicitly commits caller writes.
  IF STARDATAPARADISE_TRANSACTION_ACTIVE()<>0 OR sdp_ai.has_pending_turn() THEN CALL sdp_ai.raise_error('pending_changes');END IF;
  START TRANSACTION;SET owned_transaction=TRUE;
  SELECT MAX(object_id) INTO conversation_value FROM sdp_ai.objects WHERE kind='conversation' AND schema_name=p_schema AND name=p_name
    AND state='active' AND owner=STARDATAPARADISE_ACTOR();
  IF conversation_value IS NULL THEN CALL sdp_ai.raise_error('conversation_not_found');END IF;
  SELECT revision INTO revision_value FROM sdp_ai.conversations WHERE conversation_id=conversation_value FOR UPDATE;
  IF EXISTS(SELECT 1 FROM sdp_ai.runs WHERE conversation_id=conversation_value AND state IN ('queued','running')) THEN CALL sdp_ai.raise_error('pending_changes');END IF;
  SELECT COALESCE(JSON_EXTRACT(document,'$.settings.behaviors'),JSON_ARRAY()) INTO bindings_doc FROM sdp_ai.objects WHERE object_id=conversation_value FOR UPDATE;
  CALL sdp_ai.behavior_binding(conversation_value,bindings_doc,p_payload,p_add,normalized_doc);
  CALL sdp_ai.apply_behaviors(conversation_value,normalized_doc);
  UPDATE sdp_ai.conversations SET revision=revision+1 WHERE conversation_id=conversation_value;
  COMMIT;SET owned_transaction=FALSE;
  SELECT JSON_OBJECT('conversation_id',conversation_value,'behaviors',normalized_doc,'revision',revision_value+1,'staged',FALSE) AS document;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.check_capture$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.check_capture(IN p_capture JSON,IN p_format VARCHAR(8),OUT p_normalized JSON)
SQL SECURITY DEFINER
capture_block: BEGIN
  DECLARE keys_doc JSON; DECLARE target_doc JSON; DECLARE k VARCHAR(128); DECLARE name_value VARCHAR(256); DECLARE previous_name VARCHAR(256); DECLARE kind_value VARCHAR(32); DECLARE previous_kind VARCHAR(32); DECLARE i INT DEFAULT 0;
  SET p_normalized=JSON_OBJECT();
  IF p_capture IS NULL THEN LEAVE capture_block; END IF;
  IF sdp_ai.json_kind(p_capture)<>'OBJECT' OR p_format NOT IN ('TEXT','JSON') THEN CALL sdp_ai.raise_error('capture_type'); END IF;
  SET keys_doc=JSON_KEYS(p_capture);
  WHILE i<JSON_LENGTH(keys_doc) DO
    SET k=JSON_UNQUOTE(JSON_EXTRACT(keys_doc,CONCAT('$[',i,']')));
    IF k NOT IN ('format','value_target','metadata_target') THEN CALL sdp_ai.raise_error('unknown_option'); END IF;
    SET i=i+1;
  END WHILE;
  IF JSON_CONTAINS_PATH(p_capture,'one','$.format') AND (sdp_ai.json_kind(JSON_EXTRACT(p_capture,'$.format'))<>'STRING' OR JSON_UNQUOTE(JSON_EXTRACT(p_capture,'$.format'))<>p_format) THEN CALL sdp_ai.raise_error('option_conflict'); END IF;
  SET i=0;
  WHILE i<2 DO
    SET k=IF(i=0,'value_target','metadata_target'),target_doc=JSON_EXTRACT(p_capture,CONCAT('$.',k));
    IF sdp_ai.json_kind(target_doc) NOT IN ('MISSING','NULL') THEN
      IF sdp_ai.json_kind(target_doc)<>'OBJECT' OR JSON_LENGTH(target_doc)<>2 OR
        sdp_ai.json_kind(JSON_EXTRACT(target_doc,'$.kind'))<>'STRING' OR JSON_UNQUOTE(JSON_EXTRACT(target_doc,'$.kind')) NOT IN ('user_variable','local_variable') OR
        sdp_ai.json_kind(JSON_EXTRACT(target_doc,'$.name'))<>'STRING' THEN CALL sdp_ai.raise_error('capture_type'); END IF;
      SET name_value=JSON_UNQUOTE(JSON_EXTRACT(target_doc,'$.name')),kind_value=JSON_UNQUOTE(JSON_EXTRACT(target_doc,'$.kind'));
      IF CHAR_LENGTH(name_value) NOT BETWEEN 1 AND 64 OR LOCATE(X'00',CAST(name_value AS BINARY))>0 OR
        LEFT(LOWER(name_value),4)='sdp_' OR LEFT(LOWER(name_value),5)='__sdp' THEN CALL sdp_ai.raise_error('capture_type'); END IF;
      IF i=1 AND kind_value=previous_kind AND LOWER(name_value)=LOWER(previous_name) THEN CALL sdp_ai.raise_error('option_conflict'); END IF;
      SET previous_name=name_value,previous_kind=kind_value,p_normalized=JSON_SET(p_normalized,CONCAT('$.',k),JSON_OBJECT('kind',kind_value,'name',name_value));
    END IF;
    SET i=i+1;
  END WHILE;
  IF JSON_LENGTH(p_normalized)>0 THEN SET p_normalized=JSON_SET(p_normalized,'$.format',p_format); END IF;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.prepare_response$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.prepare_response(IN p_payload JSON,IN p_ready BOOLEAN)
SQL SECURITY DEFINER
BEGIN
  DECLARE options_doc JSON; DECLARE defaults_doc JSON; DECLARE model_caps JSON; DECLARE merged_doc JSON; DECLARE checked_doc JSON;
  DECLARE capture_doc JSON; DECLARE value_doc JSON; DECLARE provided_doc JSON; DECLARE fmt VARCHAR(8); DECLARE schema_value VARCHAR(64);
  IF @sdp_ai_session IS NULL OR NOT sdp_ai.can_use(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.conversation_id')),'APPEND') THEN CALL sdp_ai.raise_error('conversation_not_found'); END IF;
  IF sdp_ai.json_kind(p_payload)<>'OBJECT' THEN CALL sdp_ai.raise_error('option_type'); END IF;
  SELECT JSON_EXTRACT(o.document,'$.settings.response'),JSON_EXTRACT(v.document,'$.capabilities'),o.schema_name INTO defaults_doc,model_caps,schema_value
    FROM sdp_ai.conversations c JOIN sdp_ai.objects o ON o.object_id=c.conversation_id
    JOIN sdp_ai.object_versions v ON v.object_id=c.model_id AND v.version=c.model_version
    WHERE c.conversation_id=JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.conversation_id'));
  SET options_doc=COALESCE(JSON_EXTRACT(p_payload,'$.options'),JSON_OBJECT());
  IF sdp_ai.json_kind(options_doc)<>'OBJECT' THEN CALL sdp_ai.raise_error('option_type'); END IF;
  IF JSON_CONTAINS_PATH(p_payload,'one','$.format') AND sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.format'))<>'STRING' THEN CALL sdp_ai.raise_error('option_type'); END IF;
  SET merged_doc=JSON_MERGE_PATCH(COALESCE(defaults_doc,JSON_OBJECT()),COALESCE(JSON_EXTRACT(@sdp_ai_session,'$.response'),JSON_OBJECT()),options_doc);
  SET fmt=UPPER(COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.format')),JSON_UNQUOTE(JSON_EXTRACT(merged_doc,'$.format')),'TEXT'));
  IF fmt NOT IN ('TEXT','JSON') THEN CALL sdp_ai.raise_error('option_range'); END IF;
  IF JSON_CONTAINS_PATH(merged_doc,'one','$.schema') AND sdp_ai.json_kind(JSON_EXTRACT(merged_doc,'$.schema'))='STRING' AND LOCATE('.',JSON_UNQUOTE(JSON_EXTRACT(merged_doc,'$.schema')))=0 THEN
    SET merged_doc=JSON_SET(merged_doc,'$.schema',CONCAT(schema_value,'.',JSON_UNQUOTE(JSON_EXTRACT(merged_doc,'$.schema'))));
  END IF;
  CALL sdp_ai.validate_options(JSON_REMOVE(merged_doc,'$.format'),fmt,COALESCE(model_caps,JSON_ARRAY()),checked_doc);
  CALL sdp_ai.check_capture(COALESCE(JSON_EXTRACT(p_payload,'$.capture'),JSON_EXTRACT(@sdp_ai_session,'$.capture')),fmt,capture_doc);
  IF p_ready THEN
    SET provided_doc=JSON_EXTRACT(p_payload,'$.provided');
    IF sdp_ai.json_kind(provided_doc)<>'OBJECT' OR NOT JSON_CONTAINS_PATH(provided_doc,'one','$.value') OR
      sdp_ai.json_kind(JSON_EXTRACT(provided_doc,'$.source_sql_null'))<>'BOOLEAN' OR sdp_ai.json_kind(JSON_EXTRACT(provided_doc,'$.source_json'))<>'BOOLEAN' THEN CALL sdp_ai.raise_error('option_type'); END IF;
    IF JSON_UNQUOTE(JSON_EXTRACT(provided_doc,'$.source_sql_null'))='true' THEN CALL sdp_ai.raise_error('capture_type'); END IF;
    SET value_doc=JSON_EXTRACT(provided_doc,'$.value');
    IF fmt='JSON' AND sdp_ai.json_kind(value_doc)='STRING' AND JSON_UNQUOTE(JSON_EXTRACT(provided_doc,'$.source_json'))<>'true' THEN
      BEGIN
        DECLARE EXIT HANDLER FOR SQLSTATE '22032' CALL sdp_ai.raise_error('invalid_json');
        SET value_doc=CAST(JSON_UNQUOTE(value_doc) AS JSON);
      END;
    ELSEIF fmt='TEXT' THEN
      IF sdp_ai.json_kind(value_doc)='NULL' THEN CALL sdp_ai.raise_error('capture_type'); END IF;
      SET value_doc=CAST(JSON_QUOTE(IF(sdp_ai.json_kind(value_doc)='STRING',JSON_UNQUOTE(value_doc),CAST(value_doc AS CHAR))) AS JSON);
    END IF;
    SET provided_doc=JSON_OBJECT('format',fmt,'value',value_doc);
  END IF;
  SET @sdp_ai_session=JSON_SET(@sdp_ai_session,'$.response',JSON_SET(merged_doc,'$.format',fmt),'$.capture',capture_doc,
    '$.request_response',CAST(IF(p_ready,'false','true') AS JSON));
  IF p_ready THEN SET @sdp_ai_session=JSON_SET(@sdp_ai_session,'$.provided_response',provided_doc); END IF;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.dispatch$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.dispatch(IN p_operation VARCHAR(32),IN p_kind VARCHAR(32),
  IN p_schema VARCHAR(64),IN p_name VARCHAR(64),IN p_payload JSON,OUT p_output LONGTEXT)
SQL SECURITY DEFINER
dispatch_block: BEGIN
  DECLARE actor VARCHAR(576) DEFAULT STARDATAPARADISE_ACTOR();
  DECLARE id_value CHAR(32); DECLARE model_id_value CHAR(32); DECLARE model_version_value BIGINT UNSIGNED;
  DECLARE doc JSON; DECLARE settings JSON; DECLARE model_doc JSON; DECLARE value_doc JSON;
  DECLARE v_version BIGINT UNSIGNED DEFAULT 1; DECLARE v_state VARCHAR(24) DEFAULT 'active';
  DECLARE provider VARCHAR(32); DECLARE k VARCHAR(256); DECLARE keys_doc JSON; DECLARE i INT DEFAULT 0;
  DECLARE ticket_value CHAR(32); DECLARE rev BIGINT UNSIGNED; DECLARE ep BIGINT UNSIGNED;
  DECLARE job_value CHAR(32); DECLARE expected_sha VARCHAR(64); DECLARE source_path VARCHAR(1024);
  DECLARE owned_transaction BOOLEAN DEFAULT FALSE; DECLARE mutation_started BOOLEAN DEFAULT FALSE;
  DECLARE old_doc JSON;DECLARE response_doc JSON;DECLARE response_checked JSON;DECLARE response_format VARCHAR(8);
  DECLARE schema_target CHAR(32);DECLARE schema_target_version BIGINT UNSIGNED;DECLARE next_name VARCHAR(64);
  DECLARE EXIT HANDLER FOR SQLEXCEPTION
  BEGIN
    IF mutation_started THEN
      IF owned_transaction THEN ROLLBACK; ELSE ROLLBACK TO SAVEPOINT __sdp_ai_dispatch_v1_46ac5bf2; RELEASE SAVEPOINT __sdp_ai_dispatch_v1_46ac5bf2; END IF;
    END IF;
    IF p_operation IN ('commit_async','commit_sync') THEN SET @sdp_ai_commit_mode=NULL,@sdp_ai_wait_timeout_ms=NULL; END IF;
    RESIGNAL;
  END;
  SET p_output=NULL;
  IF p_kind IS NULL OR p_kind NOT IN ('model','conversation','behavior','response_schema','knowledge','run') THEN CALL sdp_ai.raise_error('invalid_dependency'); END IF;
  IF p_operation IS NULL OR p_operation NOT IN ('list','describe','drop','create','import','alter','rename','archive','unarchive','start','start_ticket',
    'append_prompt','append_context','append_response','append_ready','response_options','request_response','set_memory','bind_behavior','unbind_behavior','memory','flush','finish','describe_run','fetch_response','cancel_run','commit_async','commit_sync') THEN CALL sdp_ai.raise_error('unsupported_option'); END IF;
  IF (p_operation IN ('start','start_ticket','append_prompt','append_context','append_response','append_ready','response_options','request_response','set_memory','bind_behavior','unbind_behavior','memory','flush','finish','commit_async','commit_sync') AND p_kind<>'conversation') OR
     (p_operation IN ('describe_run','fetch_response','cancel_run') AND p_kind<>'run') OR (p_operation='import' AND p_kind<>'model') THEN CALL sdp_ai.raise_error('invalid_dependency'); END IF;
  IF p_operation IN ('rename','archive','unarchive') AND p_kind<>'conversation' THEN CALL sdp_ai.raise_error('invalid_dependency');END IF;
  IF p_operation IN ('commit_async','commit_sync') THEN
    IF @sdp_ai_session IS NULL THEN CALL sdp_ai.raise_error('no_active_conversation'); END IF;
    IF sdp_ai.json_kind(p_payload)<>'OBJECT' THEN CALL sdp_ai.raise_error('option_type'); END IF;
    SET keys_doc=JSON_KEYS(p_payload),i=0;
    WHILE i<JSON_LENGTH(keys_doc) DO
      SET k=JSON_UNQUOTE(JSON_EXTRACT(keys_doc,CONCAT('$[',i,']')));
      IF k NOT IN ('request_key','wait_timeout_ms') THEN CALL sdp_ai.raise_error('unknown_option'); END IF;
      SET i=i+1;
    END WHILE;
    IF JSON_CONTAINS_PATH(p_payload,'one','$.request_key') THEN
      IF sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.request_key'))<>'STRING' THEN CALL sdp_ai.raise_error('option_type'); END IF;
      IF NOT REGEXP_LIKE(JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.request_key')),'^[0-9a-fA-F]{32}$','c') THEN CALL sdp_ai.raise_error('option_range'); END IF;
      SET ticket_value=LOWER(JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.request_key')));
    END IF;
    IF JSON_CONTAINS_PATH(p_payload,'one','$.wait_timeout_ms') THEN
      IF sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.wait_timeout_ms'))<>'INTEGER' THEN CALL sdp_ai.raise_error('option_type'); END IF;
      IF JSON_EXTRACT(p_payload,'$.wait_timeout_ms') NOT BETWEEN 1 AND 3600000 THEN CALL sdp_ai.raise_error('option_range'); END IF;
      IF p_operation='commit_async' THEN CALL sdp_ai.raise_error('option_conflict'); END IF;
    END IF;
    IF JSON_LENGTH(JSON_EXTRACT(@sdp_ai_session,'$.pending'))=0 AND
      COALESCE(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.request_response')),'false')<>'true' AND
      COALESCE(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.flush')),'false')<>'true' AND
      COALESCE(JSON_LENGTH(JSON_EXTRACT(@sdp_ai_session,'$.memory_pending')),0)=0 AND
      COALESCE(JSON_LENGTH(JSON_EXTRACT(@sdp_ai_session,'$.temporary_pending')),0)=0 AND
      NOT JSON_CONTAINS_PATH(@sdp_ai_session,'one','$.provided_response','$.behaviors_pending') THEN CALL sdp_ai.raise_error('empty_context'); END IF;
    IF ticket_value IS NOT NULL THEN SET @sdp_ai_session=JSON_SET(@sdp_ai_session,'$.request_key',ticket_value); END IF;
    SET @sdp_ai_commit_mode=IF(p_operation='commit_async','async','sync'),
      @sdp_ai_wait_timeout_ms=IF(p_operation='commit_async',0,COALESCE(JSON_EXTRACT(p_payload,'$.wait_timeout_ms'),30000)),
      @sdp_ai_commit_result=NULL;
    COMMIT;
    IF p_operation='commit_sync' THEN
      IF @sdp_ai_commit_result IS NULL THEN CALL sdp_ai.raise_error('response_not_ready'); END IF;
      SET doc=CAST(@sdp_ai_commit_result AS JSON);
      SET p_output=IF(JSON_UNQUOTE(JSON_EXTRACT(doc,'$.result.format'))='JSON',CAST(JSON_EXTRACT(doc,'$.result.value') AS CHAR),
        IF(JSON_UNQUOTE(JSON_EXTRACT(doc,'$.result.format'))='TEXT',JSON_UNQUOTE(JSON_EXTRACT(doc,'$.result.value')),NULL));
      SET @sdp_ai_commit_mode=NULL,@sdp_ai_wait_timeout_ms=NULL;
      SELECT doc AS document;
    ELSEIF @sdp_ai_last_run_id IS NULL THEN
      SET @sdp_ai_commit_mode=NULL,@sdp_ai_wait_timeout_ms=NULL,p_output=NULL;
      SELECT JSON_OBJECT('run_id',NULL,'state','memory_committed','commit_state','committed','epoch',JSON_EXTRACT(@sdp_ai_session,'$.epoch')) AS document;
    ELSE
      SET @sdp_ai_commit_mode=NULL,@sdp_ai_wait_timeout_ms=NULL,p_output=@sdp_ai_last_run_id;
      SELECT JSON_OBJECT('run_id',r.run_id,'job_id',r.job_id,'state',r.state,'commit_state','committed','replayed',CAST(IF(COALESCE(@sdp_ai_last_replayed,FALSE),'true','false') AS JSON)) AS document
        FROM sdp_ai.runs r WHERE r.run_id=p_output AND r.owner=actor;
    END IF;
    LEAVE dispatch_block;
  END IF;
  IF p_operation='list' THEN
    SELECT object_id AS id,name,schema_name,version,state,owner,updated_at FROM sdp_ai.objects
      WHERE kind=p_kind AND state<>'retired' AND (owner=actor OR sdp_ai.can_use(object_id,CASE p_kind WHEN 'conversation' THEN 'SELECT' WHEN 'knowledge' THEN 'EXECUTE' ELSE 'USE' END)) ORDER BY schema_name,name;
    LEAVE dispatch_block;
  END IF;
  IF p_kind='run' THEN
    SET id_value=JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.value'));
    IF NOT EXISTS(SELECT 1 FROM sdp_ai.runs WHERE run_id=id_value AND owner=actor) THEN CALL sdp_ai.raise_error('conversation_not_found'); END IF;
    IF p_operation='cancel_run' THEN
      UPDATE sdp_jobs.compute_jobs SET cancel_requested=TRUE WHERE job_id=id_value AND job_kind='ai' AND owner=actor AND state IN ('queued','running');
      SELECT JSON_OBJECT('run_id',id_value,'cancel_requested',TRUE) AS document;
    ELSEIF p_operation='fetch_response' THEN
      SELECT state INTO v_state FROM sdp_ai.runs WHERE run_id=id_value AND owner=actor;
      IF v_state NOT IN ('succeeded','refused','failed','cancelled') THEN CALL sdp_ai.raise_error('response_not_ready'); END IF;
      SELECT IF(JSON_UNQUOTE(JSON_EXTRACT(response,'$.format'))='JSON',CAST(JSON_EXTRACT(response,'$.value') AS CHAR),
                IF(JSON_UNQUOTE(JSON_EXTRACT(response,'$.format'))='TEXT',JSON_UNQUOTE(JSON_EXTRACT(response,'$.value')),NULL))
        INTO p_output FROM sdp_ai.runs WHERE run_id=id_value AND owner=actor;
      SELECT JSON_OBJECT('run_id',run_id,'state',state,'result',response,'metadata',metadata,'error',error) AS document FROM sdp_ai.runs WHERE run_id=id_value AND owner=actor;
    ELSEIF p_operation='describe_run' THEN
      SET value_doc=JSON_ARRAY();
      IF EXISTS(SELECT 1 FROM sdp_ai.runs WHERE run_id=id_value AND owner=actor AND JSON_EXTRACT(execution_context,'$.has_tools')=TRUE) THEN
        SELECT COALESCE(JSON_ARRAYAGG(JSON_OBJECT('call_id',t.call_id,'state',t.state,'kind',JSON_EXTRACT(t.snapshot,'$.kind'),
          'schema_name',JSON_EXTRACT(t.snapshot,'$.schema_name'),'name',JSON_EXTRACT(t.snapshot,'$.name'),'action_id',t.action_id)),JSON_ARRAY())
          INTO value_doc FROM sdp_ai.tool_calls t WHERE t.run_id=id_value;
      END IF;
      SELECT JSON_OBJECT('run_id',run_id,'job_id',job_id,'state',state,'conversation_revision',conversation_revision,'epoch',epoch,'created_at',created_at,'finished_at',finished_at,'error',error,'tool_calls',value_doc) AS document
        FROM sdp_ai.runs WHERE run_id=id_value AND owner=actor;
    ELSE CALL sdp_ai.raise_error('unsupported_option'); END IF;
    LEAVE dispatch_block;
  END IF;
  IF p_operation IN ('bind_behavior','unbind_behavior') THEN
    IF p_name<>'' AND NOT EXISTS(SELECT 1 FROM sdp_ai.objects WHERE object_id=JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.conversation_id')) AND schema_name=p_schema AND name=p_name AND kind='conversation' AND state='active') THEN
      CALL sdp_ai.named_behavior(p_schema,p_name,p_payload,p_operation='bind_behavior');
    ELSE
      CALL sdp_ai.stage_behavior(p_payload,p_operation='bind_behavior');
    END IF;LEAVE dispatch_block;
  END IF;
  IF p_operation IN ('append_prompt','append_context','append_response','append_ready','response_options','request_response','set_memory','memory','flush','finish') THEN
    IF @sdp_ai_session IS NULL OR sdp_ai.json_kind(@sdp_ai_session)<>'OBJECT' THEN CALL sdp_ai.raise_error('no_active_conversation'); END IF;
    SET id_value=JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.conversation_id'));
    IF p_operation<>'finish' AND NOT sdp_ai.can_use(id_value,IF(p_operation='memory','SELECT','APPEND')) THEN CALL sdp_ai.raise_error('conversation_not_found'); END IF;
    IF p_operation IN ('append_prompt','append_context') THEN
      IF p_operation='append_prompt' THEN
        IF sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.value'))<>'STRING' OR OCTET_LENGTH(JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.value')))>1048576 THEN CALL sdp_ai.raise_error('context_budget'); END IF;
        SET @sdp_ai_session=JSON_ARRAY_APPEND(@sdp_ai_session,'$.pending',JSON_OBJECT('role','user','kind','prompt','content',JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.value'))));
      ELSE
        IF NOT JSON_CONTAINS_PATH(p_payload,'one','$.value') OR OCTET_LENGTH(CAST(JSON_EXTRACT(p_payload,'$.value') AS CHAR))>1048576 THEN CALL sdp_ai.raise_error('context_budget'); END IF;
        IF JSON_CONTAINS_PATH(p_payload,'one','$.name') AND (sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.name'))<>'STRING' OR
          CHAR_LENGTH(JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.name'))) NOT BETWEEN 1 AND 128 OR LOCATE(X'00',CAST(JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.name')) AS BINARY))>0) THEN CALL sdp_ai.raise_error('option_range'); END IF;
        SET k=COALESCE(JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.name')),'context');
        SET @sdp_ai_session=JSON_ARRAY_APPEND(@sdp_ai_session,'$.pending',JSON_OBJECT('role','user','kind','context','name',k,
          'content',CONCAT('[Structured context data - ',k,']',CHAR(10),CAST(JSON_EXTRACT(p_payload,'$.value') AS CHAR))));
      END IF;
    ELSEIF p_operation IN ('append_ready','response_options') THEN
      CALL sdp_ai.prepare_response(p_payload,p_operation='append_ready');
    ELSEIF p_operation='append_response' THEN
      SET value_doc=JSON_EXTRACT(p_payload,'$.value');
      IF sdp_ai.json_kind(value_doc)='STRING' THEN SET value_doc=CAST(JSON_UNQUOTE(value_doc) AS JSON); END IF;
      IF sdp_ai.json_kind(value_doc)<>'OBJECT' THEN CALL sdp_ai.raise_error('option_type'); END IF;
      SET @sdp_ai_session=JSON_SET(@sdp_ai_session,'$.response',value_doc,'$.request_response',TRUE);
    ELSEIF p_operation='request_response' THEN
      SET @sdp_ai_session=JSON_SET(@sdp_ai_session,'$.request_response',TRUE);
    ELSEIF p_operation='set_memory' THEN
      IF sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.name'))<>'STRING' OR sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.persistent'))<>'BOOLEAN' OR
        NOT JSON_CONTAINS_PATH(p_payload,'one','$.value') THEN CALL sdp_ai.raise_error('option_type'); END IF;
      SET k=JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.name'));
      IF CHAR_LENGTH(k) NOT BETWEEN 1 AND 128 OR LOCATE(X'00',CAST(k AS BINARY))>0 THEN CALL sdp_ai.raise_error('option_range'); END IF;
      IF JSON_EXTRACT(p_payload,'$.persistent')=TRUE THEN
        SET value_doc=sdp_ai.merge_memory(JSON_EXTRACT(@sdp_ai_session,'$.memory_pending'),JSON_OBJECT(k,JSON_EXTRACT(p_payload,'$.value')));
        SET @sdp_ai_session=JSON_SET(@sdp_ai_session,'$.memory_pending',value_doc);
      ELSE
        SET value_doc=sdp_ai.merge_memory(JSON_EXTRACT(@sdp_ai_session,'$.temporary_pending'),JSON_OBJECT(k,JSON_EXTRACT(p_payload,'$.value')));
        SET @sdp_ai_session=JSON_SET(@sdp_ai_session,'$.temporary_pending',value_doc);
      END IF;
    ELSEIF p_operation='flush' THEN
      SET @sdp_ai_session=JSON_SET(@sdp_ai_session,'$.flush',TRUE,'$.memory_pending',JSON_OBJECT(),'$.temporary_pending',JSON_OBJECT());
    ELSEIF p_operation='memory' THEN
      SELECT JSON_OBJECT('memory',sdp_ai.merge_memory(IF(JSON_EXTRACT(@sdp_ai_session,'$.flush')=TRUE,JSON_OBJECT(),memory),JSON_EXTRACT(@sdp_ai_session,'$.memory_pending')),
        'temporary',sdp_ai.merge_memory(IF(JSON_EXTRACT(@sdp_ai_session,'$.flush')=TRUE,JSON_OBJECT(),JSON_EXTRACT(@sdp_ai_session,'$.temporary_memory')),JSON_EXTRACT(@sdp_ai_session,'$.temporary_pending')),
        'epoch',epoch,'pending_flush',JSON_EXTRACT(@sdp_ai_session,'$.flush')) AS document FROM sdp_ai.conversations WHERE conversation_id=id_value;
    ELSEIF p_operation='finish' THEN
      IF sdp_ai.has_pending_turn() THEN CALL sdp_ai.raise_error('pending_changes'); END IF;
      SET @sdp_ai_session=NULL;
    END IF;
    LEAVE dispatch_block;
  END IF;
  IF p_operation='start_ticket' THEN
    SET id_value=JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.value'));
  ELSE
    IF p_schema IS NULL OR p_schema='' OR p_name IS NULL OR p_name='' OR CHAR_LENGTH(p_name)>64 OR CHAR_LENGTH(p_schema)>64 OR LOCATE(X'00',CAST(p_schema AS BINARY))>0 OR LOCATE(X'00',CAST(p_name AS BINARY))>0 THEN CALL sdp_ai.raise_error('invalid_dependency'); END IF;
    SELECT MAX(object_id) INTO id_value FROM sdp_ai.objects WHERE kind=p_kind AND schema_name=p_schema AND name=p_name AND state<>'retired';
  END IF;
  IF p_operation='describe' THEN CALL sdp_ai.object_document(id_value); LEAVE dispatch_block; END IF;
  IF p_operation IN ('start','start_ticket') THEN
    IF NOT sdp_ai.can_use(id_value,'APPEND') OR NOT EXISTS(SELECT 1 FROM sdp_ai.objects WHERE object_id=id_value AND kind='conversation' AND state='active') THEN CALL sdp_ai.raise_error('conversation_not_found'); END IF;
    IF @sdp_ai_session IS NOT NULL AND sdp_ai.has_pending_turn() THEN CALL sdp_ai.raise_error('pending_changes'); END IF;
    SELECT revision,epoch INTO rev,ep FROM sdp_ai.conversations WHERE conversation_id=id_value;
    SET @sdp_ai_session=JSON_OBJECT('conversation_id',id_value,'base_revision',rev,'epoch',ep,'request_key',REPLACE(UUID(),'-',''),
      'pending',JSON_ARRAY(),'instructions','','response',JSON_OBJECT(),'request_response',FALSE,'flush',FALSE,
      'memory_pending',JSON_OBJECT(),'temporary_pending',JSON_OBJECT(),'temporary_memory',JSON_OBJECT(),'capture',JSON_OBJECT());
    SET p_output=id_value;
    SELECT JSON_OBJECT('ticket',id_value,'revision',rev,'epoch',ep) AS document;
    LEAVE dispatch_block;
  END IF;
  IF p_operation IN ('create','import','alter','drop','rename','archive','unarchive') THEN
    IF STARDATAPARADISE_TRANSACTION_ACTIVE()<>1 THEN
      IF @sdp_ai_session IS NOT NULL OR STARDATAPARADISE_TRANSACTION_DIRTY()<>0 THEN CALL sdp_ai.raise_error('pending_changes'); END IF;
      START TRANSACTION; SET owned_transaction=TRUE;
    ELSE
      SAVEPOINT __sdp_ai_dispatch_v1_46ac5bf2;
    END IF;
    SET mutation_started=TRUE;
  END IF;
  IF p_kind='conversation' AND p_operation IN ('alter','rename','archive','unarchive','drop') THEN
    IF NOT EXISTS(SELECT 1 FROM sdp_ai.objects WHERE object_id=id_value AND owner=actor AND state<>'retired') THEN CALL sdp_ai.raise_error('conversation_not_found');END IF;
    -- Keep the same lock order as turn publication. A concurrent staged turn
    -- observes the revision change instead of losing messages or memory.
    SELECT revision INTO rev FROM sdp_ai.conversations WHERE conversation_id=id_value FOR UPDATE;
    SELECT document,version+1,state INTO old_doc,v_version,v_state FROM sdp_ai.objects WHERE object_id=id_value FOR UPDATE;
    IF EXISTS(SELECT 1 FROM sdp_ai.runs WHERE conversation_id=id_value AND state IN ('queued','running')) THEN CALL sdp_ai.raise_error('pending_changes');END IF;
    IF @sdp_ai_session IS NOT NULL AND JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.conversation_id'))=id_value AND sdp_ai.has_pending_turn() THEN CALL sdp_ai.raise_error('pending_changes');END IF;
  END IF;
  IF p_operation IN ('rename','archive','unarchive') THEN
    IF sdp_ai.json_kind(p_payload)<>'OBJECT' THEN CALL sdp_ai.raise_error('option_type');END IF;
    SET doc=old_doc,next_name=p_name;
    IF p_operation='rename' THEN
      IF JSON_LENGTH(p_payload)<>1 OR sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.name'))<>'STRING' THEN CALL sdp_ai.raise_error('option_type');END IF;
      SET next_name=JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.name'));
      IF CHAR_LENGTH(next_name) NOT BETWEEN 1 AND 64 OR LOCATE(X'00',CAST(next_name AS BINARY))>0 THEN CALL sdp_ai.raise_error('option_range');END IF;
      IF EXISTS(SELECT 1 FROM sdp_ai.objects WHERE kind='conversation' AND schema_name=p_schema AND name=next_name AND state<>'retired' AND object_id<>id_value) THEN CALL sdp_ai.raise_error('revision_conflict');END IF;
    ELSE
      IF JSON_LENGTH(p_payload)<>0 THEN CALL sdp_ai.raise_error('unknown_option');END IF;
      SET v_state=IF(p_operation='archive','archived','active');
    END IF;
    SET doc=JSON_SET(doc,'$.version',v_version,'$.name',next_name,'$.state',v_state);
    UPDATE sdp_ai.objects SET name=next_name,state=v_state,version=v_version,document=doc,updated_at=UTC_TIMESTAMP(6) WHERE object_id=id_value;
    INSERT INTO sdp_ai.object_versions VALUES(id_value,v_version,doc,v_state,UTC_TIMESTAMP(6));
    INSERT INTO sdp_ai.dependencies SELECT object_id,v_version,target_id,target_version,kind FROM sdp_ai.dependencies WHERE object_id=id_value AND object_version=v_version-1;
    UPDATE sdp_ai.conversations SET revision=revision+1 WHERE conversation_id=id_value;
    SET p_output=id_value;
    IF owned_transaction THEN COMMIT;ELSE RELEASE SAVEPOINT __sdp_ai_dispatch_v1_46ac5bf2;END IF;
    SET mutation_started=FALSE;SELECT doc AS document;LEAVE dispatch_block;
  END IF;
  IF p_operation='drop' THEN
    IF NOT EXISTS(SELECT 1 FROM sdp_ai.objects WHERE object_id=id_value AND owner=actor) THEN CALL sdp_ai.raise_error('invalid_dependency'); END IF;
    IF EXISTS(SELECT 1 FROM sdp_ai.dependencies d JOIN sdp_ai.objects o ON o.object_id=d.object_id AND o.version=d.object_version WHERE d.target_id=id_value AND o.state IN ('active','archived')) THEN CALL sdp_ai.raise_error('invalid_dependency'); END IF;
    IF EXISTS(SELECT 1 FROM sdp_ai.runs WHERE (conversation_id=id_value OR JSON_UNQUOTE(JSON_EXTRACT(model,'$.id'))=id_value) AND state IN ('queued','running')) THEN CALL sdp_ai.raise_error('pending_changes'); END IF;
    UPDATE sdp_ai.objects SET state='retired',updated_at=UTC_TIMESTAMP(6),document=JSON_SET(document,'$.state','retired') WHERE object_id=id_value;
    IF owned_transaction THEN COMMIT; ELSE RELEASE SAVEPOINT __sdp_ai_dispatch_v1_46ac5bf2; END IF;
    SET mutation_started=FALSE;
    SELECT JSON_OBJECT('id',id_value,'state','retired') AS document;
    LEAVE dispatch_block;
  END IF;
  IF p_operation NOT IN ('create','import','alter') THEN CALL sdp_ai.raise_error('unsupported_option'); END IF;
  IF id_value IS NOT NULL THEN
    IF p_operation='create' OR NOT EXISTS(SELECT 1 FROM sdp_ai.objects WHERE object_id=id_value AND owner=actor) THEN CALL sdp_ai.raise_error('revision_conflict'); END IF;
    SELECT version+1 INTO v_version FROM sdp_ai.objects WHERE object_id=id_value FOR UPDATE;
  ELSE
    IF p_operation='alter' THEN CALL sdp_ai.raise_error('invalid_dependency'); END IF;
    SET id_value=REPLACE(UUID(),'-','');
  END IF;
  IF sdp_ai.json_kind(p_payload)<>'OBJECT' THEN CALL sdp_ai.raise_error('option_type'); END IF;
  SET doc=JSON_OBJECT('id',id_value,'kind',p_kind,'name',p_name,'schema_name',p_schema,'version',v_version,'state','active','owner',actor,
      'parameters',JSON_ARRAY(),'result_columns',JSON_ARRAY(),'dependencies',JSON_ARRAY(),'versions',JSON_ARRAY(),'settings',JSON_OBJECT());
  IF p_kind='model' THEN
    SET settings=COALESCE(JSON_EXTRACT(p_payload,'$.settings'),JSON_OBJECT());
    IF sdp_ai.json_kind(settings)<>'OBJECT' THEN CALL sdp_ai.raise_error('option_type'); END IF;
    SET keys_doc=JSON_KEYS(settings),i=0;
    WHILE i<JSON_LENGTH(keys_doc) DO
      SET k=JSON_UNQUOTE(JSON_EXTRACT(keys_doc,CONCAT('$[',i,']')));
      IF k NOT IN ('endpoint','secret_env','credential_ref','runtime','sha256','context_tokens','max_output_tokens','capabilities') THEN CALL sdp_ai.raise_error('unknown_option'); END IF;
      SET i=i+1;
    END WHILE;
    IF p_operation='import' THEN
      SET provider='local_gguf',v_state='pending_import',expected_sha=LOWER(JSON_UNQUOTE(JSON_EXTRACT(settings,'$.sha256'))),source_path=JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.artifact_source'));
      IF expected_sha IS NULL OR NOT REGEXP_LIKE(expected_sha,'^[0-9a-f]{64}$','c') OR source_path IS NULL OR CHAR_LENGTH(source_path)>1024 OR
        source_path LIKE '/%' OR LOCATE(':',source_path)>0 OR LOCATE(CHAR(92),source_path)>0 OR LOCATE(X'00',CAST(source_path AS BINARY))>0 OR
        REGEXP_LIKE(source_path,'(^|/)[.][.](/|$)','c') OR LOWER(RIGHT(source_path,5))<>'.gguf' THEN CALL sdp_ai.raise_error('model_integrity'); END IF;
      IF JSON_UNQUOTE(JSON_EXTRACT(settings,'$.runtime'))<>'llama.cpp' THEN CALL sdp_ai.raise_error('model_capability'); END IF;
      IF JSON_CONTAINS_PATH(settings,'one','$.context_tokens') AND (sdp_ai.json_kind(JSON_EXTRACT(settings,'$.context_tokens'))<>'INTEGER' OR JSON_EXTRACT(settings,'$.context_tokens') NOT BETWEEN 512 AND 131072) THEN CALL sdp_ai.raise_error('option_range'); END IF;
      SET settings=JSON_SET(settings,'$.artifact_id',expected_sha,'$.context_tokens',CAST(COALESCE(JSON_EXTRACT(settings,'$.context_tokens'),4096) AS UNSIGNED),'$.runtime_state','pending_import');
      SET doc=JSON_SET(doc,'$.remote_model',expected_sha);
    ELSE
      SET provider=LOWER(JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.provider')));
      IF provider IS NULL OR provider NOT IN ('openai','anthropic') THEN CALL sdp_ai.raise_error('model_capability'); END IF;
      IF sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.remote_model'))<>'STRING' OR CHAR_LENGTH(JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.remote_model'))) NOT BETWEEN 1 AND 256 THEN CALL sdp_ai.raise_error('option_type'); END IF;
      IF (JSON_CONTAINS_PATH(settings,'one','$.secret_env')+JSON_CONTAINS_PATH(settings,'one','$.credential_ref'))<>1 THEN CALL sdp_ai.raise_error('provider_auth'); END IF;
      IF JSON_CONTAINS_PATH(settings,'one','$.secret_env') AND NOT REGEXP_LIKE(JSON_UNQUOTE(JSON_EXTRACT(settings,'$.secret_env')),'^[A-Za-z_][A-Za-z0-9_]{0,127}$','c') THEN CALL sdp_ai.raise_error('provider_auth'); END IF;
      IF JSON_CONTAINS_PATH(settings,'one','$.credential_ref') AND (NOT REGEXP_LIKE(JSON_UNQUOTE(JSON_EXTRACT(settings,'$.credential_ref')),'^[A-Za-z0-9_.-]{1,128}$','c') OR LOWER(LEFT(JSON_UNQUOTE(JSON_EXTRACT(settings,'$.credential_ref')),3))='sk-') THEN CALL sdp_ai.raise_error('provider_auth'); END IF;
      IF JSON_CONTAINS_PATH(settings,'one','$.endpoint') AND (sdp_ai.json_kind(JSON_EXTRACT(settings,'$.endpoint'))<>'STRING' OR NOT REGEXP_LIKE(JSON_UNQUOTE(JSON_EXTRACT(settings,'$.endpoint')),'^https://[^/?#@]+(/[^?#]*)?$','c')) THEN CALL sdp_ai.raise_error('unsafe_endpoint'); END IF;
      SET doc=JSON_SET(doc,'$.remote_model',JSON_EXTRACT(p_payload,'$.remote_model'));
    END IF;
    IF JSON_CONTAINS_PATH(settings,'one','$.max_output_tokens') AND (sdp_ai.json_kind(JSON_EXTRACT(settings,'$.max_output_tokens'))<>'INTEGER' OR JSON_EXTRACT(settings,'$.max_output_tokens') NOT BETWEEN 1 AND 131072) THEN CALL sdp_ai.raise_error('option_range'); END IF;
    SET value_doc=COALESCE(JSON_EXTRACT(settings,'$.capabilities'),IF(provider='local_gguf',JSON_ARRAY('temperature','top_p','seed','stop'),JSON_ARRAY()));
    IF sdp_ai.json_kind(value_doc)<>'ARRAY' OR JSON_LENGTH(value_doc)>5 THEN CALL sdp_ai.raise_error('option_type'); END IF;
    SET keys_doc=JSON_OBJECT(),i=0;
    WHILE i<JSON_LENGTH(value_doc) DO
      IF sdp_ai.json_kind(JSON_EXTRACT(value_doc,CONCAT('$[',i,']')))<>'STRING' OR CHAR_LENGTH(JSON_UNQUOTE(JSON_EXTRACT(value_doc,CONCAT('$[',i,']')))) NOT BETWEEN 1 AND 32 THEN CALL sdp_ai.raise_error('option_type'); END IF;
      SET k=JSON_UNQUOTE(JSON_EXTRACT(value_doc,CONCAT('$[',i,']')));
      IF (provider='openai' AND k NOT IN ('temperature','top_p','tools')) OR
         (provider='anthropic' AND k NOT IN ('stop','tools')) OR
         (provider='local_gguf' AND k NOT IN ('temperature','top_p','seed','stop','tools')) THEN CALL sdp_ai.raise_error('model_capability'); END IF;
      IF JSON_CONTAINS_PATH(keys_doc,'one',CONCAT('$.',JSON_QUOTE(k))) THEN CALL sdp_ai.raise_error('duplicate_option'); END IF;
      SET keys_doc=JSON_SET(keys_doc,CONCAT('$.',JSON_QUOTE(k)),TRUE),i=i+1;
    END WHILE;
    SET doc=JSON_SET(doc,'$.provider',provider,'$.settings',settings,'$.capabilities',value_doc);
  ELSEIF p_kind='behavior' THEN
    IF sdp_ai.json_kind(JSON_EXTRACT(p_payload,'$.definition'))<>'STRING' OR OCTET_LENGTH(JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.definition'))) NOT BETWEEN 1 AND 65536 THEN CALL sdp_ai.raise_error('option_range'); END IF;
    SET doc=JSON_SET(doc,'$.definition',JSON_EXTRACT(p_payload,'$.definition'));
  ELSEIF p_kind='response_schema' THEN
    SET value_doc=JSON_EXTRACT(p_payload,'$.schema');
    IF sdp_ai.json_kind(value_doc)='STRING' THEN SET value_doc=CAST(JSON_UNQUOTE(value_doc) AS JSON); END IF;
    CALL sdp_ai.check_response_schema(value_doc);
    IF CAST(JSON_UNQUOTE(JSON_EXTRACT(p_payload,'$.version')) AS UNSIGNED)<>v_version THEN CALL sdp_ai.raise_error('option_range'); END IF;
    SET doc=JSON_SET(doc,'$.definition',value_doc);
  ELSEIF p_kind='conversation' THEN
    SET keys_doc=JSON_KEYS(p_payload),i=0;
    WHILE i<JSON_LENGTH(keys_doc) DO
      SET k=JSON_UNQUOTE(JSON_EXTRACT(keys_doc,CONCAT('$[',i,']')));
      IF k NOT IN ('functions','tools','auto_append','persistent','model_schema','model_name','response') THEN CALL sdp_ai.raise_error('unknown_option');END IF;
      SET i=i+1;
    END WHILE;
    IF p_operation='alter' THEN
      IF v_state<>'active' THEN CALL sdp_ai.raise_error('conversation_not_found');END IF;
      SET settings=JSON_MERGE_PATCH(JSON_EXTRACT(old_doc,'$.settings'),p_payload);
      SELECT model_id,model_version INTO model_id_value,model_version_value FROM sdp_ai.conversations WHERE conversation_id=id_value;
    ELSE SET settings=p_payload;END IF;
    SET settings=JSON_SET(settings,'$.functions',CAST(IF(sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.functions'),FALSE),'true','false') AS JSON),
      '$.tools',COALESCE(JSON_EXTRACT(settings,'$.tools'),JSON_ARRAY()));
    IF sdp_ai.json_kind(JSON_EXTRACT(settings,'$.tools'))<>'ARRAY' THEN CALL sdp_ai.raise_error('tool_arguments');END IF;
    IF NOT sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.functions'),FALSE) AND JSON_LENGTH(JSON_EXTRACT(settings,'$.tools'))>0 THEN CALL sdp_ai.raise_error('tool_denied');END IF;
    IF p_operation<>'alter' OR JSON_CONTAINS_PATH(p_payload,'one','$.tools') THEN
      SET settings=JSON_SET(settings,'$.tool_bindings',CAST(STARDATAPARADISE_TOOLS_VALIDATE(JSON_EXTRACT(settings,'$.tools')) AS JSON));
    END IF;
    IF p_operation<>'alter' OR JSON_CONTAINS_PATH(p_payload,'one','$.model_schema','$.model_name') THEN
      IF p_operation='alter' AND JSON_CONTAINS_PATH(p_payload,'all','$.model_schema','$.model_name')<>1 THEN CALL sdp_ai.raise_error('option_conflict');END IF;
      SELECT MAX(object_id),MAX(version) INTO model_id_value,model_version_value FROM sdp_ai.objects
        WHERE kind='model' AND schema_name=JSON_UNQUOTE(JSON_EXTRACT(settings,'$.model_schema')) AND name=JSON_UNQUOTE(JSON_EXTRACT(settings,'$.model_name')) AND state='active';
    END IF;
    IF NOT sdp_ai.can_use(model_id_value,'USE') THEN CALL sdp_ai.raise_error('model_unavailable'); END IF;
    SET response_doc=COALESCE(JSON_EXTRACT(settings,'$.response'),JSON_OBJECT());
    IF sdp_ai.json_kind(response_doc)<>'OBJECT' THEN CALL sdp_ai.raise_error('option_type');END IF;
    SET response_format=UPPER(COALESCE(JSON_UNQUOTE(JSON_EXTRACT(response_doc,'$.format')),'TEXT'));
    IF response_format NOT IN ('TEXT','JSON') THEN CALL sdp_ai.raise_error('option_range');END IF;
    SELECT document INTO model_doc FROM sdp_ai.object_versions WHERE object_id=model_id_value AND version=model_version_value;
    IF JSON_LENGTH(JSON_EXTRACT(settings,'$.tool_bindings'))>0 AND NOT JSON_CONTAINS(JSON_EXTRACT(model_doc,'$.capabilities'),JSON_QUOTE('tools')) THEN CALL sdp_ai.raise_error('model_capability');END IF;
    CALL sdp_ai.validate_options(JSON_REMOVE(response_doc,'$.format'),response_format,JSON_EXTRACT(model_doc,'$.capabilities'),response_checked);
    IF JSON_CONTAINS_PATH(response_doc,'one','$.schema') THEN
      SET k=JSON_UNQUOTE(JSON_EXTRACT(response_doc,'$.schema'));
      IF LOCATE('.',k)=0 THEN SET k=CONCAT(p_schema,'.',k);END IF;
      SET response_doc=JSON_SET(response_doc,'$.schema',k),schema_target_version=JSON_EXTRACT(response_doc,'$.schema_version');
      SELECT MAX(object_id) INTO schema_target FROM sdp_ai.objects WHERE kind='response_schema' AND CONCAT(schema_name,'.',name)=k AND state='active';
      IF NOT sdp_ai.can_use(schema_target,'USE') OR NOT EXISTS(SELECT 1 FROM sdp_ai.object_versions WHERE object_id=schema_target AND version=schema_target_version AND state='active') THEN CALL sdp_ai.raise_error('invalid_dependency');END IF;
    END IF;
    SET doc=JSON_SET(doc,'$.model_id',model_id_value,'$.model_version',model_version_value,'$.settings',
      JSON_SET(settings,'$.auto_append',CAST(IF(sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.auto_append'),FALSE),'true','false') AS JSON),
        '$.persistent',CAST(IF(sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.persistent'),TRUE),'true','false') AS JSON),
        '$.behaviors',COALESCE(JSON_EXTRACT(settings,'$.behaviors'),JSON_ARRAY()),'$.response',response_doc));
  ELSEIF p_kind='knowledge' THEN
    -- Direct JSON callers are untrusted. The native compiler itself validates
    -- the typed INVOKER program and returns canonical metadata; a client flag
    -- cannot assert readonly safety or smuggle a DEFINER routine into storage.
    SET value_doc=CAST(STARDATAPARADISE_KNOWLEDGE_VALIDATE(JSON_SET(p_payload,'$.schema_name',p_schema)) AS JSON);
    SET doc=JSON_MERGE_PATCH(doc,value_doc);
  END IF;
  SET doc=JSON_SET(doc,'$.state',v_state);
  IF v_version=1 THEN
    INSERT INTO sdp_ai.objects(object_id,kind,schema_name,name,owner,state,document) VALUES(id_value,p_kind,p_schema,p_name,actor,v_state,doc);
  ELSE
    UPDATE sdp_ai.objects SET version=v_version,state=v_state,document=doc,updated_at=UTC_TIMESTAMP(6) WHERE object_id=id_value;
  END IF;
  INSERT INTO sdp_ai.object_versions(object_id,version,document,state) VALUES(id_value,v_version,doc,v_state);
  IF p_kind='conversation' THEN
    IF v_version=1 THEN
      INSERT INTO sdp_ai.conversations(conversation_id,model_id,model_version,messages,memory,functions_enabled,auto_append,persistent_memory)
        VALUES(id_value,model_id_value,model_version_value,JSON_ARRAY(),JSON_OBJECT(),sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.functions'),FALSE),sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.auto_append'),FALSE),sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.persistent'),TRUE));
    ELSE
      UPDATE sdp_ai.conversations SET model_id=model_id_value,model_version=model_version_value,revision=revision+1,
        functions_enabled=sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.functions'),FALSE),
        auto_append=sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.auto_append'),FALSE),persistent_memory=sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.persistent'),TRUE) WHERE conversation_id=id_value;
      INSERT INTO sdp_ai.dependencies SELECT object_id,v_version,target_id,target_version,kind FROM sdp_ai.dependencies WHERE object_id=id_value AND object_version=v_version-1 AND kind NOT IN ('model','response_schema');
    END IF;
    INSERT INTO sdp_ai.dependencies VALUES(id_value,v_version,model_id_value,model_version_value,'model');
    IF schema_target IS NOT NULL THEN INSERT INTO sdp_ai.dependencies VALUES(id_value,v_version,schema_target,schema_target_version,'response_schema');END IF;
    SET @ticket=id_value;
  ELSEIF p_operation='import' THEN
    SET job_value=REPLACE(UUID(),'-','');
    INSERT INTO sdp_ai.model_imports(job_id,model_id,version,owner,relative_source,sha256,context_tokens)
      VALUES(job_value,id_value,v_version,actor,source_path,expected_sha,JSON_EXTRACT(settings,'$.context_tokens'));
    INSERT INTO sdp_jobs.compute_jobs(job_id,owner,request_token,iterations,timeout_ms,job_kind,payload,events)
      VALUES(job_value,actor,job_value,0,600000,'ai_import',JSON_OBJECT('model_id',id_value,'version',v_version),JSON_ARRAY(JSON_OBJECT('state','queued','at',UNIX_TIMESTAMP())));
  END IF;
  SET p_output=id_value;
  IF owned_transaction THEN COMMIT; ELSE RELEASE SAVEPOINT __sdp_ai_dispatch_v1_46ac5bf2; END IF;
  SET mutation_started=FALSE;
  SELECT doc AS document;
END$$

DROP FUNCTION IF EXISTS sdp_ai.knowledge_value$$
CREATE DEFINER='sdp_ai_internal'@'localhost' FUNCTION sdp_ai.knowledge_value(p_schema VARCHAR(64),p_name VARCHAR(64)) RETURNS JSON
READS SQL DATA SQL SECURITY DEFINER
BEGIN
  DECLARE id_value CHAR(32);DECLARE doc JSON;
  SELECT MAX(object_id) INTO id_value FROM sdp_ai.objects WHERE kind='knowledge' AND schema_name=p_schema AND name=p_name AND state='active';
  IF NOT sdp_ai.can_use(id_value,'EXECUTE') THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  SELECT document INTO doc FROM sdp_ai.objects WHERE object_id=id_value AND kind='knowledge' AND state='active' FOR SHARE;
  RETURN doc;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.knowledge_document$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.knowledge_document(IN p_schema VARCHAR(64),IN p_name VARCHAR(64))
SQL SECURITY DEFINER
BEGIN
  DECLARE id_value CHAR(32);
  SELECT MAX(object_id) INTO id_value FROM sdp_ai.objects
    WHERE kind='knowledge' AND schema_name=p_schema AND name=p_name AND state='active';
  IF NOT sdp_ai.can_use(id_value,'EXECUTE') THEN CALL sdp_ai.raise_error('tool_denied'); END IF;
  SELECT document FROM sdp_ai.objects WHERE object_id=id_value AND kind='knowledge' AND state='active';
END$$

DROP PROCEDURE IF EXISTS sdp_ai.before_commit$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.before_commit() SQL SECURITY DEFINER
turn_block: BEGIN
  DECLARE actor VARCHAR(576) DEFAULT STARDATAPARADISE_ACTOR(); DECLARE id_value CHAR(32); DECLARE key_value CHAR(32);
  DECLARE base_revision BIGINT UNSIGNED; DECLARE revision_value BIGINT UNSIGNED; DECLARE epoch_value BIGINT UNSIGNED;
  DECLARE model_id_value CHAR(32); DECLARE model_version_value BIGINT UNSIGNED; DECLARE model_doc JSON;
  DECLARE messages_doc JSON; DECLARE pending_doc JSON; DECLARE response_doc JSON; DECLARE options_doc JSON; DECLARE schema_doc JSON;
  DECLARE format_value VARCHAR(8) DEFAULT 'TEXT'; DECLARE schema_name_value VARCHAR(256); DECLARE schema_version_value BIGINT UNSIGNED;
  DECLARE run_value CHAR(32); DECLARE instructions_value LONGTEXT; DECLARE flush_value BOOLEAN; DECLARE request_value BOOLEAN;
  DECLARE pending_index INT DEFAULT 0; DECLARE pending_item JSON; DECLARE pending_prompts INT DEFAULT 0; DECLARE pending_messages JSON DEFAULT JSON_ARRAY();
  DECLARE fingerprint_value CHAR(64); DECLARE existing_run CHAR(32); DECLARE existing_fingerprint CHAR(64);
  DECLARE memory_doc JSON; DECLARE temporary_doc JSON; DECLARE effective_memory_doc JSON; DECLARE provider_messages_doc JSON; DECLARE memory_changed BOOLEAN;
  DECLARE provided_doc JSON; DECLARE capture_doc JSON; DECLARE defaults_doc JSON; DECLARE provided_value BOOLEAN;
  DECLARE auto_value BOOLEAN; DECLARE local_metadata JSON; DECLARE content_value LONGTEXT;
  DECLARE tools_enabled BOOLEAN;DECLARE tool_requests JSON;DECLARE tools_doc JSON;DECLARE pinned_tools JSON;DECLARE principals_doc JSON;
  DECLARE behaviors_doc JSON; DECLARE normalized_behaviors JSON; DECLARE behavior_instructions LONGTEXT; DECLARE behaviors_changed BOOLEAN;
  SET @sdp_ai_commit_receipt=NULL;
  IF @sdp_ai_session IS NULL THEN LEAVE turn_block; END IF;
  IF STARDATAPARADISE_TRANSACTION_ACTIVE()<>1 THEN CALL sdp_ai.raise_error('pending_changes'); END IF;
  IF sdp_ai.json_kind(@sdp_ai_session)<>'OBJECT' THEN CALL sdp_ai.raise_error('no_active_conversation'); END IF;
  SET id_value=JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.conversation_id')),key_value=JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.request_key'));
  SET pending_doc=JSON_EXTRACT(@sdp_ai_session,'$.pending'),response_doc=COALESCE(JSON_EXTRACT(@sdp_ai_session,'$.response'),JSON_OBJECT()),flush_value=COALESCE(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.flush'))='true',FALSE);
  SET request_value=COALESCE(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.request_response'))='true',FALSE);
  SET provided_doc=JSON_EXTRACT(@sdp_ai_session,'$.provided_response'),provided_value=JSON_CONTAINS_PATH(@sdp_ai_session,'one','$.provided_response');
  IF provided_value AND (sdp_ai.json_kind(provided_doc)<>'OBJECT' OR NOT JSON_CONTAINS_PATH(provided_doc,'one','$.value') OR
      sdp_ai.json_kind(JSON_EXTRACT(provided_doc,'$.format'))<>'STRING') THEN CALL sdp_ai.raise_error('option_type'); END IF;
  IF sdp_ai.json_kind(pending_doc)<>'ARRAY' OR JSON_LENGTH(pending_doc)>100 OR OCTET_LENGTH(CAST(pending_doc AS CHAR))>4194304 OR key_value IS NULL OR NOT REGEXP_LIKE(key_value,'^[0-9a-f]{32}$','c') THEN CALL sdp_ai.raise_error('context_budget'); END IF;
  IF sdp_ai.json_kind(JSON_EXTRACT(@sdp_ai_session,'$.base_revision'))<>'INTEGER' OR sdp_ai.json_kind(JSON_EXTRACT(@sdp_ai_session,'$.epoch'))<>'INTEGER' OR
    sdp_ai.json_kind(JSON_EXTRACT(@sdp_ai_session,'$.instructions'))<>'STRING' OR sdp_ai.json_kind(JSON_EXTRACT(@sdp_ai_session,'$.flush'))<>'BOOLEAN' THEN CALL sdp_ai.raise_error('option_type'); END IF;
  WHILE pending_index<JSON_LENGTH(pending_doc) DO
    SET pending_item=JSON_EXTRACT(pending_doc,CONCAT('$[',pending_index,']'));
    IF sdp_ai.json_kind(pending_item)<>'OBJECT' OR sdp_ai.json_kind(JSON_EXTRACT(pending_item,'$.role'))<>'STRING' OR
      JSON_UNQUOTE(JSON_EXTRACT(pending_item,'$.role'))<>'user' OR sdp_ai.json_kind(JSON_EXTRACT(pending_item,'$.content'))<>'STRING' THEN CALL sdp_ai.raise_error('option_type'); END IF;
    IF JSON_CONTAINS_PATH(pending_item,'one','$.kind') AND (sdp_ai.json_kind(JSON_EXTRACT(pending_item,'$.kind'))<>'STRING' OR
      JSON_UNQUOTE(JSON_EXTRACT(pending_item,'$.kind')) NOT IN ('prompt','context')) THEN CALL sdp_ai.raise_error('option_type'); END IF;
    IF COALESCE(JSON_UNQUOTE(JSON_EXTRACT(pending_item,'$.kind')),'prompt')='prompt' THEN SET pending_prompts=pending_prompts+1;END IF;
    SET pending_messages=JSON_ARRAY_APPEND(pending_messages,'$',JSON_OBJECT('role','user','content',JSON_EXTRACT(pending_item,'$.content')));
    SET pending_index=pending_index+1;
  END WHILE;
  SET memory_changed=COALESCE(JSON_LENGTH(JSON_EXTRACT(@sdp_ai_session,'$.memory_pending')),0)>0 OR
    COALESCE(JSON_LENGTH(JSON_EXTRACT(@sdp_ai_session,'$.temporary_pending')),0)>0;
  SET behaviors_changed=JSON_CONTAINS_PATH(@sdp_ai_session,'one','$.behaviors_pending');
  IF JSON_LENGTH(pending_doc)=0 AND NOT flush_value AND NOT request_value AND NOT memory_changed AND NOT provided_value AND NOT behaviors_changed THEN LEAVE turn_block; END IF;
  IF NOT EXISTS(SELECT 1 FROM sdp_ai.objects WHERE object_id=id_value AND kind='conversation' AND state='active') OR NOT sdp_ai.can_use(id_value,'APPEND') THEN CALL sdp_ai.raise_error('conversation_not_found'); END IF;
  SELECT revision,epoch,model_id,model_version,messages,memory,auto_append INTO revision_value,epoch_value,model_id_value,model_version_value,messages_doc,memory_doc,auto_value
    FROM sdp_ai.conversations WHERE conversation_id=id_value FOR UPDATE;
  SELECT JSON_EXTRACT(document,'$.settings.response'),COALESCE(JSON_EXTRACT(@sdp_ai_session,'$.behaviors_pending'),JSON_EXTRACT(document,'$.settings.behaviors'),JSON_ARRAY())
    INTO defaults_doc,behaviors_doc FROM sdp_ai.objects WHERE object_id=id_value;
  CALL sdp_ai.check_behaviors(behaviors_doc,normalized_behaviors,behavior_instructions);
  SELECT functions_enabled INTO tools_enabled FROM sdp_ai.conversations WHERE conversation_id=id_value;
  SELECT JSON_EXTRACT(document,'$.settings.tools'),JSON_EXTRACT(document,'$.settings.tool_bindings') INTO tool_requests,pinned_tools FROM sdp_ai.objects WHERE object_id=id_value;
  IF tools_enabled THEN
    SET tools_doc=CAST(STARDATAPARADISE_TOOLS_VALIDATE(tool_requests) AS JSON);
    IF NOT(tools_doc<=>pinned_tools) THEN CALL sdp_ai.raise_error('approval_stale');END IF;
  ELSE SET tools_doc=JSON_ARRAY();END IF;
  SET principals_doc=STARDATAPARADISE_PRINCIPALS();
  IF NOT(JSON_UNQUOTE(JSON_EXTRACT(principals_doc,'$.actor'))<=>actor) THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  SET response_doc=JSON_MERGE_PATCH(COALESCE(defaults_doc,JSON_OBJECT()),response_doc);
  SET format_value=COALESCE(JSON_UNQUOTE(JSON_EXTRACT(response_doc,'$.format')),'TEXT');
  CALL sdp_ai.check_capture(JSON_EXTRACT(@sdp_ai_session,'$.capture'),format_value,capture_doc);
  IF JSON_LENGTH(capture_doc)>0 AND @sdp_ai_commit_mode='async' THEN CALL sdp_ai.raise_error('option_conflict'); END IF;
  IF provided_value THEN
    IF JSON_UNQUOTE(JSON_EXTRACT(provided_doc,'$.format'))<>format_value OR
      (format_value='TEXT' AND sdp_ai.json_kind(JSON_EXTRACT(provided_doc,'$.value'))<>'STRING') THEN CALL sdp_ai.raise_error('capture_type'); END IF;
    SET request_value=FALSE;
  ELSEIF pending_prompts>0 AND auto_value THEN
    SET request_value=TRUE;
  END IF;
  SET fingerprint_value=SHA2(CAST(JSON_OBJECT('conversation_id',id_value,'pending',pending_doc,'response',response_doc,
    'instructions',JSON_EXTRACT(@sdp_ai_session,'$.instructions'),'flush',flush_value,'request_response',request_value,
    'memory_pending',JSON_EXTRACT(@sdp_ai_session,'$.memory_pending'),'temporary_pending',JSON_EXTRACT(@sdp_ai_session,'$.temporary_pending'),
    'provided_response',provided_doc,'behaviors',normalized_behaviors,'tools',tools_doc,'execution_context',principals_doc,'time_zone',@@session.time_zone) AS CHAR),256);
  SELECT run_id,request_fingerprint INTO existing_run,existing_fingerprint FROM sdp_ai.runs WHERE owner=actor AND request_key=key_value FOR UPDATE;
  IF existing_run IS NOT NULL THEN
    IF existing_fingerprint<>fingerprint_value THEN CALL sdp_ai.raise_error('request_key_conflict'); END IF;
    SET @sdp_ai_commit_receipt=JSON_OBJECT('revision',revision_value,'epoch',epoch_value,'run_id',existing_run,'replayed',TRUE,'capture',capture_doc,
      'temporary_memory',sdp_ai.merge_memory(JSON_EXTRACT(@sdp_ai_session,'$.temporary_memory'),JSON_EXTRACT(@sdp_ai_session,'$.temporary_pending')));
    LEAVE turn_block;
  END IF;
  IF revision_value<>JSON_EXTRACT(@sdp_ai_session,'$.base_revision') THEN CALL sdp_ai.raise_error('revision_conflict'); END IF;
  IF epoch_value<>JSON_EXTRACT(@sdp_ai_session,'$.epoch') THEN CALL sdp_ai.raise_error('memory_epoch_changed'); END IF;
  IF EXISTS(SELECT 1 FROM sdp_ai.runs WHERE conversation_id=id_value AND state IN ('queued','running')) AND (NOT flush_value OR JSON_LENGTH(pending_doc)>0 OR request_value) THEN CALL sdp_ai.raise_error('pending_changes'); END IF;
  IF flush_value THEN
    SET epoch_value=epoch_value+1,messages_doc=JSON_ARRAY(),memory_doc=JSON_OBJECT();
    -- Clear active provider context. Immutable run snapshots remain the audit
    -- history; an older turn must not leak back into a post-FLUSH request.
    UPDATE sdp_ai.conversations SET memory=JSON_OBJECT(),messages=JSON_ARRAY(),epoch=epoch_value WHERE conversation_id=id_value;
    UPDATE sdp_jobs.compute_jobs q JOIN sdp_ai.runs r ON r.job_id=q.job_id SET q.cancel_requested=TRUE
      WHERE r.conversation_id=id_value AND r.state IN ('queued','running') AND q.job_kind='ai';
  END IF;
  SET memory_doc=sdp_ai.merge_memory(memory_doc,JSON_EXTRACT(@sdp_ai_session,'$.memory_pending'));
  SET temporary_doc=sdp_ai.merge_memory(IF(flush_value,JSON_OBJECT(),JSON_EXTRACT(@sdp_ai_session,'$.temporary_memory')),JSON_EXTRACT(@sdp_ai_session,'$.temporary_pending'));
  SET effective_memory_doc=sdp_ai.merge_memory(memory_doc,temporary_doc);
  IF memory_changed THEN
    UPDATE sdp_ai.conversations SET memory=memory_doc WHERE conversation_id=id_value;
  END IF;
  IF behaviors_changed THEN CALL sdp_ai.apply_behaviors(id_value,normalized_behaviors); END IF;
  IF NOT request_value AND NOT provided_value THEN
    IF JSON_LENGTH(pending_doc)>0 THEN
      SET messages_doc=JSON_MERGE_PRESERVE(messages_doc,pending_messages);
      UPDATE sdp_ai.conversations SET messages=messages_doc WHERE conversation_id=id_value;
    END IF;
    IF memory_changed OR JSON_LENGTH(pending_doc)>0 OR behaviors_changed THEN
      SET revision_value=revision_value+1;
      UPDATE sdp_ai.conversations SET revision=revision_value WHERE conversation_id=id_value;
    END IF;
    SET @sdp_ai_commit_receipt=JSON_OBJECT('revision',revision_value,'epoch',epoch_value,'temporary_memory',temporary_doc,'capture',capture_doc); LEAVE turn_block;
  END IF;
  IF NOT sdp_ai.can_use(model_id_value,'USE') OR NOT EXISTS(SELECT 1 FROM sdp_ai.object_versions WHERE object_id=model_id_value AND state='active' AND version=model_version_value) THEN CALL sdp_ai.raise_error('model_unavailable'); END IF;
  SELECT document INTO model_doc FROM sdp_ai.object_versions WHERE object_id=model_id_value AND version=model_version_value;
  SET model_doc=JSON_MERGE_PATCH(model_doc,JSON_EXTRACT(model_doc,'$.settings'));
  SET options_doc=IF(JSON_CONTAINS_PATH(model_doc,'one','$.max_output_tokens'),JSON_OBJECT('max_output_tokens',JSON_EXTRACT(model_doc,'$.max_output_tokens')),JSON_OBJECT());
  SET options_doc=JSON_MERGE_PATCH(options_doc,JSON_REMOVE(response_doc,'$.format'));
  SET format_value=COALESCE(JSON_UNQUOTE(JSON_EXTRACT(response_doc,'$.format')),'TEXT');
  IF format_value NOT IN ('TEXT','JSON') THEN CALL sdp_ai.raise_error('option_range'); END IF;
  CALL sdp_ai.validate_options(options_doc,format_value,JSON_EXTRACT(model_doc,'$.capabilities'),options_doc);
  IF JSON_CONTAINS_PATH(options_doc,'one','$.schema') THEN
    SET schema_name_value=JSON_UNQUOTE(JSON_EXTRACT(options_doc,'$.schema')),schema_version_value=JSON_EXTRACT(options_doc,'$.schema_version');
    SELECT MAX(object_id) INTO run_value FROM sdp_ai.objects WHERE kind='response_schema' AND CONCAT(schema_name,'.',name)=schema_name_value AND state='active';
    IF NOT sdp_ai.can_use(run_value,'USE') THEN CALL sdp_ai.raise_error('invalid_dependency'); END IF;
    SELECT JSON_EXTRACT(document,'$.definition') INTO schema_doc FROM sdp_ai.object_versions WHERE object_id=run_value AND version=schema_version_value;
    IF schema_doc IS NULL THEN CALL sdp_ai.raise_error('invalid_dependency'); END IF;
    CALL sdp_ai.check_response_schema(schema_doc);
  END IF;
  IF provided_value THEN
    IF format_value='JSON' AND schema_doc IS NOT NULL THEN CALL sdp_ai.validate_response(JSON_EXTRACT(provided_doc,'$.value'),schema_doc); END IF;
    SET content_value=IF(format_value='JSON',CAST(JSON_EXTRACT(provided_doc,'$.value') AS CHAR),JSON_UNQUOTE(JSON_EXTRACT(provided_doc,'$.value')));
    IF OCTET_LENGTH(content_value)>JSON_EXTRACT(options_doc,'$.max_output_bytes') THEN CALL sdp_ai.raise_error('truncated_response'); END IF;
  END IF;
  SET messages_doc=JSON_MERGE_PRESERVE(messages_doc,pending_messages),revision_value=revision_value+1,run_value=REPLACE(UUID(),'-','');
  SET provider_messages_doc=messages_doc;
  IF JSON_LENGTH(memory_doc)>0 OR JSON_LENGTH(temporary_doc)>0 THEN
    SET provider_messages_doc=JSON_ARRAY_INSERT(provider_messages_doc,'$[0]',JSON_OBJECT('role','user',
      'content',CONCAT('[Structured conversation memory data]',CHAR(10),CAST(JSON_OBJECT('persistent',memory_doc,'temporary',temporary_doc,'effective',effective_memory_doc) AS CHAR))));
  END IF;
  SET instructions_value=CONCAT(COALESCE(behavior_instructions,''),IF(COALESCE(behavior_instructions,'')='','',CHAR(10)),COALESCE(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.instructions')),''));
  IF JSON_LENGTH(messages_doc)=0 AND OCTET_LENGTH(instructions_value)=0 AND NOT provided_value THEN CALL sdp_ai.raise_error('empty_context'); END IF;
  IF OCTET_LENGTH(instructions_value)>1048576 OR OCTET_LENGTH(CAST(messages_doc AS CHAR))>8388608 THEN CALL sdp_ai.raise_error('context_budget'); END IF;
  INSERT INTO sdp_ai.runs(run_id,job_id,owner,conversation_id,conversation_revision,epoch,request_key,request_fingerprint,model,messages,instructions,options,response_format,response_schema,execution_context)
    VALUES(run_value,run_value,actor,id_value,revision_value,epoch_value,key_value,fingerprint_value,model_doc,provider_messages_doc,instructions_value,options_doc,format_value,schema_doc,JSON_OBJECT('actor',actor,'roles',JSON_EXTRACT(principals_doc,'$.roles'),'time_zone',@@session.time_zone,'has_tools',CAST(IF(tools_enabled AND JSON_LENGTH(tools_doc)>0 AND NOT provided_value,'true','false') AS JSON)));
  IF tools_enabled AND JSON_LENGTH(tools_doc)>0 AND NOT provided_value THEN INSERT INTO sdp_ai.run_tools(run_id,tools) VALUES(run_value,tools_doc);END IF;
  IF provided_value THEN
    SET local_metadata=JSON_OBJECT('source','application','format',format_value);
    UPDATE sdp_ai.runs SET state='succeeded',response=provided_doc,metadata=local_metadata,
      publication_hash=SHA2(CONCAT(CAST(provided_doc AS CHAR),'|',CAST(local_metadata AS CHAR),'|null'),256),finished_at=UTC_TIMESTAMP(6) WHERE run_id=run_value;
    SET messages_doc=JSON_ARRAY_APPEND(messages_doc,'$',JSON_OBJECT('role','assistant','content',content_value));
  END IF;
  INSERT INTO sdp_jobs.compute_jobs(job_id,owner,request_token,iterations,timeout_ms,job_kind,payload,state,events)
    VALUES(run_value,actor,key_value,0,JSON_EXTRACT(options_doc,'$.run_timeout_ms'),'ai',JSON_OBJECT('run_id',run_value),IF(provided_value,'succeeded','queued'),JSON_ARRAY(JSON_OBJECT('state',IF(provided_value,'succeeded','queued'),'at',UNIX_TIMESTAMP())));
  UPDATE sdp_ai.conversations SET revision=revision_value,messages=messages_doc WHERE conversation_id=id_value;
  INSERT INTO sdp_ai.outbox(owner,aggregate_id,event_kind,document) VALUES(actor,run_value,IF(provided_value,'run_published','run_queued'),JSON_OBJECT('run_id',run_value,'revision',revision_value,'epoch',epoch_value));
  SET @sdp_ai_commit_receipt=JSON_OBJECT('revision',revision_value,'epoch',epoch_value,'run_id',run_value,'temporary_memory',temporary_doc,'capture',capture_doc);
END$$

DROP PROCEDURE IF EXISTS sdp_ai.after_commit$$
DROP FUNCTION IF EXISTS sdp_ai.run_receipt$$
CREATE DEFINER='sdp_ai_internal'@'localhost' FUNCTION sdp_ai.run_receipt(p_run CHAR(32)) RETURNS JSON
READS SQL DATA SQL SECURITY DEFINER
BEGIN
  DECLARE doc JSON;
  SELECT JSON_OBJECT('run_id',run_id,'job_id',job_id,'state',state,'commit_state','committed',
    'conversation_revision',conversation_revision,'epoch',epoch,'result',response,'metadata',metadata,'error',error)
    INTO doc FROM sdp_ai.runs WHERE run_id=p_run AND owner=STARDATAPARADISE_ACTOR();
  IF doc IS NULL THEN CALL sdp_ai.raise_error('conversation_not_found'); END IF;
  RETURN doc;
END$$

CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.after_commit() SQL SECURITY DEFINER
BEGIN
  DECLARE receipt JSON; DECLARE doc JSON; DECLARE state_value VARCHAR(24);
  SET receipt=@sdp_ai_commit_receipt;
  IF @sdp_ai_session IS NOT NULL AND @sdp_ai_commit_receipt IS NOT NULL THEN
    SET @sdp_ai_last_run_id=NULLIF(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_commit_receipt,'$.run_id')),'null');
    SET @sdp_ai_last_replayed=COALESCE(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_commit_receipt,'$.replayed'))='true',FALSE);
    SET @sdp_ai_session=JSON_SET(@sdp_ai_session,'$.base_revision',JSON_EXTRACT(@sdp_ai_commit_receipt,'$.revision'),
      '$.epoch',JSON_EXTRACT(@sdp_ai_commit_receipt,'$.epoch'),'$.pending',JSON_ARRAY(),'$.flush',FALSE,'$.request_response',FALSE,'$.instructions','','$.response',JSON_OBJECT(),'$.request_key',REPLACE(UUID(),'-',''));
    SET @sdp_ai_session=JSON_SET(JSON_REMOVE(@sdp_ai_session,'$.provided_response','$.behaviors_pending'),'$.capture',JSON_OBJECT(),'$.memory_pending',JSON_OBJECT(),'$.temporary_pending',JSON_OBJECT(),
      '$.temporary_memory',COALESCE(JSON_EXTRACT(@sdp_ai_commit_receipt,'$.temporary_memory'),JSON_OBJECT()));
  END IF;
  SET @sdp_ai_commit_receipt=NULL;
  IF receipt IS NOT NULL THEN
    SET @sdp_ai_commit_result=NULL;
    SET @sdp_ai_committed_capture=COALESCE(JSON_EXTRACT(receipt,'$.capture'),JSON_OBJECT());
    SET @sdp_ai_committed_run_id=NULLIF(JSON_UNQUOTE(JSON_EXTRACT(receipt,'$.run_id')),'null');
    IF @sdp_ai_committed_run_id IS NOT NULL THEN
      SET doc=JSON_SET(sdp_ai.run_receipt(@sdp_ai_committed_run_id),'$.capture',CAST(@sdp_ai_committed_capture AS JSON)),state_value=JSON_UNQUOTE(JSON_EXTRACT(doc,'$.state'));
    ELSE
      SET state_value='memory_committed',doc=JSON_OBJECT('run_id',NULL,'state','memory_committed','commit_state','committed','epoch',JSON_EXTRACT(receipt,'$.epoch'));
    END IF;
    SELECT CAST(@sdp_ai_committed_run_id AS CHAR) AS run_id,
      CAST(COALESCE(@sdp_ai_commit_mode,'sync') AS CHAR) AS mode,
      CAST(COALESCE(@sdp_ai_wait_timeout_ms,30000) AS CHAR) AS wait_ms,
      CAST(state_value AS CHAR) AS state,CAST(doc AS CHAR) AS document;
  END IF;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.poll_commit$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.poll_commit() SQL SECURITY DEFINER
BEGIN
  DECLARE doc JSON;
  SET doc=JSON_SET(sdp_ai.run_receipt(@sdp_ai_committed_run_id),'$.capture',CAST(COALESCE(@sdp_ai_committed_capture,'{}') AS JSON));
  SELECT CAST(@sdp_ai_committed_run_id AS CHAR) AS run_id,
    CAST(COALESCE(@sdp_ai_commit_mode,'sync') AS CHAR) AS mode,
    CAST(COALESCE(@sdp_ai_wait_timeout_ms,30000) AS CHAR) AS wait_ms,
    CAST(JSON_UNQUOTE(JSON_EXTRACT(doc,'$.state')) AS CHAR) AS state,CAST(doc AS CHAR) AS document;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.rollback_turn$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.rollback_turn() SQL SECURITY DEFINER
BEGIN
  IF @sdp_ai_session IS NOT NULL THEN
    SET @sdp_ai_session=JSON_SET(@sdp_ai_session,'$.pending',JSON_ARRAY(),'$.flush',FALSE,'$.request_response',FALSE,'$.instructions','','$.response',JSON_OBJECT(),'$.request_key',REPLACE(UUID(),'-',''));
    SET @sdp_ai_session=JSON_SET(JSON_REMOVE(@sdp_ai_session,'$.provided_response','$.behaviors_pending'),'$.capture',JSON_OBJECT(),'$.memory_pending',JSON_OBJECT(),'$.temporary_pending',JSON_OBJECT());
  END IF;
  SET @sdp_ai_commit_receipt=NULL;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.claim_run$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.claim_run(IN p_worker CHAR(32),IN p_lease_seconds INT)
SQL SECURITY DEFINER
claim_block: BEGIN
  DECLARE id_value CHAR(32); DECLARE doc JSON;
  DECLARE owned_transaction BOOLEAN DEFAULT FALSE;
  DECLARE EXIT HANDLER FOR SQLEXCEPTION BEGIN IF owned_transaction THEN ROLLBACK; END IF; RESIGNAL; END;
  CALL sdp_ai.assert_worker();
  IF NOT REGEXP_LIKE(p_worker,'^[0-9a-f]{32}$','c') OR p_lease_seconds NOT BETWEEN 1 AND 3720 THEN CALL sdp_ai.raise_error('option_range'); END IF;
  START TRANSACTION; SET owned_transaction=TRUE;
  -- An expired running lease has an ambiguous external outcome. Never requeue.
  UPDATE sdp_ai.runs SET state='failed',finished_at=UTC_TIMESTAMP(6),error=JSON_OBJECT('code',511002,'phase','recovery','commit_state','committed','unknown_outcome',TRUE,'run_state','failed') WHERE state='running' AND lease_until<UTC_TIMESTAMP(6);
  UPDATE sdp_jobs.compute_jobs q JOIN sdp_ai.runs r ON q.job_id=r.job_id SET q.state='failed',q.error_code='511002',q.events=JSON_ARRAY_APPEND(q.events,'$',JSON_OBJECT('state','failed','unknown_outcome',TRUE,'at',UNIX_TIMESTAMP())) WHERE q.job_kind='ai' AND q.state='running' AND r.state='failed';
  SELECT r.run_id INTO id_value FROM sdp_ai.runs r JOIN sdp_jobs.compute_jobs q ON q.job_id=r.job_id
    WHERE r.state='queued' AND q.state='queued' AND q.job_kind='ai' ORDER BY q.sequence LIMIT 1 FOR UPDATE SKIP LOCKED;
  IF id_value IS NULL THEN COMMIT; SET owned_transaction=FALSE; LEAVE claim_block; END IF;
  IF EXISTS(SELECT 1 FROM sdp_jobs.compute_jobs WHERE job_id=id_value AND cancel_requested) THEN
    UPDATE sdp_ai.runs SET state='cancelled',finished_at=UTC_TIMESTAMP(6),error=JSON_OBJECT('code',511004,'run_state','cancelled') WHERE run_id=id_value;
    UPDATE sdp_jobs.compute_jobs SET state='cancelled',error_code='511004',events=JSON_ARRAY_APPEND(events,'$',JSON_OBJECT('state','cancelled','at',UNIX_TIMESTAMP())) WHERE job_id=id_value;
    COMMIT; SET owned_transaction=FALSE; LEAVE claim_block;
  END IF;
  UPDATE sdp_ai.runs SET state='running',lease_owner=p_worker,lease_until=TIMESTAMPADD(SECOND,p_lease_seconds,UTC_TIMESTAMP(6)) WHERE run_id=id_value;
  UPDATE sdp_jobs.compute_jobs SET state='running',attempt=attempt+1,events=JSON_ARRAY_APPEND(events,'$',JSON_OBJECT('state','running','at',UNIX_TIMESTAMP())) WHERE job_id=id_value;
  SELECT JSON_OBJECT('run_id',run_id,'job_id',job_id,'owner',owner,'epoch',epoch,'conversation_revision',conversation_revision,
    'model',model,'messages',messages,'instructions',instructions,'options',options,'response_format',response_format,'response_schema',response_schema,'functions',FALSE)
    INTO doc FROM sdp_ai.runs WHERE run_id=id_value;
  COMMIT; SET owned_transaction=FALSE;
  SELECT doc AS document;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.renew_lease$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.renew_lease(IN p_run CHAR(32),IN p_worker CHAR(32),IN p_lease_seconds INT)
SQL SECURITY DEFINER
BEGIN
  CALL sdp_ai.assert_worker();
  IF p_lease_seconds NOT BETWEEN 1 AND 3720 THEN CALL sdp_ai.raise_error('option_range'); END IF;
  UPDATE sdp_ai.runs SET lease_until=TIMESTAMPADD(SECOND,p_lease_seconds,UTC_TIMESTAMP(6))
    WHERE run_id=p_run AND state='running' AND lease_owner=p_worker AND lease_until>=UTC_TIMESTAMP(6);
  IF ROW_COUNT()<>1 THEN CALL sdp_ai.raise_error('commit_unknown'); END IF;
  SELECT JSON_OBJECT('run_id',p_run,'cancel_requested',q.cancel_requested) AS document FROM sdp_jobs.compute_jobs q WHERE q.job_id=p_run AND q.job_kind='ai';
END$$

DROP PROCEDURE IF EXISTS sdp_ai.publish_run$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.publish_run(IN p_run CHAR(32),IN p_worker CHAR(32),IN p_response JSON,IN p_metadata JSON,IN p_error JSON)
SQL SECURITY DEFINER
publish_block: BEGIN
  DECLARE actor VARCHAR(576); DECLARE conversation_value CHAR(32); DECLARE revision_value BIGINT UNSIGNED; DECLARE epoch_value BIGINT UNSIGNED;
  DECLARE old_revision BIGINT UNSIGNED; DECLARE old_epoch BIGINT UNSIGNED; DECLARE worker_value CHAR(32); DECLARE lease_value DATETIME(6);
  DECLARE state_value VARCHAR(24); DECLARE hash_value CHAR(64); DECLARE old_hash CHAR(64); DECLARE schema_doc JSON; DECLARE format_value VARCHAR(8);
  DECLARE error_doc JSON; DECLARE result_doc JSON; DECLARE content_value LONGTEXT; DECLARE auto_value BOOLEAN; DECLARE cancelled BOOLEAN;
  DECLARE owned_transaction BOOLEAN DEFAULT FALSE;
  DECLARE EXIT HANDLER FOR SQLEXCEPTION BEGIN IF owned_transaction THEN ROLLBACK; END IF; RESIGNAL; END;
  CALL sdp_ai.assert_worker();
  SET hash_value=SHA2(CONCAT(COALESCE(CAST(p_response AS CHAR),'null'),'|',COALESCE(CAST(p_metadata AS CHAR),'null'),'|',COALESCE(CAST(p_error AS CHAR),'null')),256);
  START TRANSACTION; SET owned_transaction=TRUE;
  SELECT owner,conversation_id,conversation_revision,epoch,lease_owner,lease_until,state,publication_hash,response_schema,response_format
    INTO actor,conversation_value,revision_value,epoch_value,worker_value,lease_value,state_value,old_hash,schema_doc,format_value
    FROM sdp_ai.runs WHERE run_id=p_run FOR UPDATE;
  IF conversation_value IS NULL OR NOT(worker_value<=>p_worker) THEN CALL sdp_ai.raise_error('commit_unknown'); END IF;
  IF state_value IN ('succeeded','refused','failed','cancelled') AND old_hash=hash_value THEN
    COMMIT; SET owned_transaction=FALSE; SELECT JSON_OBJECT('run_id',p_run,'state',state_value,'replayed',TRUE) AS document; LEAVE publish_block;
  END IF;
  IF state_value<>'running' OR lease_value<UTC_TIMESTAMP(6) THEN CALL sdp_ai.raise_error('commit_unknown'); END IF;
  SELECT revision,epoch,auto_append INTO old_revision,old_epoch,auto_value FROM sdp_ai.conversations WHERE conversation_id=conversation_value FOR UPDATE;
  SELECT cancel_requested INTO cancelled FROM sdp_jobs.compute_jobs WHERE job_id=p_run AND job_kind='ai' FOR UPDATE;
  SET error_doc=IF(sdp_ai.json_kind(p_error)='NULL',NULL,p_error),result_doc=IF(sdp_ai.json_kind(p_response)='NULL',NULL,p_response);
  IF cancelled THEN
    SET state_value='cancelled',error_doc=JSON_OBJECT('code',511004,'run_state','cancelled','commit_state','committed'),result_doc=NULL;
  ELSEIF epoch_value<>old_epoch OR revision_value<>old_revision THEN
    SET state_value='failed',error_doc=JSON_OBJECT('code',508002,'run_state','failed','commit_state','committed'),result_doc=NULL;
  ELSEIF error_doc IS NOT NULL THEN
    IF sdp_ai.json_kind(error_doc)<>'OBJECT' OR sdp_ai.json_kind(JSON_EXTRACT(error_doc,'$.code'))<>'INTEGER' THEN CALL sdp_ai.raise_error('provider_protocol'); END IF;
    SET state_value='failed',result_doc=NULL;
  ELSE
    IF sdp_ai.json_kind(result_doc)<>'OBJECT' OR NOT JSON_CONTAINS_PATH(result_doc,'all','$.format','$.value') OR JSON_UNQUOTE(JSON_EXTRACT(result_doc,'$.format')) NOT IN ('TEXT','JSON','REFUSAL') THEN CALL sdp_ai.raise_error('provider_protocol'); END IF;
    IF JSON_UNQUOTE(JSON_EXTRACT(result_doc,'$.format'))<>'REFUSAL' AND JSON_UNQUOTE(JSON_EXTRACT(result_doc,'$.format'))<>format_value THEN CALL sdp_ai.raise_error('capture_type'); END IF;
    IF JSON_UNQUOTE(JSON_EXTRACT(result_doc,'$.format'))<>'REFUSAL' AND format_value='JSON' AND schema_doc IS NOT NULL THEN CALL sdp_ai.validate_response(JSON_EXTRACT(result_doc,'$.value'),schema_doc); END IF;
    IF JSON_UNQUOTE(JSON_EXTRACT(result_doc,'$.format'))='TEXT' AND sdp_ai.json_kind(JSON_EXTRACT(result_doc,'$.value'))<>'STRING' THEN CALL sdp_ai.raise_error('capture_type'); END IF;
    SET state_value=IF(JSON_UNQUOTE(JSON_EXTRACT(result_doc,'$.format'))='REFUSAL','refused','succeeded');
    IF JSON_UNQUOTE(JSON_EXTRACT(result_doc,'$.format'))<>'REFUSAL' THEN
      SET content_value=IF(format_value='TEXT',JSON_UNQUOTE(JSON_EXTRACT(result_doc,'$.value')),CAST(JSON_EXTRACT(result_doc,'$.value') AS CHAR));
      UPDATE sdp_ai.conversations SET messages=JSON_ARRAY_APPEND(messages,'$',JSON_OBJECT('role','assistant','content',content_value)) WHERE conversation_id=conversation_value;
    END IF;
  END IF;
  UPDATE sdp_ai.runs SET state=state_value,response=result_doc,error=error_doc,metadata=p_metadata,publication_hash=hash_value,finished_at=UTC_TIMESTAMP(6) WHERE run_id=p_run;
  UPDATE sdp_jobs.compute_jobs SET state=IF(state_value='refused','succeeded',state_value),error_code=JSON_UNQUOTE(JSON_EXTRACT(error_doc,'$.code')),
    events=JSON_ARRAY_APPEND(events,'$',JSON_OBJECT('state',IF(state_value='refused','succeeded',state_value),'result_state',state_value,'at',UNIX_TIMESTAMP())) WHERE job_id=p_run AND job_kind='ai';
  INSERT INTO sdp_ai.outbox(owner,aggregate_id,event_kind,document) VALUES(actor,p_run,'run_published',JSON_OBJECT('run_id',p_run,'state',state_value,'revision',revision_value,'epoch',epoch_value));
  COMMIT; SET owned_transaction=FALSE;
  SELECT JSON_OBJECT('run_id',p_run,'state',state_value,'replayed',FALSE) AS document;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.claim_model_import$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.claim_model_import(IN p_worker CHAR(32),IN p_lease_seconds INT)
SQL SECURITY DEFINER
import_block: BEGIN
  DECLARE id_value CHAR(32); DECLARE doc JSON;
  DECLARE owned_transaction BOOLEAN DEFAULT FALSE;
  DECLARE EXIT HANDLER FOR SQLEXCEPTION BEGIN IF owned_transaction THEN ROLLBACK; END IF; RESIGNAL; END;
  CALL sdp_ai.assert_worker();
  IF NOT REGEXP_LIKE(p_worker,'^[0-9a-f]{32}$','c') OR p_lease_seconds NOT BETWEEN 1 AND 3720 THEN CALL sdp_ai.raise_error('option_range'); END IF;
  START TRANSACTION; SET owned_transaction=TRUE;
  UPDATE sdp_ai.model_imports SET state='failed',error=JSON_OBJECT('code',511002,'unknown_outcome',TRUE) WHERE state='running' AND lease_until<UTC_TIMESTAMP(6);
  UPDATE sdp_jobs.compute_jobs q JOIN sdp_ai.model_imports i ON q.job_id=i.job_id SET q.state='failed',q.error_code='511002' WHERE q.job_kind='ai_import' AND q.state='running' AND i.state='failed';
  SELECT i.job_id INTO id_value FROM sdp_ai.model_imports i JOIN sdp_jobs.compute_jobs q ON q.job_id=i.job_id
    WHERE i.state='queued' AND q.state='queued' AND q.job_kind='ai_import' AND NOT q.cancel_requested ORDER BY q.sequence LIMIT 1 FOR UPDATE SKIP LOCKED;
  IF id_value IS NULL THEN COMMIT; SET owned_transaction=FALSE; LEAVE import_block; END IF;
  UPDATE sdp_ai.model_imports SET state='running',lease_owner=p_worker,lease_until=TIMESTAMPADD(SECOND,p_lease_seconds,UTC_TIMESTAMP(6)) WHERE job_id=id_value;
  UPDATE sdp_jobs.compute_jobs SET state='running',attempt=attempt+1,events=JSON_ARRAY_APPEND(events,'$',JSON_OBJECT('state','running','at',UNIX_TIMESTAMP())) WHERE job_id=id_value;
  SELECT JSON_OBJECT('job_id',i.job_id,'owner',i.owner,'model_id',i.model_id,'version',i.version,'relative_source',i.relative_source,'sha256',i.sha256,'context_tokens',i.context_tokens,
    'capabilities',JSON_EXTRACT(v.document,'$.capabilities')) INTO doc FROM sdp_ai.model_imports i
    JOIN sdp_ai.object_versions v ON v.object_id=i.model_id AND v.version=i.version WHERE i.job_id=id_value;
  COMMIT; SET owned_transaction=FALSE; SELECT doc AS document;
END$$

DROP PROCEDURE IF EXISTS sdp_ai.publish_model_import$$
DROP PROCEDURE IF EXISTS sdp_ai.renew_model_import$$
CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.renew_model_import(IN p_job CHAR(32),IN p_worker CHAR(32),IN p_lease_seconds INT)
SQL SECURITY DEFINER
BEGIN
  CALL sdp_ai.assert_worker();
  IF p_lease_seconds NOT BETWEEN 1 AND 3720 THEN CALL sdp_ai.raise_error('option_range'); END IF;
  UPDATE sdp_ai.model_imports SET lease_until=TIMESTAMPADD(SECOND,p_lease_seconds,UTC_TIMESTAMP(6))
    WHERE job_id=p_job AND state='running' AND lease_owner=p_worker AND lease_until>=UTC_TIMESTAMP(6);
  IF ROW_COUNT()<>1 THEN CALL sdp_ai.raise_error('commit_unknown'); END IF;
  SELECT JSON_OBJECT('job_id',p_job,'cancel_requested',CAST(IF(q.cancel_requested,'true','false') AS JSON)) AS document
    FROM sdp_jobs.compute_jobs q WHERE q.job_id=p_job AND q.job_kind='ai_import';
END$$

CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.publish_model_import(IN p_job CHAR(32),IN p_worker CHAR(32),IN p_artifact JSON,IN p_error JSON)
SQL SECURITY DEFINER
import_publish_block: BEGIN
  DECLARE id_value CHAR(32); DECLARE version_value BIGINT UNSIGNED; DECLARE sha_value CHAR(64); DECLARE actor VARCHAR(576);
  DECLARE lease_value DATETIME(6); DECLARE worker_value CHAR(32); DECLARE state_value VARCHAR(24); DECLARE doc JSON; DECLARE error_doc JSON;
  DECLARE old_hash CHAR(64); DECLARE hash_value CHAR(64); DECLARE cancelled BOOLEAN;
  DECLARE owned_transaction BOOLEAN DEFAULT FALSE;
  DECLARE EXIT HANDLER FOR SQLEXCEPTION BEGIN IF owned_transaction THEN ROLLBACK; END IF; RESIGNAL; END;
  CALL sdp_ai.assert_worker(); START TRANSACTION; SET owned_transaction=TRUE;
  SET hash_value=SHA2(CONCAT(COALESCE(CAST(p_artifact AS CHAR),'null'),'|',COALESCE(CAST(p_error AS CHAR),'null')),256);
  SELECT model_id,version,sha256,owner,lease_owner,lease_until,state,publication_hash INTO id_value,version_value,sha_value,actor,worker_value,lease_value,state_value,old_hash FROM sdp_ai.model_imports WHERE job_id=p_job FOR UPDATE;
  IF id_value IS NULL OR worker_value<>p_worker THEN CALL sdp_ai.raise_error('commit_unknown'); END IF;
  IF state_value IN ('succeeded','failed') AND hash_value=old_hash THEN
    COMMIT; SET owned_transaction=FALSE;
    SELECT JSON_OBJECT('job_id',p_job,'model_id',id_value,'state',state_value,'replayed',TRUE) AS document;
    LEAVE import_publish_block;
  END IF;
  IF state_value<>'running' OR lease_value<UTC_TIMESTAMP(6) THEN CALL sdp_ai.raise_error('commit_unknown'); END IF;
  SET error_doc=IF(sdp_ai.json_kind(p_error)='NULL',NULL,p_error);
  SELECT document INTO doc FROM sdp_ai.objects WHERE object_id=id_value AND version=version_value AND state='pending_import' FOR UPDATE;
  IF doc IS NULL THEN CALL sdp_ai.raise_error('revision_conflict'); END IF;
  SELECT cancel_requested INTO cancelled FROM sdp_jobs.compute_jobs WHERE job_id=p_job AND job_kind='ai_import' FOR UPDATE;
  IF cancelled THEN SET error_doc=JSON_OBJECT('code',511004,'run_state','cancelled','commit_state','committed'); END IF;
  IF error_doc IS NULL THEN
    IF sdp_ai.json_kind(p_artifact)<>'OBJECT' OR
       sdp_ai.json_kind(JSON_EXTRACT(p_artifact,'$.artifact_id'))<>'STRING' OR JSON_UNQUOTE(JSON_EXTRACT(p_artifact,'$.artifact_id'))<>sha_value OR
       sdp_ai.json_kind(JSON_EXTRACT(p_artifact,'$.sha256'))<>'STRING' OR JSON_UNQUOTE(JSON_EXTRACT(p_artifact,'$.sha256'))<>sha_value OR
       sdp_ai.json_kind(JSON_EXTRACT(p_artifact,'$.bytes'))<>'INTEGER' OR JSON_EXTRACT(p_artifact,'$.bytes')<24 OR
       sdp_ai.json_kind(JSON_EXTRACT(p_artifact,'$.runtime_verified'))<>'BOOLEAN' OR JSON_UNQUOTE(JSON_EXTRACT(p_artifact,'$.runtime_verified'))<>'true' THEN CALL sdp_ai.raise_error('model_integrity'); END IF;
    IF JSON_CONTAINS(JSON_EXTRACT(doc,'$.capabilities'),JSON_QUOTE('tools')) AND
      (sdp_ai.json_kind(JSON_EXTRACT(p_artifact,'$.tool_template_verified'))<>'BOOLEAN' OR JSON_UNQUOTE(JSON_EXTRACT(p_artifact,'$.tool_template_verified'))<>'true' OR
       sdp_ai.json_kind(JSON_EXTRACT(p_artifact,'$.tool_template_sha256'))<>'STRING' OR NOT REGEXP_LIKE(JSON_UNQUOTE(JSON_EXTRACT(p_artifact,'$.tool_template_sha256')),'^[0-9a-f]{64}$','c')) THEN CALL sdp_ai.raise_error('model_capability'); END IF;
    SET state_value='succeeded',doc=JSON_SET(doc,'$.state','active','$.settings.artifact',p_artifact,'$.settings.artifact_id',sha_value,'$.settings.runtime_state','verified');
    IF JSON_CONTAINS(JSON_EXTRACT(doc,'$.capabilities'),JSON_QUOTE('tools')) THEN
      SET doc=JSON_SET(doc,'$.settings.tool_template_sha256',JSON_EXTRACT(p_artifact,'$.tool_template_sha256'));
    END IF;
  ELSE
    SET state_value='failed',doc=JSON_SET(doc,'$.state','invalid','$.settings.runtime_state','import_failed');
  END IF;
  UPDATE sdp_ai.model_imports SET state=state_value,artifact=p_artifact,error=error_doc,publication_hash=hash_value WHERE job_id=p_job;
  UPDATE sdp_ai.objects SET state=IF(state_value='succeeded','active','invalid'),document=doc,updated_at=UTC_TIMESTAMP(6) WHERE object_id=id_value AND version=version_value;
  UPDATE sdp_ai.object_versions SET state=IF(state_value='succeeded','active','invalid'),document=doc WHERE object_id=id_value AND version=version_value;
  UPDATE sdp_jobs.compute_jobs SET state=IF(cancelled,'cancelled',state_value),error_code=JSON_UNQUOTE(JSON_EXTRACT(error_doc,'$.code')),events=JSON_ARRAY_APPEND(events,'$',JSON_OBJECT('state',IF(cancelled,'cancelled',state_value),'at',UNIX_TIMESTAMP())) WHERE job_id=p_job AND job_kind='ai_import';
  INSERT INTO sdp_ai.outbox(owner,aggregate_id,event_kind,document) VALUES(actor,id_value,'model_import_published',JSON_OBJECT('model_id',id_value,'state',state_value,'artifact_id',IF(state_value='succeeded',sha_value,NULL)));
  COMMIT; SET owned_transaction=FALSE; SELECT JSON_OBJECT('job_id',p_job,'model_id',id_value,'state',state_value,'replayed',FALSE) AS document;
END$$
DELIMITER ;

-- Per-object grants have no schema-name wildcard and confer no routine DDL.
GRANT SELECT ON sdp_ai.catalog_version TO 'sdp_ai_internal'@'localhost';
GRANT SELECT,INSERT,UPDATE ON sdp_ai.objects TO 'sdp_ai_internal'@'localhost';
GRANT SELECT,INSERT,UPDATE ON sdp_ai.object_versions TO 'sdp_ai_internal'@'localhost';
GRANT SELECT,INSERT ON sdp_ai.dependencies TO 'sdp_ai_internal'@'localhost';
GRANT SELECT ON sdp_ai.object_grants TO 'sdp_ai_internal'@'localhost';
GRANT SELECT,INSERT,UPDATE ON sdp_ai.conversations TO 'sdp_ai_internal'@'localhost';
GRANT SELECT,INSERT,UPDATE ON sdp_ai.runs TO 'sdp_ai_internal'@'localhost';
GRANT SELECT,INSERT,UPDATE ON sdp_ai.model_imports TO 'sdp_ai_internal'@'localhost';
GRANT SELECT,INSERT ON sdp_ai.outbox TO 'sdp_ai_internal'@'localhost';
GRANT SELECT ON sdp_ai.worker_accounts TO 'sdp_ai_internal'@'localhost';
GRANT SELECT,INSERT,UPDATE ON sdp_jobs.compute_jobs TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON FUNCTION sdp_ai.json_kind TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON FUNCTION sdp_ai.can_use TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON FUNCTION sdp_ai.has_pending_turn TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON FUNCTION sdp_ai.run_receipt TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON FUNCTION sdp_ai.merge_memory TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON FUNCTION sdp_ai.boolean_value TO 'sdp_ai_internal'@'localhost';

CALL mysql.__sdp_ai_restore_routine_grants_v1();
DROP PROCEDURE mysql.__sdp_ai_restore_routine_grants_v1;
DROP TEMPORARY TABLE sdp_ai.__saved_routine_grants_v1;
SET @__ai_restore_grant=NULL;
GRANT EXECUTE ON PROCEDURE sdp_ai.raise_error TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.assert_worker TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.validate_options TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.check_response_schema TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.validate_response TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.object_document TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.dispatch TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.knowledge_document TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.before_commit TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.after_commit TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.poll_commit TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.rollback_turn TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.claim_run TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.renew_lease TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.publish_run TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.claim_model_import TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.renew_model_import TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.publish_model_import TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.check_capture TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.prepare_response TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.check_behaviors TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.apply_behaviors TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.stage_behavior TO 'sdp_ai_internal'@'localhost';


GRANT EXECUTE ON PROCEDURE sdp_ai.behavior_binding TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON PROCEDURE sdp_ai.named_behavior TO 'sdp_ai_internal'@'localhost';
GRANT EXECUTE ON FUNCTION sdp_ai.knowledge_value TO 'sdp_ai_internal'@'localhost';
