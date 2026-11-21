"""Bounded Windows child lifetime; closing/crashing the worker kills its runtime."""
import os

from .contracts import AiError


def runtime_environment(environment, token):
    # A local inference process does not need the worker's cloud credentials,
    # database password variables, proxy secrets or llama tool/plugin settings.
    allowed={'SYSTEMROOT','WINDIR','SYSTEMDRIVE','PATH','PATHEXT','TEMP','TMP',
             'USERPROFILE','HOMEDRIVE','HOMEPATH','APPDATA','LOCALAPPDATA','HOME','LANG','LC_ALL'}
    result={name:value for name,value in environment.items() if name.upper() in allowed}
    result['LLAMA_API_KEY']=token
    return result


class OwnedProcessJob:
    def __init__(self, process, memory_limit_bytes):
        self.handle=None
        if os.name!='nt': return
        import ctypes
        from ctypes import wintypes
        class Basic(ctypes.Structure):
            _fields_=[('process_time',ctypes.c_longlong),('job_time',ctypes.c_longlong),
                      ('flags',wintypes.DWORD),('min_ws',ctypes.c_size_t),('max_ws',ctypes.c_size_t),
                      ('active_processes',wintypes.DWORD),('affinity',ctypes.c_size_t),
                      ('priority',wintypes.DWORD),('scheduling',wintypes.DWORD)]
        class Io(ctypes.Structure):
            _fields_=[(name,ctypes.c_ulonglong) for name in ('read_ops','write_ops','other_ops','read_bytes','write_bytes','other_bytes')]
        class Limits(ctypes.Structure):
            _fields_=[('basic',Basic),('io',Io),('process_memory',ctypes.c_size_t),
                      ('job_memory',ctypes.c_size_t),('peak_process',ctypes.c_size_t),('peak_job',ctypes.c_size_t)]
        api=ctypes.WinDLL('kernel32',use_last_error=True)
        api.CreateJobObjectW.argtypes=[ctypes.c_void_p,wintypes.LPCWSTR]
        api.CreateJobObjectW.restype=wintypes.HANDLE
        api.SetInformationJobObject.argtypes=[wintypes.HANDLE,ctypes.c_int,ctypes.c_void_p,wintypes.DWORD]
        api.SetInformationJobObject.restype=wintypes.BOOL
        api.AssignProcessToJobObject.argtypes=[wintypes.HANDLE,wintypes.HANDLE]
        api.AssignProcessToJobObject.restype=wintypes.BOOL
        api.CloseHandle.argtypes=[wintypes.HANDLE]
        api.CloseHandle.restype=wintypes.BOOL
        self.api=api
        handle=api.CreateJobObjectW(None,None)
        if not handle:
            raise AiError('model_unavailable',phase='runtime',details={'stage':'create_job','system_code':ctypes.get_last_error()})
        limits=Limits()
        # KILL_ON_JOB_CLOSE, PROCESS_MEMORY, ACTIVE_PROCESS. The text inference
        # runtime owns one process and cannot launch shell/tool subprocesses.
        limits.basic.flags=0x2000|0x100|0x8
        limits.basic.active_processes=1
        limits.process_memory=memory_limit_bytes
        configured=api.SetInformationJobObject(handle,9,ctypes.byref(limits),ctypes.sizeof(limits))
        assigned=configured and api.AssignProcessToJobObject(handle,int(process._handle))
        if not assigned:
            system_code=ctypes.get_last_error()
            api.CloseHandle(handle)
            raise AiError('model_unavailable',phase='runtime',details={'stage':'assign_job' if configured else 'configure_job',
                          'system_code':system_code})
        self.handle=handle

    def close(self):
        if self.handle is not None:
            self.api.CloseHandle(self.handle)
            self.handle=None
