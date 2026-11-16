"""Run the native physical dependency protocol against real SDK providers/faults.

The production header executes. Producer completion and byte publication are
controlled CPU work; there is no native renderer registration, recorder or GPU.
Cross-page/chain selection runs against a sequential byte oracle and includes
disjoint producers that must remain pending until requested.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--runtime-dir',type=Path,required=True);p.add_argument('--previous-source',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];sdk=root.parent/'dev-assets/sdk-install';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
header=root/'rexlego/src/gpu_native/physical_dependency_provider.h';fixture=Path(__file__).with_suffix('.cpp')
for name in ['rexruntime.dll','amd_fidelityfx_dx12.dll']:shutil.copy2(a.runtime_dir/name,out/name)
records=[]
def build(label,text):
 (out/'provider-under-test.h').write_text(text);exe=out/(label+'.exe')
 r=subprocess.run(['clang-cl',str(fixture),'/std:c++latest','/O2','/MD','/EHsc','/D_CRT_SECURE_NO_WARNINGS',
  '/I'+str(out),'/I'+str(root/'rexlego/src'),'/I'+str(sdk/'include'),'/I'+str(root/'rexglue-sdk/include'),
  '/Fo'+str(out)+'/', '/Fe'+str(exe),'/link',str(sdk/'lib/rexruntime.lib'),'kernel32.lib','user32.lib'],capture_output=True,text=True,timeout=90)
 (out/(label+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode();return exe
def run(exe,label,mode='positive',expected=0):
 r=subprocess.run([str(exe),mode],cwd=out,env=dict(os.environ,LEGO_TEST_RUNTIME_EXPECTED=str(out/'rexruntime.dll')),capture_output=True,text=True,timeout=120 if mode=='cross-page-order' else 14)
 (out/(label+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==expected,(label,r.returncode,r.stdout,r.stderr)
 record=dict(label=label,exit=r.returncode,output=r.stdout,error=r.stderr);records.append(record);print(r.stdout+r.stderr,end='');return record
text=header.read_text();exe=build('production',text);run(exe,'positive');run(exe,'retry-stop','retry-stop');run(exe,'cross-page-order','cross-page-order')
for mode in ['failed','unsafe-metadata']:
 record=run(exe,mode,mode,23);assert 'EXPECTED_UNDELIVERED_DEPENDENCY_FATAL' in record['error'] and 'UNSAFE STALE READ' not in record['output']
marker='    return delivered;';assert text.count(marker)==1
mutant=text.replace(marker,'    if (delivered) for (const auto& ticket : batch) for (auto range : ticket->ranges) memory_.DisablePhysicalMemoryDataProviders(range.address, range.length);\n'+marker)
exe=build('early-release',mutant);run(exe,'early-release',expected=23)
held=text.replace('      critical.unlock();','      // deliberately hold SDK lock',1).replace('      critical.lock();','      // still held',1)
exe=build('held-critical',held);run(exe,'held-critical',expected=23)
retire='      retired_.insert(retired_.end(), batch.begin(), batch.end());';assert text.count(retire)==1
exe=build('inline-producer-destruction',text.replace(retire,''));run(exe,'inline-producer-destruction',expected=23)
marker='      if (!selected) {';assert text.count(marker)==1
exe=build('page-local-only',text.replace(marker,'      if (false) {'));run(exe,'page-local-only','cross-page-order',23)
previous=None
if a.previous_source:
 previous=dict(path=str(a.previous_source.resolve()),sha256=hashlib.sha256(a.previous_source.read_bytes()).hexdigest())
 exe=build('previous-cross-page',a.previous_source.read_text());run(exe,'previous-cross-page','cross-page-order',23)
(out/'provider-under-test.h').write_text(text)
sources=[header,Path(__file__),fixture,root/'rexlego/src/gpu_native/guest_metadata.h',root/'rexlego/src/gpu_native/physical_write_scope.h']
(out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,gpu_tested=False,actual_sdk_faults=True,
 records=records,sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},previous_source=previous,runtime_sha256=hashlib.sha256((out/'rexruntime.dll').read_bytes()).hexdigest(),
 scope=__doc__,limits=['Native draw/resolve capture publication, immutable GPU version ownership, internal CPU snapshot ordering, readback encoding, resource relocation/reuse and native device lifecycle remain to be integrated/verified.',
 'Stop requires the caller to quiesce any legacy-slot producer and to call outside renderer/SDK locks. It is not an app UI close callback.',
 'Synthetic producer byte ranges stand in for exact GPU coverage. No game visual or performance acceptance is proved.']),indent=2))
