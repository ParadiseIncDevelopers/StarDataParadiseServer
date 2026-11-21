"""AI handlers for the shared durable JOB queue; provider I/O holds no SQL locks.

This worker consumes the native catalog's claim/publication API. It does not
parse application SQL, invent queue successes, or execute model-authored SQL.
"""
from __future__ import annotations

import argparse
from dataclasses import replace
import json
from pathlib import Path
import re
import time
import uuid

from .contracts import AiError, canonical, validate_options
from .local_model import LocalModelStore, LlamaRuntime
from .providers import Model, ProviderClient
from .response_schema import check_schema, decode_response, validate_value
from .secrets import SecretStore


def _document(result):
    row = result.fetch_one()
    if row is None: return None
    value = row[0]
    return json.loads(value) if isinstance(value, str) else value


def _tool_rollback(error, run_id, call_id):
    """Accept only the native executor's definite, pre-COMMIT rollback ABI."""
    native_errno=getattr(error,'native_errno',None)
    code=getattr(error,'code',None)
    if native_errno is None and code==61085: native_errno=code
    if native_errno!=61085: return None
    names=('phase','commit_state','run_state','run_id','call_id','action_commit_state',
           'unknown_outcome','cause_native_errno','cause_sqlstate')
    details={name:getattr(error,name,None) for name in names}
    # mysqlsh exposes the Classic errno and message rather than SDK properties.
    for message in (getattr(error,'msg',None),*getattr(error,'args',())):
        if not isinstance(message,str) or len(message)>8192 or not message.startswith('510005:'): continue
        try: parsed=json.loads(message.partition(':')[2])
        except (ValueError,RecursionError): continue
        if isinstance(parsed,dict):
            details={name:parsed.get(name) for name in names}
            break
    if code not in (510005,61085) or details.get('phase')!='tool' or \
       details.get('commit_state')!='committed' or details.get('run_id')!=run_id or \
       details.get('call_id')!=call_id or details.get('action_commit_state')!='rolled_back' or \
       details.get('unknown_outcome') is not False or getattr(error,'unknown_outcome',False) is True:
        return None
    result={'code':510005,'native_errno':61085,'sqlstate':'45000',
            'message':'Tool transaction rolled back.','phase':'tool','commit_state':'committed',
            'run_state':'running','run_id':run_id,'call_id':call_id,'action_commit_state':'rolled_back',
            'unknown_outcome':False,'retry':{'action':'NONE'}}
    cause=details.get('cause_native_errno')
    if type(cause) is int and 1<=cause<=65535: result['cause_native_errno']=cause
    state=details.get('cause_sqlstate')
    if isinstance(state,str) and re.fullmatch('[A-Z0-9]{5}',state): result['cause_sqlstate']=state
    return result


