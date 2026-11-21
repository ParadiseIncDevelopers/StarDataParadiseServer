import json
from pathlib import Path
import tempfile
import unittest
from compatibility.ai.service import serve

class Session:
    closed=False
    def run_sql(self,sql):return self
    def fetch_one(self):return ('Ssl_cipher','TLS_AES_256_GCM_SHA384')
    def close(self):self.closed=True

class ServiceTests(unittest.TestCase):
    def test_reload_between_jobs_and_close_session(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);config=root/'config.json';status=root/'status.json'
            config.write_text('{"credentials":{}}');observed=[];session=Session()
            class Worker:
                def __init__(self,session,value):observed.append(value)
                def work_once(self):
                    config.write_text('{"credentials":{"owner":{}}}')
                    return None
            serve(lambda:session,config,status,root/'stop',sleep=lambda _:None,worker_type=Worker,max_cycles=2)
            self.assertEqual(observed,[{'credentials':{}},{'credentials':{'owner':{}}}])
            self.assertTrue(session.closed)
            self.assertEqual(json.loads(status.read_text())['state'],'stopped')
    def test_errors_are_redacted_and_reconnect_without_repeating_rpc(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);config=root/'config.json';status=root/'status.json';config.write_text('{}')
            calls=[];sessions=[];errors=[]
            def connect():
                session=Session();sessions.append(session);return session
            class Worker:
                def __init__(self,session,value):self.session=session
                def work_once(self):
                    calls.append(self.session)
                    if len(calls)==1:raise RuntimeError('secret-key-and-sql')
                    return {'run_id':'123','state':'succeeded','response':'private'}
            def sleep(_):errors.append(json.loads(status.read_text()))
            serve(connect,config,status,root/'stop',sleep=sleep,worker_type=Worker,max_cycles=2)
            self.assertEqual(len(sessions),2)
            self.assertIsNot(calls[0],calls[1])
            self.assertEqual(errors[0]['error'],'RuntimeError')
            self.assertNotIn('secret',json.dumps(errors))
            self.assertNotIn('private',status.read_text())
    def test_no_plaintext_session(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);config=root/'config';config.write_text('{}');status=root/'status'
            class Plain(Session):
                def fetch_one(self):return ('Ssl_cipher','')
            observed=[]
            def never_worker(*args):self.fail('Plaintext session reached worker')
            serve(Plain,config,status,root/'stop',sleep=lambda _:observed.append(json.loads(status.read_text())),worker_type=never_worker,max_cycles=1)
            self.assertEqual(observed[0]['state'],'error')
            self.assertEqual(observed[0]['error'],'ValueError')

if __name__=='__main__':unittest.main()
