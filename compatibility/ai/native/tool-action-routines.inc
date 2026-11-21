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
    UPDATE sdp_ai.tool_calls SET state='denied',error=content_doc,finished_at=CURRENT_TIMESTAMP(6) WHERE call_id=p_call;
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
