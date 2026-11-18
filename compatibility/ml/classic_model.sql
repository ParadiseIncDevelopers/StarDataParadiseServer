-- Install with the Classic mysql client after registering sdp_model_compute.
CREATE SCHEMA IF NOT EXISTS sdp_models DEFAULT CHARACTER SET utf8mb4;

DELIMITER //

CREATE FUNCTION IF NOT EXISTS sdp_models.model_identifier(value TEXT)
RETURNS VARCHAR(66) CHARACTER SET ascii
DETERMINISTIC NO SQL SQL SECURITY INVOKER
BEGIN
    IF value IS NULL OR CHAR_LENGTH(value) NOT BETWEEN 1 AND 64
       OR NOT REGEXP_LIKE(value, '^[A-Za-z_]', 'c')
       OR REGEXP_LIKE(value, '[^A-Za-z0-9_]', 'c') THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Expected a simple SQL identifier for the model, registry or source';
    END IF;
    RETURN CONCAT('`', value, '`');
END//

CREATE FUNCTION IF NOT EXISTS sdp_models.model_json_identifier(input JSON, path VARCHAR(64))
RETURNS VARCHAR(66) CHARACTER SET ascii
DETERMINISTIC NO SQL SQL SECURITY INVOKER
BEGIN
    DECLARE value JSON DEFAULT JSON_EXTRACT(input, path);
    IF NOT (JSON_TYPE(value) <=> 'STRING') THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Expected a JSON string for the source schema, table or column';
    END IF;
    RETURN sdp_models.model_identifier(JSON_UNQUOTE(value));
END//

CREATE PROCEDURE IF NOT EXISTS sdp_models.run_model_sql(IN command_text LONGTEXT, IN parameters JSON, OUT output JSON)
SQL SECURITY INVOKER
BEGIN
    DECLARE statement_prepared BOOLEAN DEFAULT FALSE;
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        IF statement_prepared THEN
            DEALLOCATE PREPARE sdp_classic_model_stmt;
        END IF;
        SET @sdp_classic_model_sql = NULL, @sdp_classic_model_input = NULL, @sdp_classic_model_output = NULL;
        RESIGNAL;
    END;
    SET @sdp_classic_model_sql = command_text, @sdp_classic_model_input = CAST(parameters AS CHAR CHARACTER SET utf8mb4), @sdp_classic_model_output = NULL;
    PREPARE sdp_classic_model_stmt FROM @sdp_classic_model_sql;
    SET statement_prepared = TRUE;
    EXECUTE sdp_classic_model_stmt;
    DEALLOCATE PREPARE sdp_classic_model_stmt;
    SET statement_prepared = FALSE;
    SET output = CONVERT(@sdp_classic_model_output USING utf8mb4);
    SET @sdp_classic_model_sql = NULL, @sdp_classic_model_input = NULL, @sdp_classic_model_output = NULL;
END//

CREATE PROCEDURE IF NOT EXISTS sdp_models.install_model_registry(IN schema_name TEXT)
SQL SECURITY INVOKER
BEGIN
    DECLARE registry VARCHAR(66) DEFAULT sdp_models.model_identifier(schema_name);
    DECLARE unused JSON;
    CALL sdp_models.run_model_sql(CONCAT(
        'CREATE TABLE IF NOT EXISTS ', registry, '.sdp_model_definitions (',
        'name VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,',
        'algorithm VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL',
        ') ENGINE=InnoDB'), NULL, unused);
    CALL sdp_models.run_model_sql(CONCAT(
        'CREATE TABLE IF NOT EXISTS ', registry, '.sdp_model_versions (',
        'version BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,',
        'name VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,',
        'request_token CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,',
        'training_request JSON NOT NULL, artifact LONGTEXT CHARACTER SET utf8mb4 NOT NULL,',
        'sha256 CHAR(64) CHARACTER SET ascii NOT NULL,',
        'UNIQUE KEY (name, request_token), KEY (name, version),',
        'FOREIGN KEY (name) REFERENCES ', registry, '.sdp_model_definitions(name),',
        'CHECK (JSON_VALID(artifact))',
        ') ENGINE=InnoDB'), NULL, unused);
END//

CREATE PROCEDURE IF NOT EXISTS sdp_models.read_model_request(
    IN payload LONGTEXT, OUT request JSON, OUT registry VARCHAR(66), OUT model_name VARCHAR(64))
