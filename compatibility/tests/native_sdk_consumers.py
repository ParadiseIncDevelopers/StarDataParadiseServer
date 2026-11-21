"""Packed Node and compiled external Java consumers against a real native TLS server."""
import argparse
import json
from pathlib import Path
import os
import secrets
import sys

from .dotnet_native_acceptance import native_bridge
from .fixture import Fixture,BASEDIR,BIN,MIGRATION,REPOSITORY,SHELL,query,require,run
sys.path.insert(1,str(REPOSITORY/'tests'))
from service_smoke import certificates


def check(fixture,root,node_entry,java_classpath,java):
    front,back=root/'front',root/'back'
    front.mkdir();back.mkdir()
    openssl=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe'
    for location in (front,back): certificates(str(openssl),location)
    user,password='native_installed',secrets.token_hex(24)
    with fixture.disposable(root/'server',tls_dir=back) as instance:
        environment,classic,_=instance
        port=next(arg.split('=',1)[1] for arg in classic if arg.startswith('--port='))
        setup=f"""CREATE DATABASE native_installed;
CREATE USER '{user}'@'localhost' IDENTIFIED BY '{password}' REQUIRE SSL;
GRANT SELECT,INSERT,UPDATE,DELETE,CREATE TEMPORARY TABLES,CREATE ROUTINE,ALTER ROUTINE,EXECUTE ON native_installed.* TO '{user}'@'localhost';
"""
        installed=run(classic,environment,30,script=setup+'\n')
        require(installed.returncode==0,'native installed consumer setup failed')
        uuid,version=query(instance,'SELECT @@server_uuid,@@version;').split('\t')
        command=[sys.executable,'-B',str(REPOSITORY/'compatibility/wire_v1/service.py'),'--port','0',
          '--cert',str(front/'server.pem'),'--key',str(front/'server.key'),'--mysql-port',port,
          '--mysql-ca',str(back/'ca.pem'),'--mysql-uuid',uuid,'--product-version',version.removesuffix('-StarDataParadise'),
          '--schema','native_installed','--native-sql']
        bridge_environment=environment.copy()
        bridge_environment['PYTHONPATH']=str(MIGRATION/'wire-v1/dependencies')+os.pathsep+bridge_environment.get('PYTHONPATH','')
        with native_bridge(command,bridge_environment,root) as gateway:
            consumer_environment=environment.copy()
            consumer_environment['SDP_NATIVE_SETTINGS']=json.dumps({'host':'127.0.0.1','port':gateway,'tlsName':'localhost',
                  'caFile':str(front/'ca.pem'),'user':user,'password':password,'timeoutMs':75000})
            consumer_environment['SDP_NATIVE_NODE_ENTRY']=str(node_entry)
            consumers=[('Node',['node',str(REPOSITORY/'compatibility/tests/native_node_consumer.mjs')]),
                       ('Java',[str(java),'-cp',java_classpath,'NativeJavaConsumer'])]
            for name,command in consumers:
                result=run(command,consumer_environment,120)
                if result.returncode:
                    diagnostic=REPOSITORY/'out/sdk-native'/('installed-'+name.lower()+'-failure.log')
                    diagnostic.write_text((result.stdout+result.stderr).replace(password,'<redacted>'),encoding='utf-8')
                    raise RuntimeError(name+' installed consumer failed; redacted log '+str(diagnostic))
                require('PASS installed '+name+' native TLS' in result.stdout,'consumer did not run acceptance')
                print(result.stdout.strip(),flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--node-entry',type=Path,required=True)
    parser.add_argument('--java-classpath',required=True)
    parser.add_argument('--java',type=Path,required=True)
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('native-installed') as root:
        check(fixture,root,args.node_entry.resolve(strict=True),args.java_classpath,args.java.resolve(strict=True))


if __name__=='__main__':main()
