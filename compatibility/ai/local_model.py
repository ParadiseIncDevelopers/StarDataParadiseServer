"""Content-addressed GGUF imports and an owned, loopback-only llama.cpp process."""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import socket
import struct
import subprocess
import time
import uuid
from urllib.error import URLError
from urllib.request import Request, build_opener, ProxyHandler

from .contracts import AiError, canonical
from .owned_process import OwnedProcessJob,runtime_environment


class LocalModelStore:
    def __init__(self, root, *, maximum_bytes=64 * 1024**3):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.maximum_bytes = maximum_bytes

    @staticmethod
    def header(path):
        with path.open('rb') as stream:
            data = stream.read(24)
        if len(data) != 24:
            raise AiError('model_integrity', phase='import')
        magic, version, tensors, metadata = struct.unpack('<4sIQQ', data)
        if magic != b'GGUF' or version not in (2, 3) or tensors > 1_000_000 or metadata > 1_000_000:
            raise AiError('model_integrity', phase='import')
        return {'gguf_version': version, 'tensor_count': tensors, 'metadata_count': metadata}

    @staticmethod
    def digest(path):
        checksum = hashlib.sha256()
        with path.open('rb') as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                checksum.update(chunk)
        return checksum.hexdigest()

    @staticmethod
    def source_path(root, relative):
        """Resolve a user's upload path within the administrator's owner binding."""
        if not isinstance(relative, str) or not relative or ':' in relative or '\x00' in relative:
            raise AiError('model_integrity', phase='import')
        parts = relative.replace('\\', '/').split('/')
        if any(part in ('', '.', '..') for part in parts):
            raise AiError('model_integrity', phase='import')
        try:
            base = Path(root).resolve(strict=True)
            candidate = base.joinpath(*parts)
            # Reject links/junctions even when their current target is inside the root.
            for entry in (candidate, *candidate.parents):
                if entry == base: break
                if entry.is_symlink() or (hasattr(entry, 'is_junction') and entry.is_junction()):
                    raise AiError('model_integrity', phase='import')
            resolved = candidate.resolve(strict=True)
            resolved.relative_to(base)
            if not base.is_dir() or not resolved.is_file(): raise ValueError()
            return base, resolved
        except (OSError, ValueError):
            raise AiError('model_unavailable', phase='import') from None

    @staticmethod
    def _check_handle(stream, authorized_root):
        # Check the opened file, so swapping an upload directory between resolve
        # and open cannot import a file outside the owner's authorized directory.
        try:
            if os.name == 'nt':
                import ctypes
                from ctypes import wintypes
                import msvcrt
                api = ctypes.WinDLL('kernel32', use_last_error=True).GetFinalPathNameByHandleW
                api.argtypes = [wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD]
                api.restype = wintypes.DWORD
                buffer = ctypes.create_unicode_buffer(32768)
                length = api(msvcrt.get_osfhandle(stream.fileno()), buffer, len(buffer), 0)
                if not 0 < length < len(buffer): raise ValueError()
                final = buffer.value
                if final.startswith('\\\\?\\UNC\\'): final = '\\\\' + final[8:]
                elif final.startswith('\\\\?\\'): final = final[4:]
            else:
                final = os.readlink('/proc/self/fd/' + str(stream.fileno()))
            Path(final).resolve(strict=True).relative_to(authorized_root)
        except (OSError, ValueError):
            raise AiError('model_integrity', phase='import') from None

    def import_file(self, source, *, expected_sha256=None, authorized_root=None, progress=None):
        source = Path(source)
        if source.is_symlink() or source.suffix.lower() != '.gguf' or not source.is_file():
            raise AiError('model_integrity', phase='import')
        if expected_sha256 is not None and not re.fullmatch('[0-9a-f]{64}', expected_sha256):
            raise AiError('model_integrity', phase='import')
        temporary = self.root / ('.import-' + uuid.uuid4().hex + '.part')
        checksum, count = hashlib.sha256(), 0
        try:
            with source.open('rb') as incoming, temporary.open('xb') as outgoing:
                if authorized_root is not None: self._check_handle(incoming, authorized_root)
                original = os.fstat(incoming.fileno())
                if not 24 <= original.st_size <= self.maximum_bytes:
                    raise AiError('model_integrity', phase='import')
                for chunk in iter(lambda: incoming.read(1024 * 1024), b''):
                    count += len(chunk)
                    if count > self.maximum_bytes: raise AiError('model_integrity', phase='import')
                    checksum.update(chunk)
                    outgoing.write(chunk)
                    if progress is not None: progress(count)
                current = os.fstat(incoming.fileno())
                if (original.st_size, original.st_mtime_ns) != (current.st_size, current.st_mtime_ns) or count != original.st_size:
                    raise AiError('model_integrity', phase='import')
                outgoing.flush()
                os.fsync(outgoing.fileno())
            metadata = self.header(temporary)
            digest = checksum.hexdigest()
            if expected_sha256 is not None and digest != expected_sha256:
                raise AiError('model_integrity', phase='import')
            target = self.root / (digest + '.gguf')
            try:
                # Atomic create without replacing an existing model artifact.
                if os.name == 'nt':
                    # Windows rename fails if target exists; unlike os.replace.
                    os.rename(temporary, target)
                else:
                    os.link(temporary, target)
            except FileExistsError:
                if self.digest(target) != digest: raise AiError('model_integrity', phase='import')
            return dict(artifact_id=digest, sha256=digest, bytes=count, **metadata)
        except OSError:
            raise AiError('model_unavailable', phase='import') from None
        finally:
            temporary.unlink(missing_ok=True)

    def resolve(self, artifact_id):
        if not isinstance(artifact_id, str) or not re.fullmatch('[0-9a-f]{64}', artifact_id):
            raise AiError('model_integrity', phase='runtime')
        target = self.root / (artifact_id + '.gguf')
        if target.is_symlink() or not target.is_file() or target.resolve().parent != self.root:
            raise AiError('model_unavailable', phase='runtime', details={'stage': 'configuration'})
        if self.digest(target) != artifact_id:
            raise AiError('model_integrity', phase='runtime')
        self.header(target)
        return target


