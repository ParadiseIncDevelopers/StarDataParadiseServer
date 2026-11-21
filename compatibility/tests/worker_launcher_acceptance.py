"""Black-box Windows launcher regression with a poisoned parent Python environment."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--launcher', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    scratch = root / 'out/compatibility-tests'; scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='worker-launcher-', dir=scratch) as temporary:
        directory = Path(temporary)
        entry = directory / 'runtime/compatibility/ai/Run-LocalWorker.py'
        entry.parent.mkdir(parents=True); entry.write_text('# test entry')
        source = directory / 'Probe.cs'
        source.write_text('''using System;using System.IO;
class Probe {static int Main(string[] args) {
foreach(string key in Environment.GetEnvironmentVariables().Keys)
 if(key.StartsWith("PYTHON",StringComparison.OrdinalIgnoreCase))return 7;
if(Environment.GetEnvironmentVariable("SDP_TEST_KEEP")!="kept")return 8;
File.WriteAllText(Environment.GetEnvironmentVariable("SDP_TEST_RESULT"),"clean");return 0;}}''')
        cli = directory / 'mysqlsh.exe'
        subprocess.run([str(args.compiler.resolve()), '/nologo', '/target:exe', '/out:' + str(cli), str(source)], check=True)
        (directory / 'connection.json').write_text(json.dumps(dict(mysqlsh_executable=str(cli))))
        result = directory / 'probe.result'
        environment = dict(os.environ, PYTHONHOME='not-the-worker-python', PYTHONPATH='not-the-worker-path',
                           PYTHONSTARTUP='unexpected-startup.py', SDP_TEST_KEEP='kept', SDP_TEST_RESULT=str(result))
        command = [str(args.launcher.resolve()), '--deployment', str(directory)]
        active = subprocess.run(command, env=environment, timeout=20)
        assert active.returncode == 0 and result.read_text() == 'clean'
        (directory / 'connection.json').unlink()
        invalid = subprocess.run(command, env=environment, timeout=20)
        assert invalid.returncode == 1
        assert 'FileNotFoundException' in (directory / 'launcher-error.log').read_text()
    print('PASS real compiled launcher isolates embedded PYTHON*, preserves unrelated environment, waits for child and fails closed on missing deployment.')


if __name__ == '__main__': main()
