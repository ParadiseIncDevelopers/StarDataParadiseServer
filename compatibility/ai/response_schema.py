"""Bounded JSON Schema 2020-12 structural subset, with all $ref disabled.

Unknown keywords are rejected at registration. No remote resolution, regex,
format assertion, dynamic references or silent acceptance of unsupported rules.
"""
from __future__ import annotations

from .contracts import AiError, canonical
from .providers import parse_json

TYPES = {'null', 'boolean', 'object', 'array', 'number', 'integer', 'string'}
KEYS = {'$schema', 'title', 'description', 'type', 'properties', 'required', 'additionalProperties',
        'items', 'minItems', 'maxItems', 'minLength', 'maxLength', 'minimum', 'maximum', 'enum', 'const'}
DIALECT = 'https://json-schema.org/draft/2020-12/schema'


def check_schema(schema, *, maximum_bytes=65536):
    if len(canonical(schema)) > maximum_bytes:
        raise AiError('option_range', details={'option': 'response_schema'})
    remaining = [1024]

    def visit(node, depth):
        remaining[0] -= 1
        if depth > 16 or remaining[0] < 0 or not isinstance(node, (dict, bool)):
            raise AiError('option_range', details={'option': 'response_schema'})
        if type(node) is bool: return
        if set(node) - KEYS:
            raise AiError('unsupported_option', details={'option': 'response_schema_keyword'})
        if '$schema' in node and node['$schema'] != DIALECT:
            raise AiError('unsupported_option', details={'option': 'response_schema_dialect'})
        for name in ('title', 'description'):
            if name in node and not isinstance(node[name], str): raise AiError('option_type')
        if 'type' in node:
            kinds = node['type'] if isinstance(node['type'], list) else [node['type']]
            if not kinds or any(not isinstance(kind, str) or kind not in TYPES for kind in kinds) or len(kinds) != len(set(kinds)):
                raise AiError('option_type', details={'option': 'response_schema_type'})
        if 'properties' in node:
            if not isinstance(node['properties'], dict) or len(node['properties']) > 256: raise AiError('option_range')
            for child in node['properties'].values(): visit(child, depth + 1)
        if 'required' in node:
            required = node['required']
            if not isinstance(required, list) or any(not isinstance(name, str) for name in required) or len(required) != len(set(required)):
                raise AiError('option_type', details={'option': 'required'})
        if 'additionalProperties' in node: visit(node['additionalProperties'], depth + 1)
        if 'items' in node: visit(node['items'], depth + 1)
        for low, high in (('minItems', 'maxItems'), ('minLength', 'maxLength')):
            for key in (low, high):
                if key in node and (type(node[key]) is not int or not 0 <= node[key] <= 1_000_000):
                    raise AiError('option_range')
            if low in node and high in node and node[low] > node[high]: raise AiError('option_range')
        for key in ('minimum', 'maximum'):
            if key in node and type(node[key]) not in (int, float): raise AiError('option_type')
        if 'minimum' in node and 'maximum' in node and node['minimum'] > node['maximum']: raise AiError('option_range')
        if 'enum' in node and (not isinstance(node['enum'], list) or not 1 <= len(node['enum']) <= 256): raise AiError('option_range')
    visit(schema, 0)
    return schema


def _equal(left, right):
    if isinstance(left, bool) or isinstance(right, bool): return type(left) is type(right) and left == right
    if isinstance(left, dict) and isinstance(right, dict):
        return left.keys() == right.keys() and all(_equal(value, right[key]) for key, value in left.items())
    if isinstance(left, list) and isinstance(right, list):
        return len(left) == len(right) and all(_equal(a, b) for a, b in zip(left, right))
    return left == right


def validate_value(value, schema):
    check_schema(schema)
    remaining = [100000]

    def fail(path): raise AiError('schema_mismatch', phase='response', details={'path': path})

    def matches(value, kind):
        return {'null': value is None, 'boolean': type(value) is bool, 'object': isinstance(value, dict),
                'array': isinstance(value, list), 'number': type(value) in (int, float),
                'integer': type(value) is int or type(value) is float and value.is_integer(),
                'string': isinstance(value, str)}[kind]

    def visit(value, node, path, depth):
        remaining[0] -= 1
        if depth > 64 or remaining[0] < 0: raise AiError('context_budget', phase='response')
        if node is False: fail(path)
        if node is True: return
        if 'type' in node:
            kinds = node['type'] if isinstance(node['type'], list) else [node['type']]
            if not any(matches(value, kind) for kind in kinds): fail(path)
        if 'enum' in node and not any(_equal(value, allowed) for allowed in node['enum']): fail(path)
        if 'const' in node and not _equal(value, node['const']): fail(path)
        if isinstance(value, dict):
            for key in node.get('required', []):
                if key not in value: fail(path + '/' + key.replace('~', '~0').replace('/', '~1'))
            properties = node.get('properties', {})
            for key, child in value.items():
                child_schema = properties.get(key, node.get('additionalProperties', True))
                visit(child, child_schema, path + '/' + key.replace('~', '~0').replace('/', '~1'), depth + 1)
        elif isinstance(value, list):
            if len(value) < node.get('minItems', 0) or len(value) > node.get('maxItems', 1_000_000): fail(path)
            for index, child in enumerate(value): visit(child, node.get('items', True), path + '/' + str(index), depth + 1)
        elif isinstance(value, str):
            if len(value) < node.get('minLength', 0) or len(value) > node.get('maxLength', 1_000_000): fail(path)
        elif type(value) in (int, float):
            if 'minimum' in node and value < node['minimum'] or 'maximum' in node and value > node['maximum']: fail(path)
    canonical(value)
    visit(value, schema, '', 0)
    return value


def decode_response(text, schema=None, *, maximum_bytes=1048576):
    if not isinstance(text, str): raise AiError('capture_type', phase='response')
    canonical(text)
    if len(text.encode('utf-8')) > maximum_bytes: raise AiError('truncated_response', phase='response')
    value = parse_json(text)
    return validate_value(value, schema) if schema is not None else value
