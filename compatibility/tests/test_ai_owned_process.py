"""Local inference children receive no worker secrets and die with their owner."""
import os
import subprocess
import sys
import unittest

from compatibility.ai.owned_process import OwnedProcessJob, runtime_environment


class OwnedProcessTests(unittest.TestCase):
    def test_environment_excludes_credentials_proxies_and_runtime_plugins(self):
        environment = {'SystemRoot': 'C:/Windows', 'PATH': 'runtime-path', 'TEMP': 'temp',
                       'OPENAI_API_KEY': 'secret', 'ANTHROPIC_API_KEY': 'secret',
                       'SDP_PASSWORD': 'secret', 'HTTP_PROXY': 'secret',
                       'LLAMA_API_KEY': 'old', 'LLAMA_ARG_TOOLS': 'shell',
                       'LLAMA_ARG_MODEL_URL': 'https://example.invalid/model'}
        self.assertEqual({'SystemRoot': 'C:/Windows', 'PATH': 'runtime-path', 'TEMP': 'temp',
                          'LLAMA_API_KEY': 'ephemeral'}, runtime_environment(environment, 'ephemeral'))

    @unittest.skipUnless(os.name == 'nt', 'Windows Job Object contract')
    def test_closing_job_terminates_only_owned_child(self):
        child = subprocess.Popen([sys.executable, '-B', '-c', 'import time; time.sleep(120)'],
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
        job = None
        try:
            job = OwnedProcessJob(child, 256 * 1024**2)
            self.assertIsNone(child.poll())
            job.close()
            self.assertIsNotNone(child.wait(timeout=10))
            job.close()
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=10)
            if job is not None: job.close()


if __name__ == '__main__': unittest.main()
