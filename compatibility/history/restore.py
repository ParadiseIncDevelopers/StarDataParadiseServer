"""Generate statement-atomic row restore and field revert for BIGINT/TEXT History tables."""

if __package__:
    from .provision import identifier
else:
    from provision import identifier


def schema_guard(table):
    expected = []
    for column in table.columns:
        data_type = "bigint" if column.kind == "bigint" else "longtext"
        representation = ("column_type NOT LIKE '%unsigned%'" if column.kind == "bigint"
                          else "character_set_name = 'utf8mb4'")
        expected.append(f"(column_name = '{column.name}' AND data_type = '{data_type}' AND {representation})")
    columns = " OR\n             ".join(expected)
    same_table = f"table_schema = '{table.source_schema}' AND table_name = '{table.source_table}'"
    return f"""    IF NOT EXISTS (
        SELECT 1 FROM information_schema.tables WHERE {same_table} AND engine = 'InnoDB'
    ) OR (SELECT COUNT(*) FROM information_schema.columns WHERE {same_table}) <> {len(expected)}
       OR (SELECT COUNT(*) FROM information_schema.columns WHERE {same_table} AND extra = ''
           AND ({columns})) <> {len(expected)}
       OR (SELECT COUNT(*) FROM information_schema.key_column_usage
           WHERE {same_table} AND constraint_name = 'PRIMARY') <> 1
       OR NOT EXISTS (SELECT 1 FROM information_schema.key_column_usage
                      WHERE {same_table} AND constraint_name = 'PRIMARY' AND column_name = '{table.key}')
       OR EXISTS (SELECT 1 FROM information_schema.referential_constraints
           WHERE unique_constraint_schema = '{table.source_schema}' AND referenced_table_name = '{table.source_table}'
             AND update_rule NOT IN ('RESTRICT', 'NO ACTION'))
       OR (SELECT COUNT(*) FROM information_schema.triggers
           WHERE event_object_schema = '{table.source_schema}'
             AND event_object_table = '{table.source_table}' AND action_timing = 'AFTER'
             AND (trigger_name, event_manipulation) IN (('sdp_history_insert', 'INSERT'),
                  ('sdp_history_update', 'UPDATE'), ('sdp_history_delete', 'DELETE'))) <> 3
       OR (SELECT COUNT(*) FROM information_schema.tables
           WHERE table_schema = '{table.journal_schema}' AND engine = 'InnoDB'
             AND table_name IN ('row_identity', 'revision_counter', 'events', 'restore_plans')) <> 4 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'schema_conflict';
    END IF;"""


def tag(column):
    return f"JSON_UNQUOTE(JSON_EXTRACT(target_image, '$.values.{column}.t'))"


def value(column):
    return f"JSON_UNQUOTE(JSON_EXTRACT(target_image, '$.values.{column}.v'))"


def target_shape(table):
    checks = [
        "JSON_EXTRACT(target_image, '$.schema_version') = 1",
        f"JSON_LENGTH(JSON_EXTRACT(target_image, '$.values')) = {len(table.columns)}",
    ]
    for column in table.columns:
        kind = tag(column.name)
        item = value(column.name)
        string = f"JSON_TYPE(JSON_EXTRACT(target_image, '$.values.{column.name}.v')) = 'STRING'"
        if column.kind == "bigint":
            typed = (f"({kind} = 'i64' AND {string} AND "
                     f"REGEXP_LIKE({item}, '^-?(0|[1-9][0-9]{{0,18}})$', 'c') AND "
                     f"{item} = CAST(CAST({item} AS SIGNED) AS CHAR CHARACTER SET ascii))")
        else:
            typed = f"({kind} = 'text' AND {string})"
        checks.append(typed if column.name == table.key else f"({kind} = 'null' OR {typed})")
    return "\n             AND ".join(checks)


def assignment(column):
    item = value(column.name)
    if column.kind == "bigint":
        item = f"CAST({item} AS SIGNED)"
    return f"{identifier(column.name)} = IF({tag(column.name)} = 'null', NULL, {item})"


