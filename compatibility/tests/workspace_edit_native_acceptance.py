"""Execute actual Workbench edit builders against disposable native AI catalogs.

Requires the Knowledge collation roundtrip fix. No user profile, provider or
credential is accessed; this is independent of the tool/action guard suites.
"""
import argparse
from decimal import Decimal
import importlib.util
import json
from pathlib import Path
import sys

from .ai_catalog_acceptance import execute, install
from .fixture import BASEDIR, BIN, MIGRATION, SHELL, Fixture, query, require
from .workbench_native import backend_certificate


def backend_module():
    workbench = Path(__file__).resolve().parents[3] / 'StarDataParadise'
    directory = workbench / 'classic-workbench/modules/wb.utils'
    if str(directory) not in sys.path:
        sys.path.insert(0, str(directory))
    spec = importlib.util.spec_from_file_location('workspace_edit_native_backend', directory / 'sdp_workspace_backend.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def exercise(instance, full_collation=True):
    instance=(instance[0],[*instance[1],'--default-character-set=utf8mb4'],instance[2])
    ui = backend_module()
    def describe(kind, name):
        return json.loads(execute(instance, 'DESCRIBE '+kind+' fixture.'+name+';'), parse_float=Decimal)
    install(instance, False)
    execute(instance, """CREATE TABLE fixture.edit_rows(id BIGINT UNSIGNED PRIMARY KEY,note VARCHAR(40) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NULL) ENGINE=InnoDB;
      INSERT INTO fixture.edit_rows VALUES(18446744073709551615,'satış'),(7,NULL);
      CREATE BEHAVIOR fixture.edit_b AS 'original';
      CREATE RESPONSE SCHEMA fixture.edit_s VERSION 1 AS JSON '{"type":"number","minimum":0.1}';
    """)
    if full_collation:
        execute(instance, """CREATE KNOWLEDGE fixture.edit_k(p BIGINT UNSIGNED NOT NULL,hint VARCHAR(20) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NULL)
      RETURNS TABLE(label VARCHAR(40) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NULL,id BIGINT UNSIGNED NOT NULL,metric DECIMAL(38,18) NOT NULL)
      READ ONLY SQL SECURITY INVOKER AS SELECT note AS label,id,CAST(0.100000000000000001 AS DECIMAL(38,18)) AS metric
        FROM fixture.edit_rows WHERE id=:p AND (:hint IS NULL OR note=:hint);""")
    else:
        execute(instance, """CREATE KNOWLEDGE fixture.edit_k(p BIGINT UNSIGNED NOT NULL)
      RETURNS TABLE(id BIGINT UNSIGNED NOT NULL,metric DECIMAL(38,18) NOT NULL,nullable_id BIGINT UNSIGNED NULL)
      READ ONLY SQL SECURITY INVOKER AS SELECT id,CAST(0.100000000000000001 AS DECIMAL(38,18)) AS metric,id AS nullable_id
        FROM fixture.edit_rows WHERE id=:p;""")
    knowledge = describe('KNOWLEDGE', 'edit_k')
    require(knowledge['version'] == 1, 'Initial Knowledge version mismatch')
    original_signature = {key: knowledge[key] for key in ('parameters', 'result_columns')}
    if full_collation:
        require('COLLATE `utf8mb4_bin`' in knowledge['result_columns'][0]['type'], 'Native nondefault collation metadata missing')
        edited_query = ("SELECT CONCAT(note,'; edited') AS label,id,CAST(0.200000000000000001 AS DECIMAL(38,18)) AS metric\n"
                        "FROM fixture.edit_rows WHERE id=:p AND (:hint IS NULL OR note=:hint) -- retain final comment")
    else:
        edited_query = ("SELECT id,CAST(0.200000000000000001 AS DECIMAL(38,18)) AS metric,NULL AS nullable_id\n"
                        "FROM fixture.edit_rows WHERE id=:p -- retain final comment")
    source = ui.object_edit_sql('Knowledge', 'fixture', 'edit_k', knowledge, edited_query)
    execute(instance, source)
    knowledge2 = describe('KNOWLEDGE', 'edit_k')
    require(knowledge2['version'] == 2, 'Workbench Knowledge edit did not ALTER the object')
    require({key: knowledge2[key] for key in original_signature} == original_signature,
            'Knowledge edit changed parameter/result types, charset/collation or nullability')
    if full_collation:
        require(execute(instance, 'EXECUTE KNOWLEDGE fixture.edit_k(18446744073709551615,NULL);') ==
                'satış; edited\t18446744073709551615\t0.200000000000000001', 'Edited Knowledge body lost uint64/Decimal/Unicode data')
        require(execute(instance, 'EXECUTE KNOWLEDGE fixture.edit_k(7,NULL);') ==
                'NULL\t7\t0.200000000000000001', 'Edited Knowledge result lost SQL NULL')
        require(execute(instance, 'SELECT COLLATION(k.label) FROM KNOWLEDGE fixture.edit_k(18446744073709551615,NULL) AS k;') ==
                'utf8mb4_bin', 'Edited Knowledge changed the actual typed result collation')
        print('PASS Workbench native Knowledge ALTER version2, complete canonical signature, charset/collation, SQL NULL, uint64 and Decimal', flush=True)
    else:
        require(execute(instance, 'EXECUTE KNOWLEDGE fixture.edit_k(18446744073709551615);') ==
                '18446744073709551615\t0.200000000000000001\tNULL', 'Edited numeric Knowledge body lost uint64/Decimal/NULL')
        print('PASS Workbench native numeric Knowledge ALTER version2, declared nullability, uint64 and Decimal; collation gate NOT_RUN', flush=True)

    behavior = describe('BEHAVIOR', 'edit_b')
    body = "Türkçe cevap ver.'; DELETE FROM fixture.edit_rows; --"
    execute(instance, ui.object_edit_sql('Behavior', 'fixture', 'edit_b', behavior, body))
    behavior2 = describe('BEHAVIOR', 'edit_b')
    require(behavior2['version'] == 2 and behavior2['definition'] == body,
            'Workbench Behavior edit lost text or did not ALTER the object')
    require(query(instance, 'SELECT COUNT(*) FROM fixture.edit_rows') == '2', 'Behavior text escaped into a SQL command')
    print('PASS Workbench native Behavior ALTER version2, exact Unicode/quotes/semicolon text and unchanged business rows', flush=True)

    schema = describe('RESPONSE SCHEMA', 'edit_s')
    require(schema['definition']['minimum'] == Decimal('0.1'), 'Initial response schema number mismatch')
    schema['definition']['maximum'] = Decimal('1.25')
    schema['definition']['title'] = "Türkçe'; DELETE FROM fixture.edit_rows; --"
    text = ui.definition_text('Yanıt şemaları', schema)
    require(json.loads(text, parse_float=Decimal)['minimum'] == Decimal('0.1'),
            'Definition editor converted a Decimal schema number to a string')
    execute(instance, ui.object_edit_sql('Yanıt şemaları', 'fixture', 'edit_s', schema, text))
    schema2 = describe('RESPONSE SCHEMA', 'edit_s')
    require(schema2['version'] == 2 and schema2['definition']['minimum'] == Decimal('0.1') and
            schema2['definition']['maximum'] == Decimal('1.25') and
            schema2['definition']['title'] == schema['definition']['title'], 'Schema edit changed version/number/string types')
    require(query(instance, "SELECT JSON_TYPE(JSON_EXTRACT(document,'$.definition.minimum')) FROM sdp_ai.objects WHERE kind='response_schema' AND name='edit_s'") in ('DOUBLE', 'DECIMAL'),
            'Edited schema minimum is not stored as a JSON number')
    require(query(instance, 'SELECT COUNT(*) FROM fixture.edit_rows') == '2', 'Schema text escaped into a SQL command')
    print('PASS Workbench existing response_schema dispatch ALTER version2, Decimal JSON number types, exact strings and unchanged business rows', flush=True)

    for provider, name in [('OPENAI', 'edit_openai'), ('ANTHROPIC', 'edit_anthropic')]:
        statement = ui.model_registration('fixture', name, provider, 'fixture-remote', credential_ref='fixture-reference')
        execute(instance, statement)
        model = describe('MODEL', name)
        require(model['version'] == 1 and model['provider'] == provider.lower() and model['remote_model'] == 'fixture-remote' and
                model['settings']['credential_ref'] == 'fixture-reference', 'UI cloud model SQL literal/metadata roundtrip failed: '+provider)
    statement = ui.model_registration('fixture', 'edit_local', 'LOCAL_GGUF', artifact='sample.gguf', sha256='a'*64)
    execute(instance, statement)
    model = describe('MODEL', 'edit_local')
    require(model['state'] == 'pending_import' and model['provider'] == 'local_gguf' and model['settings']['sha256'] == 'a'*64,
            'UI GGUF import did not remain pending metadata')
    require(query(instance, "SELECT COUNT(*) FROM sdp_ai.model_imports WHERE state='queued'") == '1', 'Pending model import metadata missing')
    require(query(instance, 'SELECT COUNT(*) FROM sdp_ai.runs') == '0', 'Model registration unexpectedly scheduled inference')
    print('PASS actual UI OPENAI/ANTHROPIC CREATE MODEL expressions and GGUF IMPORT pending metadata; provider/model I/O NOT_RUN', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld', type=Path, required=True)
    parser.add_argument('--mysql', type=Path, default=BIN / 'mysql.exe')
    parser.add_argument('--basedir', type=Path, default=BASEDIR)
    parser.add_argument('--skip-collation', action='store_true', help='Run the numeric Knowledge and B/R/model builder slice on build13; full collation acceptance still requires build14')
    args = parser.parse_args()
    fixture = Fixture(args.mysqld.resolve(strict=True), args.mysql.resolve(strict=True), args.basedir.resolve(strict=True), SHELL, None)
    with fixture.scratch('workspace-edit-native') as root:
        cert = backend_certificate(MIGRATION / 'toolchain/openssl-firedaemon/x64/bin/openssl.exe', root, 'cert')
        with fixture.disposable(root / 'server', tls_dir=cert, bootstrap_options=('--sdp-ai-sql=ON',),
                                 server_options=('--sdp-ai-sql=ON',)) as instance:
            exercise(instance, not args.skip_collation)


if __name__ == '__main__':
    main()
