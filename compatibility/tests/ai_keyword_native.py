"""AI keyword compatibility with ordinary native functions and flag OFF/ON."""
import argparse
from pathlib import Path

from .ai_catalog_acceptance import execute
from .fixture import BASEDIR, BIN, MIGRATION, SHELL, Fixture, query, require
from .workbench_native import backend_certificate


NAMES=('ai','conversation','conversations','knowledge','knowledges','behavior','behaviors',
       'response','models','model','provider','generative','append','prompt','functions',
       'message','persistent','ticket','run','async','cancel','memory','context','finish',
       'metadata','tool','approval','approve','deny','id')


def exercise(instance,enabled):
    execute(instance,'CREATE DATABASE keyword_compat;')
    version=query(instance,'SELECT VERSION(),@@version;').split('\t')
    require(len(version)==2 and version[0]==version[1],'native VERSION() bootstrap behavior changed')
    for index,name in enumerate(NAMES):
        execute(instance,'CREATE FUNCTION keyword_compat.%s(p INT) RETURNS INT DETERMINISTIC RETURN p+%d;'%(name,index))
    source='USE keyword_compat;SELECT '+','.join('%s(7)'%name for name in NAMES)+';'
    expected='\t'.join(str(7+index) for index in range(len(NAMES)))
    require(execute(instance,source)==expected,'AI keyword blocked ordinary unqualified function dispatch')
    spaced='USE keyword_compat;SELECT '+','.join('%s (11)'%name.upper() for name in NAMES)+';'
    require(execute(instance,spaced)=='\t'.join(str(11+index) for index in range(len(NAMES))),
            'keyword function dispatch changed with whitespace or case')
    if not enabled:
        execute(instance,'CREATE TABLE keyword_compat.model(name INT);')
        fields=execute(instance,'USE keyword_compat;DESCRIBE model name;').split('\t')
        require(fields[0]=='name' and fields[1]=='int','flag OFF broke legacy DESCRIBE model column')
    print('PASS native AI keyword function compatibility: '+str(len(NAMES))+' unqualified names, case/space, VERSION(), AI flag '+('ON' if enabled else 'OFF'))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--openssl',type=Path,default=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('ai-keyword-native') as root:
        certificates=backend_certificate(args.openssl.resolve(strict=True),root,'cert')
        for enabled in (False,True):
            with fixture.disposable(root/('enabled' if enabled else 'disabled'),tls_dir=certificates,
                    server_options=('--sdp-ai-sql=ON',) if enabled else ()) as instance:
                exercise(instance,enabled)


if __name__=='__main__':
    main()
