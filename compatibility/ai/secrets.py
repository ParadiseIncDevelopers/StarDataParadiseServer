"""Worker-only credential aliases bound to authenticated database owners."""
from __future__ import annotations

import ctypes
from ctypes import wintypes
import os
import re

from .contracts import AiError, CATALOG


def authorize_cloud_endpoint(binding, provider, endpoint):
    """Only trusted worker configuration may redirect a stored cloud key."""
    if provider not in ('openai', 'anthropic') or not isinstance(binding, dict):
        raise AiError('provider_auth', phase='credential')
    providers = binding.get('providers')
    if providers is not None and (not isinstance(providers, list) or not providers or
            any(value not in ('openai', 'anthropic') for value in providers) or provider not in providers):
        raise AiError('provider_auth', phase='credential')
    endpoints = binding.get('endpoints')
    if endpoints is None:
        allowed = endpoint == CATALOG['providers'][provider]['endpoint']
    else:
        allowed = (isinstance(endpoints, list) and 1 <= len(endpoints) <= 8 and
                   all(isinstance(value, str) and 1 <= len(value) <= 2048 for value in endpoints) and endpoint in endpoints)
    if not allowed: raise AiError('unsafe_endpoint', phase='credential')


class SecretStore:
    def __init__(self, bindings, *, environment=None):
        self.bindings = bindings
        self.environment = os.environ if environment is None else environment

    def authorize_endpoint(self, owner, reference, provider, endpoint):
        authorize_cloud_endpoint(self.bindings.get(owner, {}).get(reference), provider, endpoint)

    def resolve(self, owner, reference):
        binding = self.bindings.get(owner, {}).get(reference)
        if not isinstance(binding, dict): raise AiError('provider_auth', phase='credential')
        source = binding.get('source')
        if source == 'environment':
            name = binding.get('name', '')
            if not re.fullmatch('[A-Za-z_][A-Za-z0-9_]{0,127}', name): raise AiError('provider_auth', phase='credential')
            value = self.environment.get(name, '')
        elif source == 'windows':
            target = binding.get('target', '')
            if not target.startswith('StarDataParadise/ai/') or len(target) > 256:
                raise AiError('provider_auth', phase='credential')
            value = self._windows(target)
        else:
            raise AiError('provider_auth', phase='credential')
        if not isinstance(value, str) or not value or len(value) > 16384 or '\r' in value or '\n' in value:
            raise AiError('provider_auth', phase='credential')
        return value

    @staticmethod
    def _windows(target):
        if os.name != 'nt': raise AiError('provider_auth', phase='credential')
        class Credential(ctypes.Structure):
            _fields_ = [('Flags', wintypes.DWORD), ('Type', wintypes.DWORD), ('TargetName', wintypes.LPWSTR),
                        ('Comment', wintypes.LPWSTR), ('LastWritten', wintypes.FILETIME),
                        ('CredentialBlobSize', wintypes.DWORD), ('CredentialBlob', ctypes.POINTER(ctypes.c_ubyte)),
                        ('Persist', wintypes.DWORD), ('AttributeCount', wintypes.DWORD),
                        ('Attributes', ctypes.c_void_p), ('TargetAlias', wintypes.LPWSTR), ('UserName', wintypes.LPWSTR)]
        library = ctypes.WinDLL('Advapi32.dll', use_last_error=True)
        pointer = ctypes.POINTER(Credential)()
        read = library.CredReadW
        read.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(ctypes.POINTER(Credential)))
        read.restype = wintypes.BOOL
        free = library.CredFree
        free.argtypes, free.restype = (ctypes.c_void_p,), None
        if not read(target, 1, 0, ctypes.byref(pointer)):
            raise AiError('provider_auth', phase='credential')
        try:
            value = pointer.contents
            if not 1 <= value.CredentialBlobSize <= 16384: raise AiError('provider_auth', phase='credential')
            try: return ctypes.string_at(value.CredentialBlob, value.CredentialBlobSize).decode('utf-8')
            except UnicodeError: raise AiError('provider_auth', phase='credential') from None
        finally:
            free(pointer)