def update_plan_row(table, fields):
    source, key = table.source, identifier(table.key)
    assignments = ",\n            ".join(assignment(column) for column in table.columns)
    writes = f"""    IF selected_field IS NULL THEN
        UPDATE {source} SET
            {assignments}
            WHERE {key} = expected_key;
"""
    if fields:
        options = "\n".join(
            f"        WHEN '{column.name}' THEN\n"
            f"            UPDATE {source} SET {assignment(column)} WHERE {key} = expected_key;"
            for column in fields
        )
        return writes + f"""    ELSE
        CASE selected_field
{options}
        ELSE SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'invalid_restore_field';
        END CASE;
    END IF;"""
    return writes + """    ELSE SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'invalid_restore_field';
    END IF;"""


def provision_restore(table):
    if any(column.kind == "blob" for column in table.columns):
        raise ValueError("History RESTORE requires reversible BIGINT/TEXT values; BLOB digests cannot be restored")
    journal, source = table.journal, table.source
    key = identifier(table.key)
    guard = schema_guard(table)
    shape = target_shape(table)
    fields = [column for column in table.columns if column.name != table.key]
    if fields:
        allowed_fields = ", ".join(f"'{column.name}'" for column in fields)
        invalid_field = f"CAST(selected_field AS BINARY) NOT IN ({allowed_fields})"
    else:
        invalid_field = "TRUE"
    return f"""CREATE TABLE {journal}.restore_plans (
    plan_id BINARY(16) PRIMARY KEY,
    owner VARBINARY(512) NOT NULL,
    requested_revision BIGINT UNSIGNED NOT NULL,
    base_revision BIGINT UNSIGNED NOT NULL,
    tracked_row BIGINT UNSIGNED NOT NULL,
    source_key BIGINT NOT NULL,
    image JSON NOT NULL,
    request_token BINARY(16),
    applied_revision BIGINT UNSIGNED,
    UNIQUE KEY once_by_actor (owner, request_token)
) ENGINE=InnoDB;

DELIMITER $$
CREATE FUNCTION {journal}.history_plan_preview(
    requested BIGINT UNSIGNED, stable_row BIGINT UNSIGNED,
    selected_field VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin
) RETURNS JSON SQL SECURITY INVOKER MODIFIES SQL DATA
BEGIN
    DECLARE plan BINARY(16);
    DECLARE watermark BIGINT UNSIGNED;
    DECLARE current_key BIGINT;
    DECLARE target_key BIGINT;
    DECLARE current_image JSON;
    DECLARE logged_current_image JSON;
    DECLARE target_image JSON;
    DECLARE event_before JSON;
    DECLARE event_after JSON;
    DECLARE field_path VARCHAR(280);
    DECLARE old_field JSON;
    DECLARE new_field JSON;
    DECLARE live_field JSON;

    IF requested IS NULL OR stable_row IS NULL OR stable_row = 0 THEN
        SIGNAL SQLSTATE '22023' SET MESSAGE_TEXT = 'invalid_restore_request';
    END IF;
    IF selected_field IS NOT NULL AND ({invalid_field}) THEN
        SIGNAL SQLSTATE '22023' SET MESSAGE_TEXT = 'invalid_restore_field';
    END IF;
{guard}
    SELECT counter.head, live_row.{key}, {table.row_image("live_row")},
           (SELECT e.after_image FROM {journal}.events AS e
            WHERE e.row_id = stable_row AND e.revision <= counter.head ORDER BY e.revision DESC LIMIT 1),
           IF(selected_field IS NULL, (SELECT e.after_image FROM {journal}.events AS e
            WHERE e.row_id = stable_row AND e.revision <= requested ORDER BY e.revision DESC LIMIT 1), NULL),
           chosen.before_image, chosen.after_image
      INTO watermark, current_key, current_image, logged_current_image, target_image, event_before, event_after
      FROM {journal}.row_identity AS ident
      JOIN {source} AS live_row ON live_row.{key} = ident.source_key
      CROSS JOIN {journal}.revision_counter AS counter
      LEFT JOIN {journal}.events AS chosen ON chosen.revision = requested AND chosen.row_id = ident.row_id
      WHERE ident.row_id = stable_row AND counter.singleton = 1;
    IF current_key IS NULL OR NOT (current_image <=> logged_current_image) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'row_identity_conflict';
    END IF;
    IF requested > watermark OR (selected_field IS NULL AND target_image IS NULL)
       OR (selected_field IS NOT NULL AND (event_before IS NULL OR event_after IS NULL)) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'revision_not_recoverable';
    END IF;
    IF selected_field IS NOT NULL THEN
        SET field_path = CONCAT('$.values.', selected_field);
        SET old_field = JSON_EXTRACT(event_before, field_path);
        SET new_field = JSON_EXTRACT(event_after, field_path);
        SET live_field = JSON_EXTRACT(current_image, field_path);
        IF old_field IS NULL OR new_field IS NULL OR live_field IS NULL THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'schema_conflict';
        END IF;
        IF old_field = new_field THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'revert_field_unchanged';
        END IF;
        IF live_field = old_field THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'restore_no_change';
        END IF;
        IF NOT (live_field <=> new_field) THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'revert_field_conflict';
        END IF;
        SET target_image = JSON_SET(current_image, field_path, old_field);
    END IF;
    IF ({shape}) IS NOT TRUE THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'schema_conflict';
    END IF;
    SET target_key = CAST({value(table.key)} AS SIGNED);
    IF target_key <> current_key AND EXISTS (SELECT 1 FROM {source} WHERE {key} = target_key) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'row_identity_conflict';
    END IF;
    IF current_image = target_image THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'restore_no_change';
    END IF;

    SET plan = RANDOM_BYTES(16);
    INSERT INTO {journal}.restore_plans
        (plan_id, owner, requested_revision, base_revision, tracked_row, source_key, image)
    VALUES (plan, CURRENT_USER(), requested, watermark, stable_row, current_key,
            IF(selected_field IS NULL, target_image, JSON_SET(target_image, '$.revert_field', selected_field)));
    SET current_image = JSON_OBJECT('plan_id', LOWER(HEX(plan)),
        'current_revision', CAST(watermark AS CHAR CHARACTER SET ascii),
        'revision', CAST(requested AS CHAR CHARACTER SET ascii),
        'row_id', CAST(stable_row AS CHAR CHARACTER SET ascii),
        'before', current_image, 'after', target_image);
    IF selected_field IS NOT NULL THEN
        RETURN JSON_SET(current_image, '$.mode', 'revert', '$.field', selected_field);
    END IF;
    RETURN current_image;
END$$

CREATE FUNCTION {journal}.history_restore_preview(
    requested BIGINT UNSIGNED, stable_row BIGINT UNSIGNED
) RETURNS JSON SQL SECURITY INVOKER MODIFIES SQL DATA
RETURN {journal}.history_plan_preview(requested, stable_row, NULL)$$

CREATE FUNCTION {journal}.history_revert_preview(
    requested BIGINT UNSIGNED, stable_row BIGINT UNSIGNED,
    selected_field VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin
) RETURNS JSON SQL SECURITY INVOKER MODIFIES SQL DATA
BEGIN
    IF selected_field IS NULL THEN
        SIGNAL SQLSTATE '22023' SET MESSAGE_TEXT = 'invalid_restore_field';
    END IF;
    RETURN {journal}.history_plan_preview(requested, stable_row, selected_field);
END$$

CREATE FUNCTION {journal}.history_apply_plan(
    plan_text VARCHAR(64) CHARACTER SET ascii, token_text VARCHAR(64) CHARACTER SET ascii,
    revert_mode BOOLEAN
) RETURNS JSON SQL SECURITY INVOKER MODIFIES SQL DATA
BEGIN
    DECLARE plan BINARY(16);
    DECLARE token BINARY(16);
    DECLARE prior_token BINARY(16);
    DECLARE published BIGINT UNSIGNED;
    DECLARE watermark BIGINT UNSIGNED;
    DECLARE expected_head BIGINT UNSIGNED;
    DECLARE stable_row BIGINT UNSIGNED;
    DECLARE recorded_row BIGINT UNSIGNED;
    DECLARE expected_key BIGINT;
    DECLARE actual_key BIGINT;
    DECLARE mapped_key BIGINT;
    DECLARE selected_field VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin;
    DECLARE target_image JSON;
    DECLARE recorded_image JSON;

    IF plan_text IS NULL OR token_text IS NULL OR revert_mode IS NULL OR revert_mode NOT IN (0, 1)
       OR NOT REGEXP_LIKE(plan_text, '^[0-9a-f]{{32}}$', 'c')
       OR NOT REGEXP_LIKE(token_text, '^[0-9a-f]{{32}}$', 'c') THEN
        SIGNAL SQLSTATE '22023' SET MESSAGE_TEXT = 'invalid_restore_request';
    END IF;
    SET plan = UNHEX(plan_text);
    SET token = UNHEX(token_text);
    SELECT base_revision, tracked_row, source_key, JSON_REMOVE(image, '$.revert_field'),
           JSON_UNQUOTE(JSON_EXTRACT(image, '$.revert_field')), request_token, applied_revision
      INTO expected_head, stable_row, expected_key, target_image, selected_field, prior_token, published
      FROM {journal}.restore_plans WHERE plan_id = plan AND owner = CURRENT_USER()
      FOR UPDATE;
    IF stable_row IS NULL THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'restore_plan_not_found';
    END IF;
    IF (selected_field IS NOT NULL) <> revert_mode THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'request_conflict';
    END IF;
    IF published IS NOT NULL THEN
        IF NOT (prior_token <=> token) THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'request_conflict';
        END IF;
        RETURN JSON_OBJECT('revision', CAST(published AS CHAR CHARACTER SET ascii),
                           'replayed', CAST('true' AS JSON));
    END IF;
{guard}
    IF ({shape}) IS NOT TRUE THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'schema_conflict';
    END IF;

    SELECT {key} INTO actual_key FROM {source} WHERE {key} = expected_key FOR UPDATE;
    SELECT source_key INTO mapped_key FROM {journal}.row_identity WHERE row_id = stable_row FOR UPDATE;
    SELECT head INTO watermark FROM {journal}.revision_counter WHERE singleton = 1 FOR UPDATE;
    IF NOT (watermark <=> expected_head) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'stale_preview';
    END IF;
    IF actual_key IS NULL OR NOT (mapped_key <=> actual_key) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'row_identity_conflict';
    END IF;
    IF EXISTS (SELECT 1 FROM {journal}.restore_plans
               WHERE owner = CURRENT_USER() AND request_token = token AND plan_id <> plan) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'request_conflict';
    END IF;

    UPDATE {journal}.restore_plans SET request_token = token WHERE plan_id = plan;
{update_plan_row(table, fields)}

    SELECT head INTO published FROM {journal}.revision_counter WHERE singleton = 1;
    SELECT row_id, after_image INTO recorded_row, recorded_image FROM {journal}.events WHERE revision = published;
    IF NOT (published <=> watermark + 1) OR NOT (recorded_row <=> stable_row)
       OR NOT (recorded_image <=> target_image) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'restored_image_mismatch';
    END IF;
    UPDATE {journal}.restore_plans SET applied_revision = published WHERE plan_id = plan;
    RETURN JSON_OBJECT('revision', CAST(published AS CHAR CHARACTER SET ascii),
                       'replayed', CAST('false' AS JSON));
END$$

CREATE FUNCTION {journal}.history_restore_apply(
    plan_text VARCHAR(64) CHARACTER SET ascii, token_text VARCHAR(64) CHARACTER SET ascii
) RETURNS JSON SQL SECURITY INVOKER MODIFIES SQL DATA
RETURN {journal}.history_apply_plan(plan_text, token_text, FALSE)$$

CREATE FUNCTION {journal}.history_revert_apply(
    plan_text VARCHAR(64) CHARACTER SET ascii, token_text VARCHAR(64) CHARACTER SET ascii
) RETURNS JSON SQL SECURITY INVOKER MODIFIES SQL DATA
RETURN {journal}.history_apply_plan(plan_text, token_text, TRUE)$$
DELIMITER ;
"""