SQL SECURITY INVOKER
BEGIN
    DECLARE name_text TEXT;
    DECLARE registry_text TEXT;
    IF payload IS NULL OR OCTET_LENGTH(payload) > 65536 OR JSON_VALID(payload) = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'MODEL USING requires a JSON object of at most 64 KiB';
    END IF;
    SET request = CAST(payload AS JSON);
    IF JSON_TYPE(request) <> 'OBJECT' OR NOT (JSON_TYPE(JSON_EXTRACT(request, '$.name')) <=> 'STRING')
       OR (JSON_CONTAINS_PATH(request, 'one', '$.registry')
           AND NOT (JSON_TYPE(JSON_EXTRACT(request, '$.registry')) <=> 'STRING')) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'MODEL requires a JSON object with a name and optional registry';
    END IF;
    SET name_text = JSON_UNQUOTE(JSON_EXTRACT(request, '$.name'));
    SET registry_text = COALESCE(JSON_UNQUOTE(JSON_EXTRACT(request, '$.registry')), 'sdp_models');
    SET registry = sdp_models.model_identifier(registry_text);
    -- Validate before converting to the bounded VARCHAR used by the registry.
    SET model_name = TRIM(BOTH '`' FROM sdp_models.model_identifier(name_text));
END//

CREATE PROCEDURE IF NOT EXISTS sdp_models.model_feature_columns(IN features JSON, OUT column_list TEXT)
SQL SECURITY INVOKER
BEGIN
    DECLARE index_value INT DEFAULT 0;
    DECLARE feature_name TEXT;
    DECLARE seen JSON DEFAULT JSON_ARRAY();
    IF NOT (JSON_TYPE(features) <=> 'ARRAY') OR JSON_LENGTH(features) NOT BETWEEN 1 AND 64 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'TRAIN MODEL requires 1 to 64 feature columns';
    END IF;
    SET column_list = '';
    WHILE index_value < JSON_LENGTH(features) DO
        IF JSON_TYPE(JSON_EXTRACT(features, CONCAT('$[', index_value, ']'))) <> 'STRING' THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Feature columns must be strings';
        END IF;
        SET feature_name = JSON_UNQUOTE(JSON_EXTRACT(features, CONCAT('$[', index_value, ']')));
        IF JSON_CONTAINS(seen, JSON_QUOTE(LOWER(feature_name))) THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Feature columns must be distinct';
        END IF;
        SET seen = JSON_ARRAY_APPEND(seen, '$', LOWER(feature_name));
        SET column_list = CONCAT_WS(',', NULLIF(column_list, ''), sdp_models.model_identifier(feature_name));
        SET index_value = index_value + 1;
    END WHILE;
END//

CREATE PROCEDURE IF NOT EXISTS sdp_models.create_model(IN payload LONGTEXT)
SQL SECURITY INVOKER
BEGIN
    DECLARE request JSON;
    DECLARE registry VARCHAR(66);
    DECLARE model_name VARCHAR(64);
    DECLARE algorithm TEXT;
    DECLARE committed_algorithm JSON;
    CALL sdp_models.read_model_request(payload, request, registry, model_name);
    IF JSON_LENGTH(JSON_REMOVE(request, '$.name', '$.registry', '$.algorithm', '$.request_token')) <> 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Unknown CREATE MODEL option';
    END IF;
    SET algorithm = JSON_UNQUOTE(JSON_EXTRACT(request, '$.algorithm'));
    IF algorithm IS NULL OR BINARY algorithm NOT IN ('linear','logistic','tree','forest','kmeans','dbscan','pca','mlp') THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'CREATE MODEL requires a supported algorithm';
    END IF;
    BEGIN
        DECLARE CONTINUE HANDLER FOR 1062 BEGIN END;
        CALL sdp_models.run_model_sql(CONCAT(
            'INSERT INTO ', registry, '.sdp_model_definitions (name,algorithm) VALUES (',
            'JSON_UNQUOTE(JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.name'')),',
            'JSON_UNQUOTE(JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.algorithm'')))'),
            request, committed_algorithm);
    END;
    CALL sdp_models.run_model_sql(CONCAT(
        'SELECT JSON_QUOTE(algorithm) INTO @sdp_classic_model_output FROM ', registry,
        '.sdp_model_definitions WHERE name = JSON_UNQUOTE(JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.name'')) FOR SHARE'),
        request, committed_algorithm);
    IF NOT (BINARY JSON_UNQUOTE(committed_algorithm) <=> BINARY algorithm) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Model name already has a different algorithm';
    END IF;
    SELECT model_name AS name, algorithm AS algorithm;
END//

