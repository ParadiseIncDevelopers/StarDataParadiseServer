"""Stage the matched next native tool catalog; never modify build12 inputs."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / 'out/ai-tools-next13'


def once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError('AI tools catalog anchor: ' + old[:120])
    return source.replace(old, new)


def private_profile(source):
    source=source.replace("'tool_call_status','publish_tool_failure'", "'tool_call_status','publish_tool_failure','tool_owner_context','bind_tool_action','tool_action_document','resolve_tool_action','action_dispatch'")
    source=source.replace("TABLE_NAME='object_grants' AND PRIVILEGE_TYPE IN ('SELECT')", "TABLE_NAME='object_grants' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE','DELETE')")
    for helper in ('__sdp_ai_restore_routine_grants_v1','__sdp_ai_restore_tool_grants_v1'):
        source=source.replace("ROUTINE_NAME='"+helper+"'", "ROUTINE_NAME IN ('"+helper+"','__sdp_ai_restore_action_grants_v1')")
    anchor="      OR      (TABLE_SCHEMA='sdp_jobs' AND TABLE_NAME='compute_jobs'"
    actions="""      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='action_policies' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='action_grants' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE','DELETE'))
      OR      (TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='actions' AND PRIVILEGE_TYPE IN ('SELECT','INSERT','UPDATE'))
"""
    if anchor not in source:raise RuntimeError('Missing fixed AI private table profile')
    return source.replace(anchor,actions+anchor)


def utc_routines(source):
    # Keep existing DDL defaults/data intact. Lease writes/comparisons and
    # explicit routine audit updates use a timezone-independent clock.
    start=source.index("CREATE DEFINER='sdp_ai_internal'@'localhost' PROCEDURE sdp_ai.")
    return source[:start]+source[start:].replace('CURRENT_TIMESTAMP(6)','UTC_TIMESTAMP(6)')


def catalog(source):
    source=private_profile(source)
    source=once(source,'  principal VARCHAR(576) COLLATE utf8mb4_bin NOT NULL,','  principal VARCHAR(584) COLLATE utf8mb4_bin NOT NULL,')
    source=once(source,'CREATE TABLE IF NOT EXISTS sdp_ai.conversations (',"""SET @__ai_principal_migration=IF((SELECT CHARACTER_MAXIMUM_LENGTH FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='object_grants' AND COLUMN_NAME='principal')<584,
  'ALTER TABLE sdp_ai.object_grants MODIFY COLUMN principal VARCHAR(584) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL','DO 0');
