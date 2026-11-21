"""Snapshot and verify the complete native patch against the pinned upstream.

Only source edits and explicit SDP native/test files enter the patch. Verification
uses an isolated index/object directory; it never resets the developer checkout.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

PINNED='561753af986c3c006c4b6c7caa8359748fb55b22'
REPOSITORY=Path(__file__).resolve().parents[2]


def git(source,*args,environment=None):
    result=subprocess.run(['git',*args],cwd=source,env=environment,stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if result.returncode:raise RuntimeError(result.stderr.decode('utf-8',errors='replace'))
    return result.stdout


def normalized(path):return path.read_bytes().replace(b'\r\n',b'\n')


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args(argv)
    source=args.source.resolve(strict=True);output=args.output.resolve()
    if git(source,'rev-parse','HEAD').decode().strip()!=PINNED:raise RuntimeError('unexpected upstream HEAD')
    if git(source,'diff','--cached','--name-only'):raise RuntimeError('source index has staged changes; preserve it')
    tracked=[value.decode('utf-8') for value in git(source,'diff','--name-only','-z','HEAD','--').split(b'\0') if value]
    added=[value.decode('utf-8') for value in git(source,'ls-files','--others','--exclude-standard','-z').split(b'\0') if value]
    for name in added:
        path=Path(name)
        if (source/path).is_symlink():raise RuntimeError('untracked native source is a symlink: '+name)
        if not ((path.parts[0]=='sql' and path.name.startswith('sdp_') and path.suffix in ('.h','.cc')) or
                (path.parent.as_posix() in ('mysql-test/t','mysql-test/r') and path.name.startswith('sdp_') and path.suffix in ('.test','.result'))):
            raise RuntimeError('unexpected untracked source: '+name)
    patch=git(source,'diff','--no-ext-diff','--binary','HEAD','--')
    for name in sorted(added):
        contents=normalized(source/name).decode('utf-8')
        header=f'diff --git a/{name} b/{name}\nnew file mode 100644\n'
        difference=''.join(difflib.unified_diff([],contents.splitlines(keepends=True),fromfile='/dev/null',tofile='b/'+name))
        # Native source files must have a trailing newline so the snapshot has
        # no ambiguous no-newline marker or compiler-specific EOF behavior.
        if contents and not contents.endswith('\n'):raise RuntimeError('missing newline: '+name)
        patch+=(header+difference).encode('utf-8')
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_bytes(patch)
    verification=REPOSITORY/'out/native-patch-verification'
    verification.mkdir(parents=True,exist_ok=True)
    verified_root=verification.resolve()
    verified_root.relative_to((REPOSITORY/'out').resolve())
    objects=Path(git(source,'rev-parse','--git-path','objects').decode().strip())
    if not objects.is_absolute():objects=source/objects
    files={}
    with tempfile.TemporaryDirectory(prefix='snapshot-',dir=verified_root) as directory:
        temporary=Path(directory).resolve()
        temporary.relative_to(verified_root)
        (temporary/'objects').mkdir()
        environment=os.environ.copy()
        environment.update(GIT_INDEX_FILE=str(temporary/'index'),GIT_OBJECT_DIRECTORY=str(temporary/'objects'),
                           GIT_ALTERNATE_OBJECT_DIRECTORIES=str(objects.resolve(strict=True)))
        git(source,'read-tree',PINNED,environment=environment)
        git(source,'apply','--cached','--check',str(output),environment=environment)
        git(source,'apply','--cached',str(output),environment=environment)
        for name in sorted(set(tracked+added)):
            blob=git(source,'show',':'+name,environment=environment)
            actual=normalized(source/name)
            if blob!=actual:raise RuntimeError('snapshot did not reproduce normalized source bytes: '+name)
            files[name]=hashlib.sha256(actual).hexdigest()
    manifest={'upstream':PINNED,'patch_sha256':hashlib.sha256(patch).hexdigest(),
              'source_files':files,'clean_index_apply':'PASS','normalized_source_reproduction':'PASS'}
    output.with_suffix(output.suffix+'.manifest.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n',encoding='utf-8')
    print(json.dumps({key:value for key,value in manifest.items() if key!='source_files'}))
    print('PASS complete native source snapshot:',len(files),'files,',len(patch),'bytes')


if __name__=='__main__':main()
