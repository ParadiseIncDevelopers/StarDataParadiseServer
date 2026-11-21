"""Installed Python SDK consumer for a real, preclaimed native tool deadline."""
import importlib.metadata
import json
import os
from pathlib import Path

import stardataparadise_sdk
from stardataparadise_sdk import Error, connect_native


def main():
    settings=json.loads(os.environ['SDP_NATIVE_TOOL_SETTINGS'])
    package=Path(stardataparadise_sdk.__file__).resolve()
    if 'site-packages' not in package.parts:
        raise RuntimeError('The tool ABI consumer must use an installed SDK, not a source checkout.')
    version=importlib.metadata.version('stardataparadise-sdk')
    if version!='0.3.0':
        raise RuntimeError('Expected installed SDK0.3.0; got '+version)
    with connect_native(host='127.0.0.1',port=settings['port'],user=settings['user'],password=settings['password'],
            ca_file=settings['ca_file'],tls_name='localhost',timeout=30) as connection:
        connection.execute('SET SESSION sdp_ai_worker_rpc_guard=ON')
        connection.execute("SET SESSION time_zone='+00:00'")
        try:
            connection.execute('EXECUTE AI TOOL CALL ?', (settings['call_id'],))
        except Error as error:
            actual={key:getattr(error,key,None) for key in ('code','native_errno','sqlstate','phase','commit_state',
                'run_state','run_id','call_id','action_commit_state','unknown_outcome','retry','cause_native_errno','cause_sqlstate')}
            expected=dict(code=510005,native_errno=61085,sqlstate='45000',phase='tool',commit_state='committed',
                run_state='running',run_id=settings['run_id'],call_id=settings['call_id'],action_commit_state='rolled_back',
                unknown_outcome=False,retry={'action':'NONE'},cause_native_errno=3024,cause_sqlstate='HY000')
            if actual!=expected:
                raise RuntimeError('Real native tool rollback lost SDK ABI fields: '+repr(actual))
            actual['message']=str(error)
            print(json.dumps(dict(proof=actual,sdk_version=version,sdk_file=str(package))))
        else:
            raise RuntimeError('Real CPU tool deadline unexpectedly succeeded through the installed SDK.')


if __name__=='__main__':
    main()