class LlamaRuntime:
    def __init__(self, executable, store, *, threads=4, memory_limit_bytes=4*1024**3):
        self.executable, self.store = Path(executable).resolve(), store
        self.threads,self.memory_limit_bytes=threads,memory_limit_bytes

    @contextmanager
    def start(self, artifact_id, *, context_tokens=4096, startup_timeout=120):
        if not self.executable.is_file() or type(context_tokens) is not int or not 512 <= context_tokens <= 131072 or \
           type(self.threads) is not int or not 1<=self.threads<=256 or \
           type(self.memory_limit_bytes) is not int or not 64*1024**2<=self.memory_limit_bytes<=512*1024**3:
            raise AiError('model_unavailable', phase='runtime', details={'stage': 'configuration',
                          'context_type': type(context_tokens).__name__, 'threads_type': type(self.threads).__name__})
        model = self.store.resolve(artifact_id)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        token = secrets.token_urlsafe(32)
        # No shell, tool plugins, remote model downloads or public listening socket.
        command = [str(self.executable), '--model', str(model), '--host', '127.0.0.1',
                   '--port', str(port), '--ctx-size', str(context_tokens), '--no-webui',
                   '--offline','--no-agent','--no-mmproj','--parallel','1','--cache-ram','0',
                   '--threads',str(self.threads),'--threads-batch',str(self.threads),'--alias',artifact_id]
        environment = runtime_environment(os.environ,token)
        hidden = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        try:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL, env=environment, creationflags=hidden)
        except OSError:
            raise AiError('model_unavailable', phase='runtime', details={'stage': 'launch'}) from None
        try:
            owned_job=OwnedProcessJob(process,self.memory_limit_bytes)
        except AiError:
            if process.poll() is None: process.terminate()
            process.wait(timeout=10)
            raise
        opener = build_opener(ProxyHandler({}))
        endpoint = f'http://127.0.0.1:{port}/v1/chat/completions'
        try:
            deadline, ready = time.monotonic() + startup_timeout, False
            while time.monotonic() < deadline and process.poll() is None:
                try:
                    request = Request(f'http://127.0.0.1:{port}/health', headers={'Authorization': 'Bearer ' + token})
                    with opener.open(request, timeout=1) as response:
                        ready = response.status == 200 and json.loads(response.read(4096))['status'] == 'ok'
                    if ready: break
                except (URLError, OSError, ValueError, KeyError):
                    pass
                time.sleep(0.1)
            if not ready:
                raise AiError('model_unavailable', phase='runtime', details={'stage': 'startup', 'exit_code': process.poll()})
            template_verified,template_hash=False,None
            try:
                request=Request(f'http://127.0.0.1:{port}/props',headers={'Authorization':'Bearer '+token})
                with opener.open(request,timeout=2) as response:
                    payload=response.read(1024**2+1)
                    if len(payload)>1024**2: raise ValueError()
                    properties=json.loads(payload)
                caps=properties.get('chat_template_caps',{})
                template=properties.get('chat_template')
                template_verified=(isinstance(caps,dict) and caps.get('supports_tools') is True and
                                   caps.get('supports_tool_calls') is True and isinstance(template,str) and bool(template))
                if template_verified: template_hash=hashlib.sha256(template.encode('utf-8')).hexdigest()
            except (URLError,OSError,ValueError,TypeError):
                # Text-only models remain usable. Tool enablement requires the
                # loaded runtime's affirmative template capability, not a name.
                pass
            yield {'endpoint': endpoint, 'token': token, 'artifact_id': artifact_id, 'pid': process.pid,
                   'tool_template_verified':template_verified,'tool_template_sha256':template_hash}
        finally:
            try:
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=10)
            finally:
                owned_job.close()
