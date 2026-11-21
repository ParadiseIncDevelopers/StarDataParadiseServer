"""mysqlsh entry point for the installed Windows user worker (no secrets in argv)."""
import json
import os
from pathlib import Path
import sys


def main(directory):
    import msvcrt
    from mysqlsh import mysql
    from compatibility.ai.secrets import SecretStore
    from compatibility.ai.service import serve
    directory = Path(directory).resolve(strict=True)
    with (directory / 'instance.lock').open('a+b') as lock:
        lock.seek(0); lock.write(b'0'); lock.flush(); lock.seek(0)
        try:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            return # Existing worker retains its connection and claims.
        details = json.loads((directory / 'connection.json').read_text(encoding='utf-8-sig'))
        def connect():
            options = dict(details['sql'])
            if options.get('ssl-mode') != 'VERIFY_IDENTITY' or options.get('host') not in ('localhost', '127.0.0.1', '::1'):
                raise ValueError('Installed local worker requires verified loopback TLS')
            options['password'] = SecretStore._windows(details['credential_target'])
            session = mysql.get_classic_session(options)
            try:
                identity = session.run_sql('SELECT @@server_uuid, STARDATAPARADISE_ACTOR()').fetch_one()
                if list(identity) != [details['server_uuid'], details['worker_actor']]:
                    raise ValueError('Worker SQL destination identity changed')
                return session
            except Exception:
                session.close(); raise
        serve(connect, directory / 'config.json', directory / 'status.json', directory / 'stop')


if __name__ == '__main__':
    main(sys.argv[1])
