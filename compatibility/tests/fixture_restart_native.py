"""Owned maintenance bootstrap/restart and original-error preservation."""
import argparse
from pathlib import Path
from .ai_catalog_acceptance import execute
from .fixture import BASEDIR, BIN, MIGRATION, SHELL, Fixture, query, require
from .workbench_native import backend_certificate


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('native-maintenance-restart') as root:
        cert=backend_certificate(MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe',root,'cert')
        observed=[]
        def bootstrap(instance):
            require(query(instance,'SELECT @@max_connections')=='32','bootstrap options were not used')
            observed.append(query(instance,'SELECT @@datadir'))
            execute(instance,'CREATE SCHEMA bootstrap_fixture;CREATE TABLE bootstrap_fixture.persistent_state(id INT PRIMARY KEY) ENGINE=InnoDB;INSERT INTO bootstrap_fixture.persistent_state VALUES(1);')
        with fixture.disposable(root/'server',tls_dir=cert,bootstrap=bootstrap,
                bootstrap_options=('--max-connections=32',),server_options=('--max-connections=64',)) as instance:
            require(query(instance,'SELECT @@max_connections')=='64','final server options were not used')
            require(query(instance,'SELECT @@datadir')==observed[0],'restart changed the disposable datadir')
            require(query(instance,'SELECT id FROM bootstrap_fixture.persistent_state')=='1','restart lost committed bootstrap data')
            try:instance.restart(server_options=('--datadir=outside-the-fixture',))
            except RuntimeError:pass
            else:raise AssertionError('restart allowed a datadir escape')
            require(query(instance,'SELECT @@max_connections')=='64','rejected profile changed the live Server')
            for limit in (48,64):
                instance.restart(server_options=('--max-connections='+str(limit),))
                require(query(instance,'SELECT @@max_connections')==str(limit),'owned maintenance restart did not apply its profile')
                require(query(instance,'SELECT id FROM bootstrap_fixture.persistent_state')=='1','maintenance restart lost persisted data')
        print('PASS maintenance bootstrap -> clean exit0 -> final startup: same TLS datadir and persisted data, distinct startup options')
    with fixture.scratch('native-maintenance-failure') as root:
        cert=backend_certificate(MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe',root,'cert')
        def failure(instance):
            require(query(instance,'SELECT 1')=='1','bootstrap was not connected')
            raise RuntimeError('fixture-bootstrap-primary-error')
        try:
            with fixture.disposable(root/'server',tls_dir=cert,bootstrap=failure,
                    server_options=('--fixture-final-must-not-start=ON',)):
                raise AssertionError('failed bootstrap unexpectedly yielded final server')
        except RuntimeError as error:
            require(str(error)=='fixture-bootstrap-primary-error','cleanup replaced the bootstrap failure: '+repr(error))
        else:
            raise AssertionError('failed bootstrap did not propagate its original exception')
        log=(root/'server/mysqld.log').read_text(encoding='utf-8',errors='replace')
        require('fixture-final-must-not-start' not in log,'final process started after bootstrap failure')
        print('PASS failed bootstrap: final process never started and the primary callback exception was preserved')


if __name__=='__main__':main()