class RunWorker:
    def __init__(self, session, configuration, *, client=None, clock=time.monotonic, sleep=time.sleep,
                 runtime_factory=LlamaRuntime):
        self.session, self.configuration = session, configuration
        self.worker_id = uuid.uuid4().hex
        self.client = client or ProviderClient(SecretStore(configuration.get('credentials', {})))
        self.clock, self.sleep = clock, sleep
        self.runtime_factory = runtime_factory
        self._imports_first = True

    def _ready(self):
        # A worker must not accidentally run provider calls inside a caller transaction.
        row = self.session.run_sql('SELECT @@autocommit, STARDATAPARADISE_TRANSACTION_ACTIVE(), @sdp_ai_session IS NULL').fetch_one()
        if row is None or row[0] != 1 or row[1] != 0 or row[2] != 1:
            raise AiError('pending_changes', phase='worker')
        # Metadata leases use a single clock context. Native tool execution
        # separately restores the initiating user's sealed SQL time zone.
        self.session.run_sql("SET SESSION time_zone='+00:00'")
        # Native per-RPC DD guards pin the fixed product routine manifest only
        # while a catalog call runs. No catalog MDL spans provider I/O.
        self.session.run_sql('SET SESSION sdp_ai_worker_rpc_guard=ON')

    def _renew(self, run_id):
        status = _document(self.session.run_sql('CALL sdp_ai.renew_lease(?, ?, ?)', [run_id, self.worker_id, 3720]))
        if status is None: raise AiError('revision_conflict', phase='worker')
        if status.get('cancel_requested'): raise AiError('run_cancelled', phase='worker')

    def work_once(self):
        # Alternate kinds so a stream of imports cannot starve inference, or vice versa.
        order = (self._import_once, self._run_once) if self._imports_first else (self._run_once, self._import_once)
        self._imports_first = not self._imports_first
        for handler in order:
            result = handler()
            if result is not None: return result
        return None

    def _import_once(self):
        self._ready()
        claim = _document(self.session.run_sql('CALL sdp_ai.claim_model_import(?, ?)', [self.worker_id, 3720]))
        if claim is None: return None
        artifact, error = None, None
        try:
            local = self.configuration.get('local_runtime', {})
            import_root = local.get('import_roots', {}).get(claim['owner'])
            if not import_root or not local.get('model_store') or not local.get('executable'):
                raise AiError('model_unavailable', phase='import')
            root, source = LocalModelStore.source_path(import_root, claim['relative_source'])
            store = LocalModelStore(local['model_store'], maximum_bytes=local.get('maximum_import_bytes', 64 * 1024**3))
            started, renewed = self.clock(), self.clock() - 5
            def progress(_count):
                nonlocal renewed
                now = self.clock()
                if now - started >= 3600: raise AiError('provider_timeout', phase='import')
                if now - renewed >= 5:
                    self._ready()
                    status = _document(self.session.run_sql('CALL sdp_ai.renew_model_import(?, ?, ?)',
                                                           [claim['job_id'], self.worker_id, 3720]))
                    if status is None: raise AiError('revision_conflict', phase='import')
                    if status.get('cancel_requested'): raise AiError('run_cancelled', phase='import')
                    renewed = now
            progress(0)
            artifact = store.import_file(source, expected_sha256=claim['sha256'], authorized_root=root, progress=progress)
            # A GGUF header/hash establishes artifact integrity, not an executable
            # LLM. Publication additionally requires the owned runtime to load it.
            progress(artifact['bytes'])
            runtime = self.runtime_factory(local['executable'], store,threads=local.get('threads',4),
                      memory_limit_bytes=local.get('memory_limit_bytes',4*1024**3))
            with runtime.start(artifact['artifact_id'], context_tokens=claim.get('context_tokens',4096),
                               startup_timeout=min(120,max(0,3600-(self.clock()-started)))) as process:
                progress(artifact['bytes'])
                artifact['tool_template_verified']=process.get('tool_template_verified',False) is True
                if artifact['tool_template_verified']:artifact['tool_template_sha256']=process['tool_template_sha256']
                if 'tools' in claim.get('capabilities',[]) and not artifact['tool_template_verified']:
                    raise AiError('model_capability',phase='import')
            artifact['runtime_verified'] = True
        except AiError as failure:
            artifact = None
            failure.commit_state, failure.run_state = 'committed', 'failed'
            error = failure.document()
        except (KeyError, TypeError, ValueError):
            artifact = None
            error = AiError('invalid_dependency', phase='import', commit_state='committed', run_state='failed').document()
        self._ready()
        return _document(self.session.run_sql('CALL sdp_ai.publish_model_import(?, ?, ?, ?)',
                         [claim['job_id'], self.worker_id, canonical(artifact).decode(), canonical(error).decode()]))

    def _run_once(self):
        self._ready()
        claim = _document(self.session.run_sql('CALL sdp_ai.claim_run(?, ?)', [self.worker_id, 3720]))
        if claim is None: return None
        run_id = claim['run_id']
        if claim.get('phase') == 'tools': return self._execute_tools(claim)
        result, metadata, error = None, {}, None
        started = self.clock()
        try:
            self._ready()
            limits = dict(self.configuration.get('limits', {}))
            for name, maximum in self.configuration.get('owner_limits', {}).get(claim['owner'], {}).items():
                limits[name] = min(limits.get(name, maximum), maximum)
            options = validate_options([claim.get('options', {})], capabilities=claim['model'].get('capabilities', []),
                                       response_format=claim.get('response_format', 'TEXT'), limits=limits)
            if claim.get('response_schema') is not None: check_schema(claim['response_schema'])
            deadline = started + options['run_timeout_ms'] / 1000
            definition = claim['model']
            model = Model(definition['provider'], definition['remote_model'], claim['owner'],
                          definition.get('credential_ref') or definition.get('secret_env', ''),
                          definition.get('endpoint', ''), frozenset(definition.get('capabilities', [])),
                          definition.get('api_version', '2026-10-21'))
            tools = claim.get('tools', [])
            if tools and claim.get('functions', False) and 'tools' not in model.capabilities:
                raise AiError('model_capability', phase='worker')
            if tools and not claim.get('functions', False): raise AiError('tool_denied', phase='worker')
            if len(tools) > 64 or type(claim.get('tool_round', 0)) is not int or \
               claim.get('tool_round', 0) >= min(16, self.configuration.get('maximum_tool_rounds', 8)):
                raise AiError('context_budget', phase='worker')
            if model.provider == 'local_gguf':
                local = self.configuration.get('local_runtime', {})
                if not local.get('executable') or not local.get('model_store'):
                    raise AiError('model_unavailable', phase='runtime')
                runtime = self.runtime_factory(local['executable'], LocalModelStore(local['model_store']),
                          threads=local.get('threads',4),memory_limit_bytes=local.get('memory_limit_bytes',4*1024**3))
                with runtime.start(definition['artifact_id'], context_tokens=definition.get('context_tokens', 4096),
                                   startup_timeout=min(120, max(0, deadline - self.clock()))) as process:
                    if tools and (process.get('tool_template_verified') is not True or
                                  process.get('tool_template_sha256')!=definition.get('artifact',{}).get('tool_template_sha256')):
                        raise AiError('model_capability',phase='runtime')
                    model = replace(model, endpoint=process['endpoint'])
                    client = ProviderClient(self.client.secrets, local_tokens={model.endpoint: process['token']})
                    response = self._generate(client, model, claim, options, deadline)
            else:
                response = self._generate(self.client, model, claim, options, deadline)
            metadata = {'provider': response.provider, 'model': response.model, 'request_id': response.request_id,
                        'usage': response.usage, 'elapsed_ms': int((self.clock() - started) * 1000)}
            if response.tool_calls:
                allowed = {tool['provider_name']: tool for tool in tools}
                if len(allowed) != len(tools): raise AiError('tool_denied', phase='response')
                calls = []
                for call in response.tool_calls:
                    tool = allowed.get(call['name'])
                    if tool is None: raise AiError('tool_denied', phase='response')
                    try: validate_value(call['arguments'], tool['parameters'])
                    except AiError: raise AiError('tool_arguments', phase='response') from None
                    calls.append({'provider_call_id': call['id'], 'provider_name': call['name'], 'arguments': call['arguments']})
                if response.refusal: raise AiError('provider_protocol', phase='response')
                metadata['provider_continuation'] = list(response.continuation)
                self._ready()
                self._renew(run_id)
                # One atomic batch avoids losing siblings after a worker crash.
                # The engine validates the pinned allowlist and returns durable
                # waiting states. No model-authored SQL is executed by Python.
                return _document(self.session.run_sql('CALL sdp_ai.publish_tool_requests(?, ?, ?, ?, ?, ?)',
                    [run_id, self.worker_id, uuid.uuid4().hex, response.text, canonical(calls).decode(), canonical(metadata).decode()]))
            if response.refusal:
                result = {'format': 'REFUSAL', 'value': None}
            elif claim.get('response_format', 'TEXT') == 'JSON':
                value = decode_response(response.text, claim.get('response_schema'), maximum_bytes=options['max_output_bytes'])
                result = {'format': 'JSON', 'value': value}
            else:
                result = {'format': 'TEXT', 'value': response.text}
        except AiError as failure:
            failure.commit_state, failure.run_state, failure.run_id = 'committed', \
                ('cancelled' if failure.code == 511004 else 'failed'), run_id
            error = failure.document()
        except (KeyError, TypeError, ValueError):
            error = AiError('invalid_dependency', phase='worker', commit_state='committed',
                            run_state='failed', run_id=run_id).document()
        self._ready()
        # Publication checks current epoch, revision, cancellation and worker lease
        # inside the same transaction as the response, queue status and outbox event.
        published = _document(self.session.run_sql('CALL sdp_ai.publish_run(?, ?, ?, ?, ?)',
                              [run_id, self.worker_id, canonical(result).decode(), canonical(metadata).decode(),
                               canonical(error).decode()]))
        return published

    def _generate(self, client, model, claim, options, deadline):
        for attempt in range(options['max_attempts']):
            remaining = int((deadline - self.clock()) * 1000)
            if remaining <= 0: raise AiError('provider_timeout', phase='worker')
            current = dict(options, provider_timeout_ms=min(options['provider_timeout_ms'], remaining))
            self._ready()
            self._renew(claim['run_id'])
            try:
                return client.generate(model, claim['messages'], instructions=claim.get('instructions', ''),
                                       options=current, response_format=claim.get('response_format', 'TEXT'), tools=claim.get('tools', []))
            except AiError as error:
                # Only a definitive rate-limit rejection can be retried here.
                # Timeout/transport ambiguity never causes a second provider call.
                if error.code != 509003 or error.unknown_outcome or attempt + 1 == options['max_attempts']: raise
                delay = options['retry_backoff_ms'] / 1000
                if self.clock() + delay >= deadline: raise AiError('provider_timeout', phase='worker')
                self.sleep(delay)

    def _execute_tools(self, claim):
        calls = claim.get('tool_calls_to_execute')
        if not isinstance(calls, list) or not 1 <= len(calls) <= 32:
            raise AiError('tool_arguments', phase='worker', commit_state='committed')
        ordinals = [call.get('ordinal') if isinstance(call, dict) else None for call in calls]
        if any(type(value) is not int or value < 0 for value in ordinals) or len(set(ordinals)) != len(ordinals):
            raise AiError('tool_arguments', phase='worker', commit_state='committed')
        published = None
        for call in sorted(calls, key=lambda value: value['ordinal']):
            call_id = call.get('call_id') if isinstance(call, dict) else None
            if not isinstance(call_id, str) or len(call_id) != 32 or any(c not in '0123456789abcdef' for c in call_id):
                raise AiError('tool_arguments', phase='worker', commit_state='committed')
            if call.get('state') == 'waiting_approval':
                return {'run_id': claim['run_id'], 'state': 'waiting_approval'}
            self._ready()
            self._renew(claim['run_id'])
            # Arguments, owner, pinned definition, Decision and approval are
            # resolved from the protected database record, not supplied here.
            published = self._execute_tool(claim['run_id'],call_id)
            if published is None: raise AiError('provider_protocol', phase='worker', commit_state='committed')
            if published.get('state') == 'waiting_approval': return published
        return published

    def _execute_tool(self, run_id, call_id):
        try:
            return _document(self.session.run_sql('EXECUTE AI TOOL CALL ?', [call_id]))
        except Exception as failure:
            proof=_tool_rollback(failure,run_id,call_id)
            if proof is not None:
                self._ready()
                return _document(self.session.run_sql('CALL sdp_ai.publish_tool_failure(?, ?, ?, ?)',
                    [run_id,self.worker_id,call_id,canonical(proof).decode()]))
            # An interrupted result can follow a committed tool transaction.
            # Consult an immutable receipt when this connection is usable;
            # ready/running is not rollback proof. Never repeat the invocation.
            try:
                self._ready()
                receipt=_document(self.session.run_sql('CALL sdp_ai.tool_call_status(?)',[call_id]))
            except Exception:
                receipt=None
            if isinstance(receipt,dict) and receipt.get('run_id')==run_id and receipt.get('call_id')==call_id and \
               receipt.get('replayed') is True and receipt.get('tool_state') in ('succeeded','failed','denied') and \
               receipt.get('action_commit_state') in ('committed','rolled_back','not_started'):
                return receipt
            raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True, help='Trusted worker configuration; contains aliases, never SQL text.')
    parser.add_argument('--once', action='store_true')
    args = parser.parse_args(argv)
    from mysqlsh import globals as shell_globals
    session = shell_globals.shell.get_session()
    if session is None or not session.is_open(): raise RuntimeError('Connect the worker account first.')
    tls = session.run_sql("SHOW SESSION STATUS LIKE 'Ssl_cipher'").fetch_one()
    if tls is None or not tls[1]: raise RuntimeError('The worker requires a TLS connection.')
    configuration = json.loads(args.config.read_text(encoding='utf-8-sig'))
    worker = RunWorker(session, configuration)
    while True:
        result = worker.work_once()
        if result is not None:
            # Native publication returns status identifiers, without transcript/key values.
            print(canonical({key: result[key] for key in ('run_id', 'job_id', 'model_id', 'state') if key in result}).decode(), flush=True)
        if args.once: break
        if result is None: time.sleep(0.5)


if __name__ == '__main__': main()
