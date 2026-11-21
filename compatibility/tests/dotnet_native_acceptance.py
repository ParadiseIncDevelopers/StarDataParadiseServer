"""Run built .NET native SDK consumers against a real isolated TLS SQL server.

This verifies transport and ordinary native prepared SQL, not new HIST/AI syntax.
Credentials are passed through stdin to setup and environment to consumers.
"""
import argparse
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import time
from xml.etree import ElementTree
from zipfile import ZipFile

from .fixture import BASEDIR, BIN, HIDDEN, MIGRATION, REPOSITORY, SHELL, Fixture, query, require, run

sys.path.insert(1,str(REPOSITORY/'tests'))
from service_smoke import certificates


@contextmanager
def native_bridge(command,environment,root):
    log=root/'dotnet-native-bridge.log'
    with log.open('wb') as output:
        process=subprocess.Popen(command,env=environment,stdin=subprocess.DEVNULL,stdout=output,
                                 stderr=subprocess.STDOUT,creationflags=HIDDEN)
        try:
            deadline=time.monotonic()+15
            while time.monotonic()<deadline:
                text=log.read_text(encoding='utf-8',errors='replace')
                ready=re.search(r'^SDP_WIRE_V1_READY (\d+)$',text,re.M)
                if ready:
                    yield int(ready[1])
                    return
                require(process.poll() is None,'native SDK bridge stopped before readiness: '+text)
                time.sleep(.1)
            raise RuntimeError('native SDK bridge did not become ready')
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=15)


def quoted(value):
    return '"'+str(value).replace('"','""')+'"'


def check(fixture,root,sdk,installed_packages=False):
    consumers=(
        ('net8.0',['dotnet',str(sdk/'out/native-consumer/bin/NativeSmoke/release_net8.0/NativeSmoke.dll')]),
        ('net10.0',['dotnet',str(sdk/'out/native-consumer/bin/NativeSmoke/release_net10.0/NativeSmoke.dll')]),
        ('net48',[str(sdk/'out/native-consumer-framework/bin/NativeSmoke/release/NativeSmoke.exe')]),
    )
    if installed_packages:
        consumers=[]
        for target in ('net8.0','net10.0','net48'):
            framework=target=='net48'
            project=sdk/('framework/StarDataParadiseNetFramework.csproj' if framework else 'src/StarDataParadise.SDK.NET.csproj')
            metadata=ElementTree.parse(project).getroot()
            package=metadata.findtext('.//PackageId');version=metadata.findtext('.//Version')
            directory=sdk/'native-smoke/bin/Release'/target
            assembly=directory/'StarDataParadise.SDK.NET.dll'
            with ZipFile(sdk/'out/packages'/f'{package}.{version}.nupkg') as archive:
                expected=hashlib.sha256(archive.read(f'lib/{target}/StarDataParadise.SDK.NET.dll')).digest()
            require(assembly.is_file() and hashlib.sha256(assembly.read_bytes()).digest()==expected,
                    target+' consumer DLL does not match the current stable nupkg')
            executable=directory/('NativeSmoke.exe' if framework else 'NativeSmoke.dll')
            consumers.append((target,[str(executable)] if framework else ['dotnet',str(executable)]))
        print('PASS three installed consumer DLLs exactly match the current stable nupkg payloads',flush=True)
    require(all(Path(command[-1]).is_file() for _,command in consumers),'build the three SDK.NET native-smoke target consumers first')
    front,back=root/'front',root/'back'
    front.mkdir();back.mkdir()
    openssl=MIGRATION/'toolchain/openssl-firedaemon/x64/bin/openssl.exe'
    for location in (front,back): certificates(str(openssl),location)
    user,password='dotnet_native_consumer',secrets.token_hex(24)
    with fixture.disposable(root/'server',tls_dir=back) as instance:
        environment,classic,_=instance
        port=next(arg.split('=',1)[1] for arg in classic if arg.startswith('--port='))
        bootstrap=f"""CREATE DATABASE dotnet_native;
          CREATE USER '{user}'@'localhost' IDENTIFIED BY '{password}' REQUIRE SSL;
          GRANT SELECT,INSERT,UPDATE,DELETE,CREATE TEMPORARY TABLES,CREATE ROUTINE,ALTER ROUTINE,EXECUTE ON dotnet_native.* TO '{user}'@'localhost';
        """
        installed=run(classic,environment,30,script=bootstrap+'\n')
        require(installed.returncode==0,'disposable native SDK account setup failed')
        uuid,version=query(instance,'SELECT @@server_uuid,@@version;').split('\t')
        command=[sys.executable,'-B',str(REPOSITORY/'compatibility/wire_v1/service.py'),
          '--port','0','--cert',str(front/'server.pem'),'--key',str(front/'server.key'),
          '--mysql-port',port,'--mysql-ca',str(back/'ca.pem'),'--mysql-uuid',uuid,
          '--product-version',version.removesuffix('-StarDataParadise'),
          '--schema','dotnet_native','--native-sql']
        bridge_environment=environment.copy()
        connector=MIGRATION/'wire-v1/dependencies'
        require((connector/'mysql/connector/__init__.py').is_file(),'install the pinned wire-v1 mysql connector dependency first')
        bridge_environment['PYTHONPATH']=str(connector)+os.pathsep+bridge_environment.get('PYTHONPATH','')
        with native_bridge(command,bridge_environment,root) as gateway:
            consumer_environment=environment.copy()
            consumer_environment['SDP_NATIVE_CONNECTION']=(
                f'Host=127.0.0.1;Port={gateway};TLS Name=localhost;CA File={quoted(front/"ca.pem")};'
                f'User={user};Password={password};Timeout=75;Pooling=false;')
            for target,command in consumers:
                result=run(command,consumer_environment,120)
                # Redact the generated secret before persisting diagnostics;
                # never put the environment connection text on the command line.
                if result.returncode:
                    diagnostic=REPOSITORY/'out'/('dotnet-native-'+target+'-tls-diagnostic.log')
                    diagnostic.write_text((result.stdout+result.stderr).replace(password,'<redacted>'),encoding='utf-8')
                    bridge_log=root/'dotnet-native-bridge.log'
                    if bridge_log.is_file():
                        (REPOSITORY/'out/dotnet-native-bridge-diagnostic.log').write_text(
                            bridge_log.read_text(encoding='utf-8',errors='replace').replace(password,'<redacted>'),encoding='utf-8')
                    raise RuntimeError(f'{target} native .NET consumer failed; redacted diagnostic: {diagnostic}')
                require('PASS actual native SQL/TLS .NET' in result.stdout,
                        f'{target} consumer did not execute real SQL/TLS acceptance')
                print('PASS .NET '+target+' actual TLS/native PS exact values, rollback and chunked JSON; boundary suite also passed',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mysqld',type=Path,default=BIN/'mysqld.exe')
    parser.add_argument('--mysql',type=Path,default=BIN/'mysql.exe')
    parser.add_argument('--basedir',type=Path,default=BASEDIR)
    parser.add_argument('--sdk',type=Path,default=REPOSITORY.parent/'StarDataParadise.SDK.NET')
    parser.add_argument('--installed-packages',action='store_true')
    args=parser.parse_args()
    fixture=Fixture(args.mysqld.resolve(strict=True),args.mysql.resolve(strict=True),args.basedir.resolve(strict=True),SHELL,None)
    with fixture.scratch('dotnet-native-sdk') as root:
        check(fixture,root,args.sdk.resolve(strict=True),args.installed_packages)


if __name__=='__main__':
    main()