PREPARE __ai_principal_statement FROM @__ai_principal_migration;
EXECUTE __ai_principal_statement;DEALLOCATE PREPARE __ai_principal_statement;
SET @__ai_principal_migration=NULL;
CREATE TABLE IF NOT EXISTS sdp_ai.conversations (""")
    source = once(source,
        "  options JSON NOT NULL, response_format VARCHAR(8) NOT NULL DEFAULT 'TEXT', response_schema JSON NULL,",
        "  options JSON NOT NULL, response_format VARCHAR(8) NOT NULL DEFAULT 'TEXT', response_schema JSON NULL,\n  execution_context JSON NULL,")
    anchor = 'CREATE TABLE IF NOT EXISTS sdp_ai.model_imports ('
    migration = """SET @__ai_execution_context_migration=IF(EXISTS(SELECT 1 FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA='sdp_ai' AND TABLE_NAME='runs' AND COLUMN_NAME='execution_context'),
  'DO 0','ALTER TABLE sdp_ai.runs ADD COLUMN execution_context JSON NULL');
PREPARE __ai_execution_context_statement FROM @__ai_execution_context_migration;
EXECUTE __ai_execution_context_statement;DEALLOCATE PREPARE __ai_execution_context_statement;
SET @__ai_execution_context_migration=NULL;
"""
    source = once(source, anchor, migration + anchor)
    source = once(source,
        "WHERE g.object_id=p_id AND g.principal=actor AND g.privilege_name IN (p_privilege,'OWNER')",
        "WHERE g.object_id=p_id AND JSON_CONTAINS(JSON_EXTRACT(STARDATAPARADISE_PRINCIPALS(),'$.principals'),JSON_QUOTE(g.principal))\n        AND g.privilege_name IN (p_privilege,'OWNER')")
    source = once(source,
        "  IF NOT EXISTS(SELECT 1 FROM sdp_ai.worker_accounts WHERE actor=STARDATAPARADISE_ACTOR() AND enabled) THEN",
        "  IF STARDATAPARADISE_TOOL_EXECUTION_ACTIVE()<>0 OR NOT EXISTS(SELECT 1 FROM sdp_ai.worker_accounts WHERE actor=STARDATAPARADISE_ACTOR() AND enabled) THEN")
    # Mutable conversation content is a SELECT privilege; it never grants
    # APPEND or access to another initiating actor's worker credentials.
    source=once(source,
        '  DECLARE doc JSON; DECLARE deps JSON; DECLARE versions_doc JSON;',
        '  DECLARE doc JSON; DECLARE deps JSON; DECLARE versions_doc JSON; DECLARE tool_summary JSON;')
    source=once(source,
        "  IF NOT sdp_ai.can_use(p_id,'USE') THEN CALL sdp_ai.raise_error('invalid_dependency'); END IF;\n  SELECT document INTO doc FROM sdp_ai.objects WHERE object_id=p_id;",
        """  SELECT JSON_SET(document,'$.kind',kind) INTO doc FROM sdp_ai.objects WHERE object_id=p_id;
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
  END IF;""")
    source=once(source,
        "  SET doc=JSON_OBJECT('id',id_value,'name',p_name,'schema_name',p_schema,'version',v_version,'state','active','owner',actor,",
        "  SET doc=JSON_OBJECT('id',id_value,'kind',p_kind,'name',p_name,'schema_name',p_schema,'version',v_version,'state','active','owner',actor,")
    source = once(source,
        "WHERE kind=p_kind AND state<>'retired' AND (owner=actor OR sdp_ai.can_use(object_id,'USE')) ORDER BY schema_name,name;",
        "WHERE kind=p_kind AND state<>'retired' AND (owner=actor OR sdp_ai.can_use(object_id,CASE p_kind WHEN 'conversation' THEN 'SELECT' WHEN 'knowledge' THEN 'EXECUTE' ELSE 'USE' END)) ORDER BY schema_name,name;")
    source=once(source,
        "    ELSEIF p_operation='describe_run' THEN\n      SELECT JSON_OBJECT('run_id',run_id,'job_id',job_id,'state',state,'conversation_revision',conversation_revision,'epoch',epoch,'created_at',created_at,'finished_at',finished_at,'error',error) AS document\n        FROM sdp_ai.runs WHERE run_id=id_value AND owner=actor;",
        """    ELSEIF p_operation='describe_run' THEN
      SET value_doc=JSON_ARRAY();
      IF EXISTS(SELECT 1 FROM sdp_ai.runs WHERE run_id=id_value AND owner=actor AND JSON_EXTRACT(execution_context,'$.has_tools')=TRUE) THEN
        SELECT COALESCE(JSON_ARRAYAGG(JSON_OBJECT('call_id',t.call_id,'state',t.state,'kind',JSON_EXTRACT(t.snapshot,'$.kind'),
          'schema_name',JSON_EXTRACT(t.snapshot,'$.schema_name'),'name',JSON_EXTRACT(t.snapshot,'$.name'),'action_id',t.action_id)),JSON_ARRAY())
          INTO value_doc FROM sdp_ai.tool_calls t WHERE t.run_id=id_value;
      END IF;
      SELECT JSON_OBJECT('run_id',run_id,'job_id',job_id,'state',state,'conversation_revision',conversation_revision,'epoch',epoch,'created_at',created_at,'finished_at',finished_at,'error',error,'tool_calls',value_doc) AS document
        FROM sdp_ai.runs WHERE run_id=id_value AND owner=actor;""")
    source = once(source,
        "IF NOT sdp_ai.can_use(id_value,'USE') THEN CALL sdp_ai.raise_error('conversation_not_found'); END IF;",
        "IF p_operation<>'finish' AND NOT sdp_ai.can_use(id_value,IF(p_operation='memory','SELECT','APPEND')) THEN CALL sdp_ai.raise_error('conversation_not_found'); END IF;")
    source = once(source,
        "IF NOT sdp_ai.can_use(id_value,'USE') OR NOT EXISTS(SELECT 1 FROM sdp_ai.objects WHERE object_id=id_value AND kind='conversation' AND state='active') THEN",
        "IF NOT sdp_ai.can_use(id_value,'APPEND') OR NOT EXISTS(SELECT 1 FROM sdp_ai.objects WHERE object_id=id_value AND kind='conversation' AND state='active') THEN")
    source = source.replace("sdp_ai.can_use(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.conversation_id')),'USE')",
                            "sdp_ai.can_use(JSON_UNQUOTE(JSON_EXTRACT(@sdp_ai_session,'$.conversation_id')),'APPEND')")
    source = once(source,
        "IF k NOT IN ('functions','auto_append','persistent','model_schema','model_name','response') THEN",
        "IF k NOT IN ('functions','tools','auto_append','persistent','model_schema','model_name','response') THEN")
    source = once(source,
        "    IF sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.functions'),FALSE) THEN CALL sdp_ai.raise_error('model_capability'); END IF;",
        """    SET settings=JSON_SET(settings,'$.functions',CAST(IF(sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.functions'),FALSE),'true','false') AS JSON),
      '$.tools',COALESCE(JSON_EXTRACT(settings,'$.tools'),JSON_ARRAY()));
    IF sdp_ai.json_kind(JSON_EXTRACT(settings,'$.tools'))<>'ARRAY' THEN CALL sdp_ai.raise_error('tool_arguments');END IF;
    IF NOT sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.functions'),FALSE) AND JSON_LENGTH(JSON_EXTRACT(settings,'$.tools'))>0 THEN CALL sdp_ai.raise_error('tool_denied');END IF;
    IF p_operation<>'alter' OR JSON_CONTAINS_PATH(p_payload,'one','$.tools') THEN
      SET settings=JSON_SET(settings,'$.tool_bindings',CAST(STARDATAPARADISE_TOOLS_VALIDATE(JSON_EXTRACT(settings,'$.tools')) AS JSON));
    END IF;""")
    source = once(source,
        "    SELECT document INTO model_doc FROM sdp_ai.object_versions WHERE object_id=model_id_value AND version=model_version_value;\n    CALL sdp_ai.validate_options",
        "    SELECT document INTO model_doc FROM sdp_ai.object_versions WHERE object_id=model_id_value AND version=model_version_value;\n    IF JSON_LENGTH(JSON_EXTRACT(settings,'$.tool_bindings'))>0 AND NOT JSON_CONTAINS(JSON_EXTRACT(model_doc,'$.capabilities'),JSON_QUOTE('tools')) THEN CALL sdp_ai.raise_error('model_capability');END IF;\n    CALL sdp_ai.validate_options")
    source = once(source,
        "VALUES(id_value,model_id_value,model_version_value,JSON_ARRAY(),JSON_OBJECT(),FALSE,sdp_ai.boolean_value",
        "VALUES(id_value,model_id_value,model_version_value,JSON_ARRAY(),JSON_OBJECT(),sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.functions'),FALSE),sdp_ai.boolean_value")
    source = once(source,
        "      UPDATE sdp_ai.conversations SET model_id=model_id_value,model_version=model_version_value,revision=revision+1,",
        "      UPDATE sdp_ai.conversations SET model_id=model_id_value,model_version=model_version_value,revision=revision+1,\n        functions_enabled=sdp_ai.boolean_value(JSON_EXTRACT(settings,'$.functions'),FALSE),")
    # An ALTER unrelated to tools preserves the pinned allowlist; a subsequent
    # turn revalidates it rather than silently adopting a replacement routine.
    source = once(source,
        "  DECLARE auto_value BOOLEAN; DECLARE local_metadata JSON; DECLARE content_value LONGTEXT;",
        "  DECLARE auto_value BOOLEAN; DECLARE local_metadata JSON; DECLARE content_value LONGTEXT;\n  DECLARE tools_enabled BOOLEAN;DECLARE tool_requests JSON;DECLARE tools_doc JSON;DECLARE pinned_tools JSON;DECLARE principals_doc JSON;")
    source = once(source,
        "IF NOT EXISTS(SELECT 1 FROM sdp_ai.objects WHERE object_id=id_value AND kind='conversation' AND state='active' AND owner=actor) THEN CALL sdp_ai.raise_error('conversation_not_found'); END IF;",
        "IF NOT EXISTS(SELECT 1 FROM sdp_ai.objects WHERE object_id=id_value AND kind='conversation' AND state='active') OR NOT sdp_ai.can_use(id_value,'APPEND') THEN CALL sdp_ai.raise_error('conversation_not_found'); END IF;")
    source = once(source,
        "  SET response_doc=JSON_MERGE_PATCH(COALESCE(defaults_doc,JSON_OBJECT()),response_doc);",
        """  SELECT functions_enabled INTO tools_enabled FROM sdp_ai.conversations WHERE conversation_id=id_value;
  SELECT JSON_EXTRACT(document,'$.settings.tools'),JSON_EXTRACT(document,'$.settings.tool_bindings') INTO tool_requests,pinned_tools FROM sdp_ai.objects WHERE object_id=id_value;
  IF tools_enabled THEN
    SET tools_doc=CAST(STARDATAPARADISE_TOOLS_VALIDATE(tool_requests) AS JSON);
    IF NOT(tools_doc<=>pinned_tools) THEN CALL sdp_ai.raise_error('approval_stale');END IF;
  ELSE SET tools_doc=JSON_ARRAY();END IF;
  SET principals_doc=STARDATAPARADISE_PRINCIPALS();
  IF NOT(JSON_UNQUOTE(JSON_EXTRACT(principals_doc,'$.actor'))<=>actor) THEN CALL sdp_ai.raise_error('tool_denied');END IF;
  SET response_doc=JSON_MERGE_PATCH(COALESCE(defaults_doc,JSON_OBJECT()),response_doc);""")
    source = once(source,
        "'provided_response',provided_doc,'behaviors',normalized_behaviors) AS CHAR),256);",
        "'provided_response',provided_doc,'behaviors',normalized_behaviors,'tools',tools_doc,'execution_context',principals_doc,'time_zone',@@session.time_zone) AS CHAR),256);")
    source = once(source,
        'INSERT INTO sdp_ai.runs(run_id,job_id,owner,conversation_id,conversation_revision,epoch,request_key,request_fingerprint,model,messages,instructions,options,response_format,response_schema)',
        'INSERT INTO sdp_ai.runs(run_id,job_id,owner,conversation_id,conversation_revision,epoch,request_key,request_fingerprint,model,messages,instructions,options,response_format,response_schema,execution_context)')
    source = once(source,
        'VALUES(run_value,run_value,actor,id_value,revision_value,epoch_value,key_value,fingerprint_value,model_doc,provider_messages_doc,instructions_value,options_doc,format_value,schema_doc);',
        "VALUES(run_value,run_value,actor,id_value,revision_value,epoch_value,key_value,fingerprint_value,model_doc,provider_messages_doc,instructions_value,options_doc,format_value,schema_doc,JSON_OBJECT('actor',actor,'roles',JSON_EXTRACT(principals_doc,'$.roles'),'time_zone',@@session.time_zone,'has_tools',CAST(IF(tools_enabled AND JSON_LENGTH(tools_doc)>0 AND NOT provided_value,'true','false') AS JSON)));\n  IF tools_enabled AND JSON_LENGTH(tools_doc)>0 AND NOT provided_value THEN INSERT INTO sdp_ai.run_tools(run_id,tools) VALUES(run_value,tools_doc);END IF;")
    source=source.replace('ORDER BY r.created_at,r.run_id LIMIT 1 FOR UPDATE SKIP LOCKED','ORDER BY q.sequence LIMIT 1 FOR UPDATE SKIP LOCKED')
    source=source.replace('ORDER BY i.created_at,i.job_id LIMIT 1 FOR UPDATE SKIP LOCKED','ORDER BY q.sequence LIMIT 1 FOR UPDATE SKIP LOCKED')
    return utc_routines(source)


def tools(source):
    # The native scope, actor, current role closure, conversation and model
    # rights are checked again after switching to the protected owner. A
    # physical worker's grants never authorize application execution.
    source = private_profile(source)
    source = once(source,
        '  execution_worker VARCHAR(576) COLLATE utf8mb4_bin NULL,',
        '  execution_worker VARCHAR(576) COLLATE utf8mb4_bin NULL,\n  action_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL,\n  action_snapshot_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,')
    source = once(source,
        "'timeout_ms',CAST(COALESCE(JSON_EXTRACT(r.options,'$.run_timeout_ms'),60000) AS UNSIGNED))",
        "'timeout_ms',CAST(COALESCE(JSON_EXTRACT(r.options,'$.run_timeout_ms'),60000) AS UNSIGNED),\n    'execution_context',r.execution_context,'action_id',c.action_id,'action_snapshot_hash',c.action_snapshot_hash)")
    anchor = 'DROP PROCEDURE IF EXISTS sdp_ai.knowledge_tool_document$$'
    source = once(source, anchor, """DROP PROCEDURE IF EXISTS sdp_ai.tool_owner_context$$
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

""" + anchor)
    source=once(source,'DROP PROCEDURE IF EXISTS sdp_ai.publish_tool_requests$$',
        (ROOT/'compatibility/ai/native/tool-action-routines.inc').read_text(encoding='utf-8')+'\nDROP PROCEDURE IF EXISTS sdp_ai.publish_tool_requests$$')
    for name in ('tool_owner_context','bind_tool_action','tool_action_document','resolve_tool_action'):
        source += "\nGRANT EXECUTE ON PROCEDURE sdp_ai."+name+" TO 'sdp_ai_internal'@'localhost';\n"
    source=source.replace('ORDER BY r.created_at,r.run_id LIMIT 1 FOR UPDATE SKIP LOCKED','ORDER BY q.sequence LIMIT 1 FOR UPDATE SKIP LOCKED')
    return utc_routines(source)


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, transform in [('catalog.sql', catalog), ('tool_catalog.sql', tools)]:
        source = (ROOT/'compatibility/ai'/name).read_text(encoding='utf-8')
        (OUTPUT/name).write_text(transform(source), encoding='utf-8', newline='\n')
    header=(ROOT/'compatibility/ai/native/sdp_ai_catalog_guard.h').read_text(encoding='utf-8')
    header=once(header,'"tool_call_status","publish_tool_failure"};','"tool_call_status","publish_tool_failure","tool_owner_context","bind_tool_action","tool_action_document","resolve_tool_action"};')
    (OUTPUT/'sdp_ai_catalog_guard.h').write_text(header,encoding='utf-8',newline='\n')
    print('Staged native13 catalog only:', OUTPUT)


if __name__ == '__main__':
    main()
