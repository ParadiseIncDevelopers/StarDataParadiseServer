"""Worker API contract fixtures; database/native acceptance is separate."""
import copy
from contextlib import contextmanager
import json
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest

from compatibility.ai.contracts import AiError
from compatibility.ai.providers import ProviderResult
from compatibility.ai.secrets import SecretStore
from compatibility.ai.worker import RunWorker


class Rows:
    def __init__(self, row): self.row = row
    def fetch_one(self): return self.row


class Session:
    def __init__(self, claim, *, transaction=False, cancel=False, import_claim=None, ai_context=False,
                 tool_failure=None, tool_receipt=None):
        self.claim, self.transaction, self.cancel, self.events, self.publications = claim, transaction, cancel, [], []
        self.import_claim, self.ai_context = import_claim, ai_context
        self.tool_failure,self.tool_receipt=tool_failure,tool_receipt
    def run_sql(self, statement, parameters=()):
        self.events.append(statement)
        if statement=="SET SESSION time_zone='+00:00'": return Rows(None)
        if statement=='SET SESSION sdp_ai_worker_rpc_guard=ON': return Rows(None)
        if statement.startswith('SELECT @@autocommit'): return Rows((1, int(self.transaction), int(not self.ai_context)))
        if 'claim_model_import' in statement: return Rows((json.dumps(self.import_claim),)) if self.import_claim else Rows(None)
        if 'renew_model_import' in statement: return Rows((json.dumps({'cancel_requested': self.cancel}),))
        if 'publish_model_import' in statement:
            self.publications.append(parameters)
            return Rows((json.dumps({'job_id': parameters[0], 'state': 'failed' if json.loads(parameters[3]) else 'succeeded'}),))
        if 'claim_run' in statement: return Rows((json.dumps(self.claim),)) if self.claim else Rows(None)
        if 'renew_lease' in statement: return Rows((json.dumps({'cancel_requested': self.cancel}),))
        if 'publish_run' in statement:
            self.publications.append(parameters)
            error = json.loads(parameters[4])
            return Rows((json.dumps({'run_id': parameters[0], 'state': error['run_state'] if error else 'succeeded'}),))
        if 'publish_tool_requests' in statement:
            self.publications.append(parameters)
            return Rows((json.dumps({'run_id': parameters[0], 'state': 'waiting_tool'}),))
        if statement == 'EXECUTE AI TOOL CALL ?':
            if self.tool_failure: raise self.tool_failure
            self.publications.append(parameters)
            return Rows((json.dumps({'run_id': self.claim['run_id'], 'state': 'queued'}),))
        if 'publish_tool_failure' in statement:
            self.publications.append(parameters)
            return Rows((json.dumps({'run_id':parameters[0],'call_id':parameters[2],'state':'queued','tool_state':'failed'}),))
        if 'tool_call_status' in statement: return Rows((json.dumps(self.tool_receipt),))
        raise AssertionError(statement)


class Client:
    def __init__(self, session, *, text='answer', failure=None):
        self.session, self.text, self.failure, self.calls = session, text, failure, 0
    def generate(self, model, messages, **kwargs):
        self.calls += 1
        if self.session.transaction: raise AssertionError('SQL transaction spans provider call')
        if not self.session.events[-1].startswith('CALL sdp_ai.renew_lease'): raise AssertionError('lease/cancel not rechecked')
        if self.failure: raise self.failure
        return ProviderResult(self.text, model.provider, model.remote_model, 'fixture', {})


class RuntimeFixture:
    def __init__(self, executable, store, **limits): self.store = store
    @contextmanager
    def start(self, artifact_id, **options):
        self.store.resolve(artifact_id)
        yield {'fixture': True}


