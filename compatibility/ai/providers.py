"""Real buffered OpenAI, Anthropic and local-model HTTP adapters.

Credentials are resolved by the worker, outside SQL and transcript storage.
The adapter never executes SQL or provider-requested tools and never retries
an ambiguous external call on its own.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import json
import os
import re
import socket
import ssl
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, build_opener, HTTPSHandler, HTTPRedirectHandler, ProxyHandler

from .contracts import AiError, CATALOG, canonical, validate_options
from .secrets import authorize_cloud_endpoint


class EnvironmentSecrets:
    """Trusted worker configuration binds an actor's alias to one environment key."""
    def __init__(self, bindings, environment=None):
        self.bindings = bindings
        self.environment = os.environ if environment is None else environment

    def authorize_endpoint(self, owner, reference, provider, endpoint):
        authorize_cloud_endpoint({}, provider, endpoint)

    def resolve(self, owner, reference):
        name = self.bindings.get(owner, {}).get(reference)
        value = self.environment.get(name, '') if name else ''
        if not value or '\r' in value or '\n' in value:
            raise AiError('provider_auth', phase='credential')
        return value


class _NoRedirects(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        # A redirect must never forward a credential to a different host.
        return None


@dataclass(frozen=True)
class Model:
    provider: str
    remote_model: str
    owner: str
    credential_ref: str = ''
    endpoint: str = ''
    capabilities: frozenset = field(default_factory=frozenset)
    api_version: str = '2026-10-21'


@dataclass(frozen=True)
class ProviderResult:
    text: str
    provider: str
    model: str
    request_id: str | None
    usage: dict
    tool_calls: tuple = ()
    refusal: bool = False
    continuation: tuple = ()


def _endpoint(model):
    if model.provider not in ('openai', 'anthropic', 'local_gguf'):
        raise AiError('model_unavailable')
    endpoint = model.endpoint or CATALOG['providers'][model.provider]['endpoint']
    parsed = urlsplit(endpoint)
    if parsed.username or parsed.password or parsed.fragment or parsed.query or not parsed.hostname:
        raise AiError('unsafe_endpoint')
    if model.provider == 'local_gguf':
        if parsed.scheme != 'http' or parsed.hostname != '127.0.0.1' or not parsed.port:
            raise AiError('unsafe_endpoint')
    elif parsed.scheme != 'https':
        raise AiError('unsafe_endpoint')
    return endpoint


def _call(call):
    if not isinstance(call, dict) or set(call) != {'id', 'name', 'arguments'} or \
       not all(isinstance(call[key], str) and re.fullmatch(r'[A-Za-z0-9_-]{1,128}', call[key])
               for key in ('id', 'name')) or not isinstance(call['arguments'], dict):
        raise AiError('tool_arguments', phase='response')
    if len(canonical(call)) > 65536: raise AiError('context_budget', phase='response')
    return dict(call)


def _messages(messages):
    """Validate ordered, durable calls/results; tool output is always data."""
    result, pending, seen = [], set(), set()
    if not isinstance(messages, (list, tuple)) or len(messages) > 4096:
        raise AiError('context_budget')
    for message in messages:
        if not isinstance(message, dict): raise AiError('tool_arguments', details={'input': 'message'})
        role = message.get('role')
        if role in ('user', 'assistant'):
            if pending or set(message) - {'role', 'content', 'tool_calls', 'provider_continuation'} or \
               not isinstance(message.get('content'), str): raise AiError('tool_arguments')
            calls = message.get('tool_calls', [])
            if calls and (role != 'assistant' or not isinstance(calls, list) or len(calls) > 32):
                raise AiError('tool_arguments')
            for call in calls:
                _call(call)
                if call['id'] in seen: raise AiError('tool_arguments')
                pending.add(call['id']); seen.add(call['id'])
            continuation = message.get('provider_continuation', [])
            if continuation and (role != 'assistant' or not isinstance(continuation, list)):
                raise AiError('tool_arguments')
            for item in continuation:
                # Only the opaque encrypted reasoning envelope is replayed. No
                # provider-supplied tool declarations or arbitrary input types.
                if not isinstance(item, dict) or item.get('type') != 'reasoning' or \
                   set(item) - {'type', 'id', 'encrypted_content', 'summary'} or \
                   not isinstance(item.get('encrypted_content'), str) or item.get('summary', []) != []:
                    raise AiError('provider_protocol')
            canonical(message)
        elif role == 'tool':
            if set(message) != {'role', 'tool_call_id', 'content'} or message['tool_call_id'] not in pending:
                raise AiError('tool_arguments')
            pending.remove(message['tool_call_id'])
            canonical(message['content'])
        else:
            raise AiError('tool_arguments', details={'input': 'message'})
        result.append(dict(message))
    if not result or pending or result[-1]['role'] not in ('user', 'tool'):
        raise AiError('empty_context')
    if len(canonical(result)) > 16 * 1024**2: raise AiError('context_budget')
    return result


def _wire_messages(provider, messages):
    result = []
    for message in messages:
        role = message['role']
        if provider == 'openai':
            if role == 'tool':
                result.append({'type': 'function_call_output', 'call_id': message['tool_call_id'],
                               'output': canonical(message['content']).decode()})
            else:
                result.extend(message.get('provider_continuation', []))
                if message['content'] or not message.get('tool_calls'):
                    result.append({'role': role, 'content': message['content']})
                for call in message.get('tool_calls', []):
                    result.append({'type': 'function_call', 'call_id': call['id'], 'name': call['name'],
                                   'arguments': canonical(call['arguments']).decode()})
        elif provider == 'anthropic':
            if role == 'tool':
                block = {'type': 'tool_result', 'tool_use_id': message['tool_call_id'],
                         'content': canonical(message['content']).decode()}
                if result and result[-1]['role'] == 'user' and isinstance(result[-1]['content'], list):
                    result[-1]['content'].append(block)
                else: result.append({'role': 'user', 'content': [block]})
            else:
                blocks = ([{'type': 'text', 'text': message['content']}] if message['content'] else [])
                blocks.extend({'type': 'tool_use', 'id': call['id'], 'name': call['name'], 'input': call['arguments']}
                              for call in message.get('tool_calls', []))
                result.append({'role': role, 'content': blocks if message.get('tool_calls') else message['content']})
        else:
            if role == 'tool':
                result.append({'role': 'tool', 'tool_call_id': message['tool_call_id'],
                               'content': canonical(message['content']).decode()})
            else:
                item = {'role': role, 'content': message['content']}
                if message.get('tool_calls'):
                    item['tool_calls'] = [{'id': call['id'], 'type': 'function',
                                          'function': {'name': call['name'], 'arguments': canonical(call['arguments']).decode()}}
                                         for call in message['tool_calls']]
                result.append(item)
    return result


def _tools(provider, tools):
    if not isinstance(tools, (list, tuple)) or len(tools) > 64: raise AiError('tool_denied')
    names, result = set(), []
    from .response_schema import check_schema
    for tool in tools:
        name = tool.get('provider_name') if isinstance(tool, dict) else None
        if not isinstance(name, str) or not re.fullmatch(r'tool_[0-9a-f]{32}', name) or name in names:
            raise AiError('tool_denied')
        names.add(name)
        schema = tool.get('parameters')
        check_schema(schema)
        if not isinstance(schema, dict) or schema.get('type') != 'object': raise AiError('tool_arguments')
        description = tool.get('description', 'Execute the registered database action with the provided typed arguments.')
        if not isinstance(description, str) or len(description) > 4096: raise AiError('tool_arguments')
        if provider == 'anthropic': result.append({'name': name, 'description': description, 'input_schema': schema})
        else:
            function = {'type': 'function', 'name': name, 'description': description, 'parameters': schema, 'strict': False}
            if provider == 'openai': result.append(function)
            else:
                function.pop('type'); function.pop('strict')
                result.append({'type': 'function', 'function': function})
    return result


def _unique_object(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError('duplicate JSON property')
        result[name] = value
    return result


def parse_json(value):
    try:
        canonical(value)  # Also reject unpaired Unicode surrogates.
        return json.loads(value, object_pairs_hook=_unique_object,
                          parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite JSON')))
    except (ValueError, TypeError):
        raise AiError('invalid_json', phase='response') from None
    except RecursionError:
        raise AiError('context_budget', phase='response') from None


class ProviderClient:
    def __init__(self, secrets, *, opener=None, local_tokens=None):
        self.secrets = secrets
        self.opener = opener or build_opener(_NoRedirects(), HTTPSHandler(context=ssl.create_default_context()))
        self.local_opener = opener or build_opener(_NoRedirects(), ProxyHandler({}))
        self.local_tokens = local_tokens or {}

    def generate(self, model, messages, *, instructions='', options=None, response_format='TEXT', tools=()):
        endpoint = _endpoint(model)
        if not isinstance(model.remote_model, str) or not 1 <= len(model.remote_model) <= 256:
            raise AiError('model_unavailable')
        supported = {'openai': {'temperature', 'top_p'}, 'anthropic': {'stop'},
                     'local_gguf': {'temperature', 'top_p', 'seed', 'stop'}}[model.provider]
        for name in ('temperature', 'top_p', 'seed', 'stop'):
            if name in (options or {}) and name not in supported:
                raise AiError('unsupported_option', details={'option': name})
        options = validate_options([options or {}], capabilities=model.capabilities, response_format=response_format)
        messages = _messages(messages)
        wire_messages = _wire_messages(model.provider, messages)
        wire_tools = _tools(model.provider, tools)
        if tools and 'tools' not in model.capabilities: raise AiError('model_capability')
        if not isinstance(instructions, str):
            raise AiError('option_type')
        canonical(instructions)
        headers = {'Content-Type': 'application/json', 'Accept': 'application/json'}
        if model.provider == 'local_gguf':
            token = self.local_tokens.get(endpoint)
            if not token: raise AiError('model_unavailable', phase='runtime')
            headers['Authorization'] = 'Bearer ' + token
        if model.provider != 'local_gguf':
            authorize = getattr(self.secrets, 'authorize_endpoint', None)
            if authorize is not None:
                authorize(model.owner, model.credential_ref, model.provider, endpoint)
            else:
                authorize_cloud_endpoint({}, model.provider, endpoint)
            secret = self.secrets.resolve(model.owner, model.credential_ref)
            headers['Authorization'] = 'Bearer ' + secret
            if model.provider == 'anthropic':
                if model.api_version != '2026-10-21':
                    raise AiError('unsupported_option', details={'option': 'api_version'})
                headers['anthropic-version'] = model.api_version
        if model.provider == 'openai':
            body = {'model': model.remote_model, 'input': wire_messages, 'instructions': instructions,
                    'max_output_tokens': options['max_output_tokens'], 'stream': False, 'store': False}
            # JSON is validated locally even when the provider uses a JSON format.
            if response_format == 'JSON':
                body['text'] = {'format': {'type': 'json_object'}}
        elif model.provider == 'anthropic':
            body = {'model': model.remote_model, 'messages': wire_messages, 'system': instructions,
                    'max_tokens': options['max_output_tokens'], 'stream': False}
        else:
            body = {'model': model.remote_model, 'messages':
                    ([{'role': 'system', 'content': instructions}] if instructions else []) + wire_messages,
                    'max_tokens': options['max_output_tokens'], 'stream': False}
            if response_format == 'JSON': body['response_format'] = {'type': 'json_object'}
        if wire_tools:
            body['tools'] = wire_tools
            body['tool_choice'] = 'auto' if model.provider != 'anthropic' else {'type': 'auto'}
            if model.provider == 'openai': body['include'] = ['reasoning.encrypted_content']
        for name in ('temperature', 'top_p', 'seed', 'stop'):
            if name in options:
                body['stop_sequences' if model.provider == 'anthropic' and name == 'stop' else name] = options[name]
        request = Request(endpoint, data=canonical(body), headers=headers, method='POST')
        deadline = time.monotonic() + options['provider_timeout_ms'] / 1000
        try:
            transport = self.local_opener if model.provider == 'local_gguf' else self.opener
            with transport.open(request, timeout=options['provider_timeout_ms'] / 1000) as response:
                chunks, length = [], 0
                maximum = max(65536, options['max_output_bytes'] * 2)
                while True:
                    if time.monotonic() >= deadline:
                        raise AiError('provider_timeout', phase='provider', unknown_outcome=True)
                    chunk = response.read(min(65536, maximum - length + 1))
                    if not chunk: break
                    length += len(chunk)
                    if length > maximum:
                        raise AiError('truncated_response', phase='provider')
                    chunks.append(chunk)
                try:
                    document = json.loads(b''.join(chunks).decode('utf-8'), object_pairs_hook=_unique_object)
                except (UnicodeError, ValueError, RecursionError):
                    raise AiError('provider_protocol', phase='provider') from None
                request_id = response.headers.get('x-request-id') or response.headers.get('request-id')
                if request_id and not re.fullmatch(r'[A-Za-z0-9_-]{1,256}', request_id): request_id = None
        except HTTPError as error:
            name = 'provider_auth' if error.code in (401, 403) else \
                   'provider_rate_limit' if error.code == 429 else \
                   'unsafe_endpoint' if 300 <= error.code < 400 else 'provider_protocol'
            raise AiError(name, phase='provider', details={'http_status': error.code}) from None
        except (TimeoutError, socket.timeout):
            raise AiError('provider_timeout', phase='provider', unknown_outcome=True) from None
        except (URLError, OSError):
            raise AiError('provider_transport', phase='provider', unknown_outcome=True) from None
        result = self._decode(model, document, request_id)
        if len(result.text.encode('utf-8')) > options['max_output_bytes']:
            raise AiError('truncated_response', phase='response')
        return result

    @staticmethod
    def _decode(model, document, request_id):
        try:
            texts, tools, continuation, refusal, missing_reasoning = [], [], [], False, False
            if model.provider == 'openai':
                if document.get('status') == 'incomplete': raise AiError('truncated_response', phase='response')
                if document.get('status') != 'completed': raise AiError('provider_protocol', phase='provider')
                for item in document['output']:
                    if item['type'] == 'function_call':
                        tools.append({'id': item['call_id'], 'name': item['name'], 'arguments': parse_json(item['arguments'])})
                    elif item['type'] == 'message':
                        for block in item['content']:
                            if block['type'] == 'output_text': texts.append(block['text'])
                            elif block['type'] == 'refusal': refusal = True
                    elif item['type'] == 'reasoning':
                        # No plain chain of thought is stored or exposed. The
                        # provider's encrypted continuation can be replayed.
                        if not isinstance(item.get('encrypted_content'), str): missing_reasoning = True
                        else:
                            continuation.append({key: item[key] for key in ('type', 'id', 'encrypted_content') if key in item})
                            continuation[-1]['summary'] = []
                    else: raise AiError('provider_protocol', phase='response')
            elif model.provider == 'anthropic':
                if document['stop_reason'] == 'max_tokens': raise AiError('truncated_response', phase='response')
                if document['stop_reason'] not in ('end_turn', 'stop_sequence', 'tool_use', 'refusal'):
                    raise AiError('provider_protocol', phase='provider')
                refusal = document['stop_reason'] == 'refusal'
                for block in document['content']:
                    if block['type'] == 'text': texts.append(block['text'])
                    elif block['type'] == 'tool_use':
                        tools.append({'id': block['id'], 'name': block['name'], 'arguments': block['input']})
            else:
                choice = document['choices'][0]
                if choice['finish_reason'] == 'length': raise AiError('truncated_response', phase='response')
                if choice['finish_reason'] not in ('stop', 'tool_calls'): raise AiError('provider_protocol', phase='provider')
                if choice['message'].get('content') is not None: texts.append(choice['message']['content'])
                for call in choice['message'].get('tool_calls', []):
                    if call['type'] != 'function': raise AiError('provider_protocol', phase='response')
                    tools.append({'id': call['id'], 'name': call['function']['name'],
                                  'arguments': parse_json(call['function']['arguments'])})
                if choice['finish_reason'] == 'tool_calls' and not tools: raise AiError('provider_protocol', phase='response')
            if len(tools) > 32 or len({call['id'] for call in tools}) != len(tools):
                raise AiError('tool_arguments', phase='response')
            tools = [_call(call) for call in tools]
            if tools and missing_reasoning: raise AiError('model_capability', phase='response')
            if len(canonical(continuation)) > 1024**2: raise AiError('context_budget', phase='response')
            if not all(isinstance(value, str) for value in texts): raise ValueError()
            text = ''.join(texts)
            canonical(text)
            if not text and not tools and not refusal: raise AiError('empty_response', phase='response')
            usage = document.get('usage', {})
            if not isinstance(usage, dict): raise ValueError()
            # Store only numeric usage counters, never arbitrary provider content.
            usage = {key: value for key, value in usage.items() if
                     key in ('input_tokens', 'output_tokens', 'total_tokens', 'prompt_tokens', 'completion_tokens')
                     and type(value) is int and value >= 0}
            return ProviderResult(text, model.provider, model.remote_model, request_id, usage, tuple(tools), refusal, tuple(continuation))
        except (KeyError, IndexError, TypeError, ValueError):
            raise AiError('provider_protocol', phase='provider') from None