CREATE PROCEDURE IF NOT EXISTS sdp_models.train_model(IN payload LONGTEXT)
SQL SECURITY INVOKER
BEGIN
    DECLARE request JSON;
    DECLARE registry VARCHAR(66);
    DECLARE model_name VARCHAR(64);
    DECLARE token TEXT;
    DECLARE algorithm_json JSON;
    DECLARE algorithm VARCHAR(16);
    DECLARE source_schema VARCHAR(66);
    DECLARE source_table VARCHAR(66);
    DECLARE order_key VARCHAR(66);
    DECLARE label_column VARCHAR(66);
    DECLARE split_column VARCHAR(66);
    DECLARE feature_columns TEXT;
    DECLARE features JSON;
    DECLARE options JSON;
    DECLARE seed JSON;
    DECLARE previous_query TEXT;
    DECLARE published JSON;
    DECLARE rows_snapshot JSON;
    DECLARE model_result LONGTEXT;
    DECLARE artifact LONGTEXT;
    DECLARE training_request JSON;
    DECLARE unused JSON;

    CALL sdp_models.read_model_request(payload, request, registry, model_name);
    IF JSON_LENGTH(JSON_REMOVE(request, '$.name', '$.registry', '$.request_token', '$.schema', '$.table', '$.order_key',
                              '$.features', '$.label', '$.split_column', '$.options', '$.seed')) <> 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Unknown TRAIN MODEL option. Specify a source table and feature columns';
    END IF;
    SET token = JSON_UNQUOTE(JSON_EXTRACT(request, '$.request_token'));
    IF NOT (JSON_TYPE(JSON_EXTRACT(request, '$.request_token')) <=> 'STRING') OR CHAR_LENGTH(token) <> 32
       OR REGEXP_LIKE(token, '[^0-9a-fA-F]', 'c') THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'TRAIN MODEL requires a 32-digit hexadecimal request_token';
    END IF;
    SET request = JSON_SET(request, '$.request_token', LOWER(token));
    CALL sdp_models.run_model_sql(CONCAT(
        'SELECT JSON_QUOTE(algorithm) INTO @sdp_classic_model_output FROM ', registry,
        '.sdp_model_definitions WHERE name = JSON_UNQUOTE(JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.name''))'),
        request, algorithm_json);
    IF algorithm_json IS NULL THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Use CREATE MODEL before TRAIN MODEL';
    END IF;
    SET algorithm = JSON_UNQUOTE(algorithm_json);
    SET source_schema = sdp_models.model_json_identifier(request, '$.schema');
    SET source_table = sdp_models.model_json_identifier(request, '$.table');
    SET order_key = sdp_models.model_json_identifier(request, '$.order_key');
    IF JSON_CONTAINS_PATH(request, 'one', '$.label') THEN
        SET label_column = sdp_models.model_json_identifier(request, '$.label');
    END IF;
    IF JSON_CONTAINS_PATH(request, 'one', '$.split_column') THEN
        SET split_column = sdp_models.model_json_identifier(request, '$.split_column');
    END IF;
    IF ((algorithm IN ('kmeans','dbscan','pca')) <> (label_column IS NULL))
       OR ((algorithm = 'mlp') <> (split_column IS NOT NULL)) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Supervised training requires label; only MLP requires split_column';
    END IF;
    SET features = JSON_EXTRACT(request, '$.features');
    CALL sdp_models.model_feature_columns(features, feature_columns);
    SET options = COALESCE(JSON_EXTRACT(request, '$.options'), JSON_OBJECT());
    SET seed = COALESCE(JSON_EXTRACT(request, '$.seed'), CAST('42' AS JSON));
    IF JSON_TYPE(options) <> 'OBJECT' OR JSON_TYPE(seed) NOT IN ('INTEGER','UNSIGNED INTEGER')
       OR CAST(JSON_UNQUOTE(seed) AS DECIMAL(20,0)) NOT BETWEEN 0 AND 4294967295 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'TRAIN MODEL requires object options and an unsigned 32-bit seed';
    END IF;

    SET training_request = JSON_OBJECT('schema', JSON_UNQUOTE(JSON_EXTRACT(request, '$.schema')),
        'table', JSON_UNQUOTE(JSON_EXTRACT(request, '$.table')), 'order_key', JSON_UNQUOTE(JSON_EXTRACT(request, '$.order_key')),
        'label', TRIM(BOTH '`' FROM label_column), 'split_column', TRIM(BOTH '`' FROM split_column),
        'features', features, 'options', options, 'seed', seed);
    SET previous_query = CONCAT(
        'SELECT JSON_OBJECT(''version'',version,''sha256'',sha256,''request'',training_request) ',
        'INTO @sdp_classic_model_output FROM ', registry, '.sdp_model_versions ',
        'WHERE name = JSON_UNQUOTE(JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.name'')) ',
        'AND request_token = JSON_UNQUOTE(JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.request_token''))');
    CALL sdp_models.run_model_sql(previous_query, request, published);
    IF published IS NULL THEN
        CALL sdp_models.run_model_sql(CONCAT(
            'SELECT JSON_ARRAYAGG(JSON_ARRAY(row_order, duplicates, null_key, column_values, train_label, partition_label)) ',
            'INTO @sdp_classic_model_output FROM (SELECT ROW_NUMBER() OVER (ORDER BY row_key) AS row_order,',
            'COUNT(*) OVER (PARTITION BY row_key) AS duplicates,IF(row_key IS NULL,1,0) AS null_key,',
            'column_values, train_label, partition_label FROM (SELECT ', order_key, ' AS row_key,',
            'JSON_ARRAY(', feature_columns, ') AS column_values,', COALESCE(label_column,'NULL'), ' AS train_label,',
            COALESCE(split_column,'NULL'), ' AS partition_label FROM ', source_schema, '.', source_table,
            ' ORDER BY ', order_key, ' LIMIT 1025) AS bounded_source) AS training_rows'), NULL, rows_snapshot);
        SET model_result = CONVERT(sdp_model_compute(JSON_OBJECT('action','train', 'algorithm',algorithm,
            'rows',rows_snapshot, 'seed',seed, 'options',options)) USING utf8mb4);
        IF model_result IS NULL OR JSON_VALID(model_result) = 0 THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Model training rejected the selected data or options';
        END IF;
        SET artifact = CAST(JSON_OBJECT('source', training_request, 'model', CAST(model_result AS JSON)) AS CHAR CHARACTER SET utf8mb4);
        BEGIN
            DECLARE CONTINUE HANDLER FOR 1062 BEGIN END;
            CALL sdp_models.run_model_sql(CONCAT(
                'INSERT INTO ', registry, '.sdp_model_versions (name,request_token,training_request,artifact,sha256) ',
                'VALUES (JSON_UNQUOTE(JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.name'')),',
                'JSON_UNQUOTE(JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.request_token'')),',
                'JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.request''),',
                'JSON_UNQUOTE(JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.artifact'')),',
                'SHA2(JSON_UNQUOTE(JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.artifact'')),256))'),
                JSON_OBJECT('name', model_name, 'request_token', LOWER(token), 'request', training_request, 'artifact', artifact), unused);
        END;
        CALL sdp_models.run_model_sql(CONCAT(previous_query, ' FOR SHARE'), request, published);
    END IF;
    IF NOT (JSON_EXTRACT(published, '$.request') <=> training_request) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'The request_token already identifies a different training request';
    END IF;
    SELECT model_name AS name, CAST(JSON_UNQUOTE(JSON_EXTRACT(published, '$.version')) AS UNSIGNED) AS version,
           JSON_UNQUOTE(JSON_EXTRACT(published, '$.sha256')) AS sha256;