class WorkerTests(unittest.TestCase):
    def claim(self):
        return {'run_id': '1'*32, 'job_id': '2'*32, 'owner': 'alice@localhost', 'epoch': 1,
                'model': {'provider': 'openai', 'remote_model': 'chosen', 'credential_ref': 'my-key'},
                'messages': [{'role': 'user', 'content': 'question'}], 'response_format': 'TEXT'}

    def test_worker_rejects_existing_transaction_without_rollback(self):
        session = Session(self.claim(), transaction=True)
        with self.assertRaises(AiError): RunWorker(session, {}).work_once()
        self.assertEqual(1, len(session.events))
        self.assertNotIn('ROLLBACK', session.events)

    def test_worker_pins_metadata_timezone_only_after_clean_session_check(self):
        session=Session(None)
        self.assertIsNone(RunWorker(session,{}).work_once())
        first=session.events.index("SET SESSION time_zone='+00:00'")
        self.assertTrue(session.events[first-1].startswith('SELECT @@autocommit'))
        self.assertEqual('SET SESSION sdp_ai_worker_rpc_guard=ON',session.events[first+1])

    def test_typed_json_publication_is_separate_from_provider_io(self):
        claim = self.claim()
        claim['response_format'] = 'JSON'
        claim['response_schema'] = {'type': 'object', 'required': ['id']}
        session = Session(claim)
        client = Client(session, text='{"id":9007199254740993}')
        result = RunWorker(session, {}, client=client).work_once()
        self.assertEqual('succeeded', result['state'])
        self.assertEqual(9007199254740993, json.loads(session.publications[0][2])['value']['id'])
        self.assertIsNone(json.loads(session.publications[0][4]))
        self.assertEqual(1, client.calls)

    def test_schema_failure_publishes_error_without_capture(self):
        claim = self.claim()
        claim.update(response_format='JSON', response_schema={'type': 'object'})
        session = Session(claim)
        result = RunWorker(session, {}, client=Client(session, text='[]')).work_once()
        self.assertEqual('failed', result['state'])
        self.assertIsNone(json.loads(session.publications[0][2]))
        error = json.loads(session.publications[0][4])
        self.assertEqual(504003, error['code'])
        self.assertEqual('committed', error['commit_state'])

    def test_cancel_before_provider_has_no_external_effect(self):
        session = Session(self.claim(), cancel=True)
        client = Client(session)
        self.assertEqual('cancelled', RunWorker(session, {}, client=client).work_once()['state'])
        self.assertEqual(0, client.calls)

    def test_ambiguous_timeout_is_not_retried(self):
        claim = self.claim()
        claim['options'] = {'max_attempts': 3}
        session = Session(claim)
        client = Client(session, failure=AiError('provider_timeout', unknown_outcome=True))
        RunWorker(session, {}, client=client).work_once()
        self.assertEqual(1, client.calls)
        self.assertTrue(json.loads(session.publications[0][4])['unknown_outcome'])

    def test_no_claim_does_not_publish_result(self):
        session = Session(None)
        self.assertIsNone(RunWorker(session, {}).work_once())
        self.assertEqual([], session.publications)

    def test_credential_alias_is_bound_to_owner(self):
        secrets = SecretStore({'alice@localhost': {'one': {'source': 'environment', 'name': 'TEST_ONE'}}},
                              environment={'TEST_ONE': 'fixture', 'OTHER_KEY': 'unrelated'})
        self.assertEqual('fixture', secrets.resolve('alice@localhost', 'one'))
        for actor, alias in (('bob@localhost', 'one'), ('alice@localhost', 'OTHER_KEY')):
            with self.assertRaises(AiError): secrets.resolve(actor, alias)

    def test_worker_rejects_staged_application_conversation(self):
        session = Session(self.claim(), ai_context=True)
        with self.assertRaises(AiError): RunWorker(session, {}).work_once()
        self.assertEqual(1, len(session.events))

    def test_local_import_is_published_only_after_owner_path_and_digest_check(self):
        output = Path(__file__).resolve().parents[2] / 'out' / 'ai-provider-tests'
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as directory:
            root = Path(directory)
            source = root / 'uploads' / 'fixture.gguf'
            source.parent.mkdir()
            source.write_bytes(struct.pack('<4sIQQ', b'GGUF', 3, 0, 0))
            claim = {'job_id': '3'*32, 'owner': 'alice@localhost', 'relative_source': 'fixture.gguf',
                     'sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
            session = Session(None, import_claim=claim)
            config = {'local_runtime': {'executable': 'runtime-fixture', 'import_roots': {'alice@localhost': str(source.parent)}, 'model_store': str(root/'store')}}
            result = RunWorker(session, config, runtime_factory=RuntimeFixture).work_once()
            self.assertEqual('succeeded', result['state'])
            artifact = json.loads(session.publications[0][2])
            self.assertEqual(claim['sha256'], artifact['artifact_id'])
            self.assertTrue(artifact['runtime_verified'])
            self.assertEqual(source.read_bytes(), (root/'store'/(artifact['artifact_id']+'.gguf')).read_bytes())
            self.assertLess(session.events.index('CALL sdp_ai.renew_model_import(?, ?, ?)'),
                            session.events.index('CALL sdp_ai.publish_model_import(?, ?, ?, ?)'))

    def test_local_import_rejects_other_owner_and_parent_path_without_read(self):
        claim = {'job_id': '3'*32, 'owner': 'bob@localhost', 'relative_source': '../private.gguf', 'sha256': '0'*64}
        session = Session(None, import_claim=claim)
        result = RunWorker(session, {'local_runtime': {'import_roots': {'alice@localhost': 'irrelevant'}, 'model_store': 'unused'}}).work_once()
        self.assertEqual('failed', result['state'])
        self.assertIsNone(json.loads(session.publications[0][2]))

    def test_local_import_cancellation_precedes_copy_and_publishes_no_artifact(self):
        output = Path(__file__).resolve().parents[2] / 'out' / 'ai-provider-tests'
        with tempfile.TemporaryDirectory(dir=output) as directory:
            root = Path(directory)
            source = root/'fixture.gguf'
            source.write_bytes(struct.pack('<4sIQQ', b'GGUF', 3, 0, 0))
            claim = {'job_id': '3'*32, 'owner': 'alice@localhost', 'relative_source': source.name, 'sha256': '0'*64}
            session = Session(None, import_claim=claim, cancel=True)
            config = {'local_runtime': {'executable': 'unused', 'import_roots': {'alice@localhost': str(root)}, 'model_store': str(root/'store')}}
            self.assertEqual('failed', RunWorker(session, config).work_once()['state'])
            self.assertEqual([], list((root/'store').iterdir()))
            self.assertEqual(511004, json.loads(session.publications[0][3])['code'])

    def test_hash_only_artifact_does_not_publish_when_runtime_cannot_load(self):
        output = Path(__file__).resolve().parents[2] / 'out' / 'ai-provider-tests'
        with tempfile.TemporaryDirectory(dir=output) as directory:
            root = Path(directory)
            source = root/'fixture.gguf'
            source.write_bytes(struct.pack('<4sIQQ', b'GGUF', 3, 0, 0))
            claim = {'job_id': '3'*32, 'owner': 'alice@localhost', 'relative_source': source.name,
                     'sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
            session = Session(None, import_claim=claim)
            config = {'local_runtime': {'executable': str(root/'missing.exe'), 'import_roots': {'alice@localhost': str(root)}, 'model_store': str(root/'store')}}
            self.assertEqual('failed', RunWorker(session, config).work_once()['state'])
            self.assertIsNone(json.loads(session.publications[0][2]))
            self.assertEqual(509001, json.loads(session.publications[0][3])['code'])

    def test_tool_requests_are_one_atomic_typed_batch_without_sql_execution(self):
        claim = self.claim()
        name = 'tool_'+'a'*32
        claim['functions'] = True
        claim['model']['capabilities'] = ['tools']
        claim['tools'] = [{'provider_name': name, 'parameters': {'type': 'object',
                           'properties': {'id': {'type': 'integer'}}, 'required': ['id'], 'additionalProperties': False}}]
        session = Session(claim)
        class ToolClient(Client):
            def generate(self, model, messages, **kwargs):
                super().generate(model, messages, **kwargs)
                return ProviderResult('', model.provider, model.remote_model, 'request1', {},
                    ({'id': 'provider1', 'name': name, 'arguments': {'id': 42}},
                     {'id': 'provider2', 'name': name, 'arguments': {'id': 43}}))
        self.assertEqual('waiting_tool', RunWorker(session, {}, client=ToolClient(session)).work_once()['state'])
        self.assertEqual(1, len(session.publications))
        calls = json.loads(session.publications[0][4])
        self.assertEqual([42,43], [call['arguments']['id'] for call in calls])
        self.assertFalse(any(statement.startswith('EXECUTE') for statement in session.events))

    def test_empty_explicit_allowlist_generates_without_implicit_tools(self):
        claim=self.claim()
        claim.update(functions=True,tools=[])
        session=Session(claim)
        class NoToolsClient(Client):
            def generate(self,model,messages,**kwargs):
                if kwargs['tools']!=[]:raise AssertionError('Empty allowlist exposed implicit tools')
                return super().generate(model,messages,**kwargs)
        client=NoToolsClient(session)
        self.assertEqual('succeeded',RunWorker(session,{},client=client).work_once()['state'])
        self.assertEqual(1,client.calls)
        self.assertFalse(any(statement.startswith('EXECUTE') or 'publish_tool_requests' in statement for statement in session.events))

    def test_tools_cannot_choose_unregistered_sql_or_wrong_typed_arguments(self):
        for registered, arguments in ((False, {'id': 1}), (True, {'id': "1; DELETE FROM orders"})):
            claim = self.claim()
            name = 'tool_'+'a'*32
            claim.update(functions=True, tools=[{'provider_name': name, 'parameters': {'type': 'object',
                'properties': {'id': {'type': 'integer'}}, 'required': ['id']}}])
            claim['model']['capabilities'] = ['tools']
            session = Session(claim)
            class ToolClient(Client):
                def generate(self, model, messages, **kwargs):
                    return ProviderResult('', model.provider, model.remote_model, None, {},
                        ({'id': 'call1', 'name': name if registered else 'DROP_TABLE', 'arguments': arguments},))
            self.assertEqual('failed', RunWorker(session, {}, client=ToolClient(session)).work_once()['state'])
            self.assertFalse(any('publish_tool_requests' in statement or statement.startswith('EXECUTE') for statement in session.events))

    def test_tool_execution_sends_only_durable_id_and_waiting_approval_does_not_execute(self):
        claim = self.claim()
        claim.update(phase='tools', tool_calls_to_execute=[{'call_id': 'a'*32, 'state': 'ready', 'ordinal': 0}])
        session = Session(claim)
        self.assertEqual('queued', RunWorker(session, {}).work_once()['state'])
        self.assertEqual([['a'*32]], session.publications)
        claim['tool_calls_to_execute'][0]['state'] = 'waiting_approval'
        session = Session(claim)
        self.assertEqual('waiting_approval', RunWorker(session, {}).work_once()['state'])
        self.assertEqual([], session.publications)

    def test_tool_batch_executes_durable_ordinal_order_and_rejects_ambiguous_order(self):
        claim = self.claim()
        claim.update(phase='tools', tool_calls_to_execute=[
            {'call_id': 'b'*32, 'state': 'ready', 'ordinal': 2},
            {'call_id': 'a'*32, 'state': 'ready', 'ordinal': 1}])
        session = Session(claim)
        RunWorker(session, {}).work_once()
        self.assertEqual([['a'*32], ['b'*32]], session.publications)
        for ordinal in (None, True, -1, 2):
            claim['tool_calls_to_execute'][1]['ordinal'] = ordinal
            session = Session(claim)
            with self.assertRaises(AiError): RunWorker(session, {})._execute_tools(claim)
            self.assertEqual([], session.events)

    def tool_failure(self, **changes):
        class NativeError(Exception): pass
        failure=NativeError('SQL text and sensitive arguments must not reach the transcript')
        fields=dict(code=510005,native_errno=61085,phase='tool',commit_state='committed',
                    run_id='1'*32,call_id='a'*32,action_commit_state='rolled_back',
                    unknown_outcome=False,cause_native_errno=3024,cause_sqlstate='HY000')
        fields.update(changes)
        for key,value in fields.items():
            setattr(failure,key,value)
        return failure

    def tool_claim(self):
        claim=self.claim()
        claim.update(phase='tools',tool_calls_to_execute=[{'call_id':'a'*32,'state':'ready','ordinal':0}])
        return claim

    def test_known_native_rollback_publishes_sanitized_failure_once(self):
        session=Session(self.tool_claim(),tool_failure=self.tool_failure())
        result=RunWorker(session,{}).work_once()
        self.assertEqual('queued',result['state'])
        self.assertEqual(1,session.events.count('EXECUTE AI TOOL CALL ?'))
        self.assertEqual(1,len(session.publications))
        error=json.loads(session.publications[0][3])
        self.assertEqual(61085,error['native_errno'])
        self.assertEqual(3024,error['cause_native_errno'])
        self.assertEqual('rolled_back',error['action_commit_state'])
        self.assertEqual('Tool transaction rolled back.',error['message'])

    def test_mysqlsh_classic_rollback_envelope_has_same_failure_contract(self):
        class ShellError(Exception): code=61085
        detail={'phase':'tool','commit_state':'committed','run_id':'1'*32,'call_id':'a'*32,
                'action_commit_state':'rolled_back','unknown_outcome':False,'cause_native_errno':3024}
        error=ShellError(61085,'510005: '+json.dumps(detail),'45000')
        session=Session(self.tool_claim(),tool_failure=error)
        self.assertEqual('queued',RunWorker(session,{}).work_once()['state'])
        self.assertEqual(61085,json.loads(session.publications[0][3])['native_errno'])

    def test_uncertain_tool_queries_terminal_receipt_without_reexecuting(self):
        failure=self.tool_failure(code=511002,native_errno=61092,unknown_outcome=True,action_commit_state='unknown')
        receipt={'run_id':'1'*32,'call_id':'a'*32,'state':'queued','tool_state':'succeeded',
                 'action_commit_state':'committed','replayed':True}
        session=Session(self.tool_claim(),tool_failure=failure,tool_receipt=receipt)
        self.assertEqual(receipt,RunWorker(session,{}).work_once())
        self.assertEqual(1,session.events.count('EXECUTE AI TOOL CALL ?'))
        self.assertEqual([],session.publications)

    def test_ready_or_mismatched_receipt_never_proves_rollback(self):
        for changes in ({'native_errno':3024},{'call_id':'b'*32},{'unknown_outcome':True}):
            failure=self.tool_failure(**changes)
            receipt={'run_id':'1'*32,'call_id':'a'*32,'state':'running','tool_state':'ready',
                     'action_commit_state':'unknown','replayed':False}
            session=Session(self.tool_claim(),tool_failure=failure,tool_receipt=receipt)
            with self.assertRaises(type(failure)) as caught: RunWorker(session,{}).work_once()
            self.assertIs(failure,caught.exception)
            self.assertEqual([],session.publications)
            self.assertEqual(1,session.events.count('EXECUTE AI TOOL CALL ?'))


if __name__ == '__main__': unittest.main()
