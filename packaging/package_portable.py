"""Stage checked portable release payloads, excluding local state and build caches."""
import argparse,hashlib,json,re,shutil,zipfile
from pathlib import Path

SERVER=Path(__file__).resolve().parents[1]
WB=SERVER.parent/'StarDataParadise'
MIGRATION=SERVER/'out/mysql-migration'
BFAST=MIGRATION/'bfast'
SOURCE=MIGRATION/'server-upstream-baseline'

def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def copy(source,root,relative):
    if source.is_symlink() or not source.is_file():raise RuntimeError('Missing or linked payload: '+str(source))
    target=root/relative;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)

def finalize(root,archive):
    files={p.relative_to(root).as_posix():digest(p) for p in sorted(root.rglob('*')) if p.is_file()}
    (root/'payload-sha256.json').write_text(json.dumps(files,indent=2)+'\n',encoding='utf-8')
    with zipfile.ZipFile(archive,'x',zipfile.ZIP_DEFLATED,compresslevel=4) as bundle:
        for path in sorted(root.rglob('*')):
            if path.is_file():bundle.write(path,path.relative_to(root))
    with zipfile.ZipFile(archive) as bundle:
        if bundle.testzip():raise RuntimeError('Release archive CRC failed')
        for name,expected in files.items():
            if hashlib.sha256(bundle.read(name)).hexdigest()!=expected:raise RuntimeError('Archive byte mismatch: '+name)
    result={'file':archive.name,'sha256':digest(archive),'bytes':archive.stat().st_size,'files':len(files)+1}
    archive.with_suffix('.manifest.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result),flush=True)

def server(runtime,stage):
    root=stage/'server';root.mkdir()
    for path in sorted(runtime.iterdir()):
        if path.name=='mysqld.exe' or path.suffix.lower()=='.dll':copy(path,root,Path('bin')/path.name)
    if not (root/'bin/mysqld.exe').is_file():raise RuntimeError('Native server missing')
    copy(BFAST/'runtime_output_directory/RelWithDebInfo/mysql.exe',root,'bin/mysql.exe')
    for name in ('component_reference_cache.dll','sdp_mysql_model_udf.dll'):
        copy(BFAST/'lib/plugin'/name,root,Path('lib/plugin')/name)
    for path in sorted((BFAST/'share').glob('*/errmsg.sys')):copy(path,root,Path('share')/path.parent.name/path.name)
    copy(BFAST/'share/dictionary.txt',root,'share/dictionary.txt')
    for path in sorted((SOURCE/'share/charsets').glob('*.xml')):copy(path,root,Path('share/charsets')/path.name)
    copy(SOURCE/'LICENSE',root,'LICENSE')
    copy(SERVER/'compatibility/ml/classic_model.sql',root,'sql/classic_model.sql')
    copy(SERVER/'release.json',root,'release.json')
    copy(SERVER/'out/release-0.5.2/bin/StarDataParadiseAiWorker.exe',root,'bin/StarDataParadiseAiWorker.exe')
    for relative in ('compatibility/ai','compatibility/wire_v1'):
        for path in sorted((SERVER/relative).rglob('*')):
            if not path.is_file() or '__pycache__' in path.parts or path.suffix not in ('.py','.sql','.md','.json','.txt','.ps1','.cs'):continue
            if relative=='compatibility/ai' and 'native' in path.relative_to(SERVER/relative).parts:continue
            if path.name.startswith('test_') or path.name.endswith('_acceptance.py') or path.name in ('acceptance.py','native_fork.py','durable_fork.py'):continue
            copy(path,root,path.relative_to(SERVER))
    copy(SERVER/'compatibility/jobs/compute_jobs.py',root,'compatibility/jobs/compute_jobs.py')
    for name in ('contracts/sql/ai-catalog-v1.json','contracts/sql/ai-actions-v1.md','contracts/sql/native-sql-v1.md','packaging/sdk-artifacts.json'):
        copy(SERVER/name,root,name)
    (root/'README.md').write_text('''# StarDataParadise Server 0.5.2\n\nStable Windows x64 portable release. Extract the entire archive. Microsoft Visual C++ x64 runtime is required. Preserve existing data and configuration; initialize only a new, empty directory.\n\n```powershell\n$base = (Get-Location).Path\n.\\bin\\mysqld.exe --no-defaults --console --initialize "--basedir=$base" "--datadir=$base\\data"\n.\\bin\\mysqld.exe --no-defaults --console "--basedir=$base" "--datadir=$base\\data" --bind-address=127.0.0.1 --port=3307\n```\n\nUse the initialization password printed by the server, then change it at first login. Configure TLS before remote access. Native server compatibility version remains 26.10.0-StarDataParadise; product release metadata is in release.json.\n\nAI/Knowledge setup, the internal queue, OpenAI/Claude credentials, optional GGUF/llama.cpp runtime and worker launch are documented in compatibility/ai/README.md. Install catalog.sql, tool_catalog.sql and action_catalog.sql in the stated maintenance profile before enabling the action profile. API keys, model weights and machine-specific worker settings are supplied by the user.\n\nThe optional SDK TLS bridge uses compatibility/wire_v1/service.py and the hash-pinned requirements.txt. Four SDK release versions and checksums are listed in packaging/sdk-artifacts.json.\n\nHIST/Git and the new public SQL JOB system are deferred. Existing legacy compute compatibility and the AI internal durable queue remain. This archive does not update Windows services or existing databases automatically.\n\nSource and validation: https://github.com/ParadiseIncDevelopers/StarDataParadiseServer/releases/tag/v0.5.2\n''',encoding='utf-8')
    finalize(root,stage/'StarDataParadise-Server-v0.5.2-windows-x64.zip')

def workbench(runtime,stage):
    root=stage/'workbench';root.mkdir()
    static={'data','extras','firewall','images','licenses','locales','modules','sdp.profiles','python','script_templates','share','snippets','structs','sys','workbench'}
    exclude={'.exp','.lib','.pdb','.ilk','.metagen','.pyc','.slnx','.log','.pem','.key','.pfx'}
    banned={'connections.xml','server_instances.xml','wb_options.xml','wb_state.xml'}
    if digest(runtime/'data/data.db')!=digest(WB/'classic-workbench/res/wbdata/data.db'):raise RuntimeError('Local Workbench state database differs from source resource')
    for path in sorted(runtime.rglob('*')):
        rel=path.relative_to(runtime)
        if not path.is_file() or path.suffix.lower() in exclude or path.name.lower() in banned:continue
        if len(rel.parts)>1 and rel.parts[0] not in static:continue
        if len(rel.parts)==1 and path.suffix.lower() not in {'.exe','.dll','.pyd','.py','.config','.ico','.vbs'} and path.name not in {'License.txt','shell_snippets.py.txt'}:continue
        if '__pycache__' in rel.parts or path.name.startswith(('WBTests','StarDataParadiseModelAcceptance')):continue
        copy(path,root,rel)
    for name in ('sdp_workspace_backend.py','sdp_workspace_grt.py','sdp_local_worker.py'):
        if digest(root/'modules'/name)!=digest(WB/'classic-workbench/modules/wb.utils'/name):raise RuntimeError('Stale Workbench module: '+name)
    for name in ('StarDataParadise.exe','wbprivate.be.dll','parsers.dll','db.sdp.parser.grt.dll','sdp.forms.dll','sdp.exe','sdpdump.exe','License.txt','data/main_menu.xml','python/lib/encodings/__init__.py','locales/tr-workspace.json'):
        if not (root/name).is_file():raise RuntimeError('Missing Workbench runtime '+name)
    aliases=json.loads((WB/'packaging/classic-workbench/artifact-names.json').read_text(encoding='utf-8'))
    legacy_names={name.casefold() for name in aliases['files']}
    legacy_prefixes=tuple(name.casefold()+'.' for name in aliases['libraries'])
    for path in root.rglob('*'):
        if path.is_file() and (path.name.casefold() in legacy_names or path.name.casefold().startswith(legacy_prefixes)):
            raise RuntimeError('Legacy Workbench artifact remains: '+str(path.relative_to(root)))
    copy(runtime/'README.md',root,'README-Upstream.md')
    copy(WB/'release.json',root,'release.json')
    copy(WB/'packaging/classic-workbench/SURUM_NOTLARI.md',root,'SURUM_NOTLARI.md')
    (root/'README-StarDataParadise.md').write_text('''# StarDataParadise Workbench 0.3.1\n\nStable Windows x64 portable release, Classic source base 8.0.47. Extract the entire archive and launch StarDataParadise.exe. Microsoft Visual C++ x64 runtime is required.\n\nConnect to a matching StarDataParadise Server 0.5.2 instance. AI/Knowledge pages and SQL completion follow the server's accepted native capabilities. The AI model page supports OpenAI, Claude/Anthropic and imported local GGUF models; provider credentials and worker configuration belong to the user. OpenAI/Claude quick setup, a local GGUF picker and the installed worker launcher are included. Keys stay in the Windows vault.\n\nKnowledge dependencies use a diagram and versions use a timeline. Conversation creation leads to an editable AI SQL execution screen. Permissions and action approval use SQL previews bound to the inspected action.\n\nHIST/Git and the new public SQL JOB screens are deferred. Existing user profiles are not included in this archive. See SURUM_NOTLARI.md and release.json.\n\nSource and validation: https://github.com/ParadiseIncDevelopers/StarDataParadise/releases/tag/v0.3.1\n''',encoding='utf-8')
    finalize(root,stage/'StarDataParadise-Workbench-v0.3.1-windows-x64.zip')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind',choices=('server','workbench'));parser.add_argument('--runtime',type=Path,required=True)
    parser.add_argument('--stage',type=Path,required=True);args=parser.parse_args()
    stage=args.stage.resolve();stage.mkdir(parents=True,exist_ok=True)
    {'server':server,'workbench':workbench}[args.kind](args.runtime.resolve(strict=True),stage)

if __name__=='__main__':main()