END//

CREATE PROCEDURE IF NOT EXISTS sdp_models.predict_model(IN payload LONGTEXT)
SQL SECURITY INVOKER
BEGIN
    DECLARE request JSON;
    DECLARE registry VARCHAR(66);
    DECLARE model_name VARCHAR(64);
    DECLARE selected_version JSON;
    DECLARE loaded JSON;
    DECLARE artifact LONGTEXT;
    DECLARE source_info JSON;
    DECLARE feature_names JSON;
    DECLARE supplied JSON;
    DECLARE feature_values JSON DEFAULT JSON_ARRAY();
    DECLARE feature_columns TEXT;
    DECLARE feature_name TEXT;
    DECLARE feature_value JSON;
    DECLARE feature_index INT DEFAULT 0;
    DECLARE order_key VARCHAR(66);
    DECLARE label_column TEXT;
    DECLARE split_column TEXT;
    DECLARE algorithm VARCHAR(16);
    DECLARE unused JSON;
    DECLARE prediction LONGTEXT;

    CALL sdp_models.read_model_request(payload, request, registry, model_name);
    IF JSON_LENGTH(JSON_REMOVE(request, '$.name', '$.registry', '$.version', '$.features')) <> 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Unknown PREDICT MODEL option';
    END IF;
    SET selected_version = JSON_EXTRACT(request, '$.version');
    IF selected_version IS NOT NULL AND (JSON_TYPE(selected_version) NOT IN ('INTEGER','UNSIGNED INTEGER')
       OR CAST(JSON_UNQUOTE(selected_version) AS DECIMAL(20,0)) NOT BETWEEN 1 AND 18446744073709551615) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'PREDICT MODEL version must be a positive integer';
    END IF;
    CALL sdp_models.run_model_sql(CONCAT(
        'SELECT JSON_OBJECT(''version'',v.version,''artifact'',v.artifact,''sha256'',v.sha256,''algorithm'',d.algorithm) ',
        'INTO @sdp_classic_model_output FROM ', registry, '.sdp_model_versions AS v JOIN ', registry,
        '.sdp_model_definitions AS d USING (name) ',
        'WHERE v.name = JSON_UNQUOTE(JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.name''))',
        IF(selected_version IS NULL, '', ' AND v.version = CAST(JSON_UNQUOTE(JSON_EXTRACT(CONVERT(@sdp_classic_model_input USING utf8mb4),''$.version'')) AS UNSIGNED)'),
        ' ORDER BY v.version DESC LIMIT 1'), request, loaded);
    SET artifact = JSON_UNQUOTE(JSON_EXTRACT(loaded, '$.artifact'));
    SET algorithm = JSON_UNQUOTE(JSON_EXTRACT(loaded, '$.algorithm'));
    IF loaded IS NULL OR NOT (SHA2(artifact, 256) <=> JSON_UNQUOTE(JSON_EXTRACT(loaded, '$.sha256'))) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Model version is missing or its artifact checksum differs';
    END IF;
    IF algorithm = 'dbscan' THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'DBSCAN does not predict new records';
    END IF;

    SET source_info = JSON_EXTRACT(artifact, '$.source');
    SET feature_names = JSON_EXTRACT(source_info, '$.features');
    CALL sdp_models.model_feature_columns(feature_names, feature_columns);
    SET order_key = sdp_models.model_identifier(JSON_UNQUOTE(JSON_EXTRACT(source_info, '$.order_key')));
    SET label_column = IF(JSON_TYPE(JSON_EXTRACT(source_info, '$.label')) = 'STRING',
        sdp_models.model_identifier(JSON_UNQUOTE(JSON_EXTRACT(source_info, '$.label'))), NULL);
    SET split_column = IF(JSON_TYPE(JSON_EXTRACT(source_info, '$.split_column')) = 'STRING',
        sdp_models.model_identifier(JSON_UNQUOTE(JSON_EXTRACT(source_info, '$.split_column'))), NULL);
    -- Preparing the SELECT checks the invoker's current grants on every training column, including when no rows are read.
    CALL sdp_models.run_model_sql(CONCAT(
        'SELECT COUNT(*) INTO @sdp_classic_model_output FROM (SELECT JSON_ARRAY(',
        CONCAT_WS(',',order_key,feature_columns,label_column,split_column), ') AS sample FROM ',
        sdp_models.model_identifier(JSON_UNQUOTE(JSON_EXTRACT(source_info, '$.schema'))), '.',
        sdp_models.model_identifier(JSON_UNQUOTE(JSON_EXTRACT(source_info, '$.table'))),
        ' LIMIT 0) AS source_access'), NULL, unused);

    SET supplied = JSON_EXTRACT(request, '$.features');
    IF NOT (JSON_TYPE(supplied) <=> 'OBJECT') OR JSON_LENGTH(supplied) <> JSON_LENGTH(feature_names) THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'PREDICT MODEL requires the trained feature names';
    END IF;
    WHILE feature_index < JSON_LENGTH(feature_names) DO
        SET feature_name = JSON_UNQUOTE(JSON_EXTRACT(feature_names, CONCAT('$[',feature_index,']')));
        SET feature_value = JSON_EXTRACT(supplied, CONCAT('$.', feature_name));
        IF feature_value IS NULL OR JSON_TYPE(feature_value) NOT IN ('INTEGER','UNSIGNED INTEGER','DOUBLE','DECIMAL') THEN
            SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'PREDICT MODEL requires numeric values for the trained feature names';
        END IF;
        SET feature_values = JSON_ARRAY_APPEND(feature_values, '$', feature_value);
        SET feature_index = feature_index + 1;
    END WHILE;
    SET prediction = CONVERT(sdp_model_compute(JSON_OBJECT('action','predict','algorithm',algorithm,
        'model',JSON_EXTRACT(artifact, '$.model'), 'features',feature_values)) USING utf8mb4);
    IF prediction IS NULL OR JSON_VALID(prediction) = 0 THEN
        SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Model prediction rejected the input or artifact';
    END IF;
    SELECT model_name AS name, CAST(JSON_UNQUOTE(JSON_EXTRACT(loaded, '$.version')) AS UNSIGNED) AS version,
           CAST(prediction AS JSON) AS prediction;
END//

DELIMITER ;

CALL sdp_models.install_model_registry('sdp_models');
