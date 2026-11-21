"""Shared numeric errors and validated response options; no provider credentials."""
from __future__ import annotations

import json
import math
from pathlib import Path

CATALOG = json.loads((Path(__file__).resolve().parents[2] / 'contracts/sql/ai-catalog-v1.json').read_text(encoding='utf-8'))
ERRORS = {record['name']: record for record in CATALOG['errors']}


class AiError(Exception):
    def __init__(self, name, *, phase='validation', commit_state='not_committed', run_state=None,
                 run_id=None, unknown_outcome=False, details=None):
        self.record = ERRORS[name]
        self.phase, self.commit_state, self.run_state = phase, commit_state, run_state
        self.run_id, self.unknown_outcome = run_id, unknown_outcome
        self.details = details or {}
        super().__init__(f"{self.record['code']}: {self.record['message']}")

    @property
    def code(self):
        return self.record['code']

    def document(self):
        return dict(code=self.code, classic_errno=self.record['classic_errno'], sqlstate=self.record['sqlstate'],
                    message=self.record['message'], phase=self.phase, commit_state=self.commit_state,
                    run_state=self.run_state, run_id=self.run_id, unknown_outcome=self.unknown_outcome,
                    retry={'action': self.record['retry_action']}, details=self.details)


def canonical(value):
    try:
        return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False).encode('utf-8')
    except (UnicodeError, ValueError, TypeError):
        raise AiError('invalid_unicode') from None
    except RecursionError:
        raise AiError('context_budget') from None


def validate_options(layers=(), *, capabilities=(), response_format='TEXT', limits=None):
    specs = CATALOG['options']
    result = {name: spec['default'] for name, spec in specs.items() if 'default' in spec}
    for layer in layers:
        pairs = list(layer.items()) if isinstance(layer, dict) else list(layer)
        seen = set()
        for name, value in pairs:
            if name not in specs:
                raise AiError('unknown_option', details={'option': str(name)[:64]})
            if name in seen:
                raise AiError('duplicate_option', details={'option': name})
            seen.add(name)
            spec, valid = specs[name], False
            kind = spec['type']
            if kind == 'integer': valid = type(value) is int
            elif kind == 'number': valid = type(value) in (int, float) and math.isfinite(value)
            elif kind == 'boolean': valid = type(value) is bool
            elif kind == 'string': valid = isinstance(value, str)
            elif kind == 'string_array': valid = isinstance(value, list) and all(isinstance(item, str) for item in value)
            if not valid:
                raise AiError('option_type', details={'option': name})
            if 'min' in spec and not spec['min'] <= value <= spec['max']:
                raise AiError('option_range', details={'option': name})
            if 'enum' in spec and value not in spec['enum']:
                raise AiError('option_range', details={'option': name})
            if kind == 'string' and (not value or len(value) > spec.get('max_length', 256)):
                raise AiError('option_range', details={'option': name})
            if kind == 'string_array' and (len(value) > spec['max_items'] or
                    any(not item or len(item) > spec['max_length'] for item in value)):
                raise AiError('option_range', details={'option': name})
            if 'requires' in spec and spec['requires'] not in capabilities:
                raise AiError('unsupported_option', details={'option': name})
            canonical(value)
            result[name] = value
    if response_format not in ('TEXT', 'JSON'):
        raise AiError('option_range', details={'option': 'format'})
    if result['stream']:
        # Buffered publication is the only currently implemented capture path.
        raise AiError('option_conflict' if response_format == 'JSON' else 'unsupported_option', details={'option': 'stream'})
    if ('schema' in result) != ('schema_version' in result) or ('schema' in result and response_format != 'JSON'):
        raise AiError('option_conflict', details={'option': 'schema'})
    if result['provider_timeout_ms'] > result['run_timeout_ms']:
        raise AiError('option_conflict', details={'option': 'provider_timeout_ms'})
    for name, maximum in (limits or {}).items():
        if name in result and (type(result[name]) is not int or result[name] > maximum):
            raise AiError('option_range', details={'option': name})
    return result
