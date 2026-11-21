"""Compile/run the exact native FULL iterator driver; not SQL feature acceptance."""
import os
from pathlib import Path
import subprocess


def main():
    repo=Path(__file__).resolve().parents[2]
    out=repo/'out/full-outer-next';out.mkdir(parents=True,exist_ok=True)
    vc=Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231')
    kit=Path('C:/Program Files (x86)/Windows Kits/10')
    env=os.environ.copy()
    env['INCLUDE']=';'.join(str(p) for p in [vc/'include',kit/'Include/10.0.26100.0/ucrt',kit/'Include/10.0.26100.0/shared',kit/'Include/10.0.26100.0/um'])
    env['LIB']=';'.join(str(p) for p in [vc/'lib/x64',kit/'Lib/10.0.26100.0/ucrt/x64',kit/'Lib/10.0.26100.0/um/x64'])
    hidden=getattr(subprocess,'CREATE_NO_WINDOW',0)
    exe=out/'driver_acceptance.exe'
    command=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo','/EHsc','/std:c++17','/utf-8',
             '/I'+str(repo/'compatibility/upstream/native'),str(Path(__file__).with_suffix('.cpp')),
             '/Fe'+str(exe),'/Fo'+str(out/'driver_acceptance.obj')]
    result=subprocess.run(command,env=env,capture_output=True,text=True,errors='replace',creationflags=hidden)
    (out/'driver-compile.log').write_text(result.stdout+result.stderr,encoding='utf-8')
    if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    result=subprocess.run([str(exe)],env=env,capture_output=True,text=True,errors='replace',creationflags=hidden)
    (out/'driver-result.log').write_text(result.stdout+result.stderr,encoding='utf-8')
    if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    print(result.stdout.strip())


if __name__=='__main__':
    main()
