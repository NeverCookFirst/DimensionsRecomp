"""Reproduce shared-page legacy/supplemental release ordering with the delivered SDK.

No game, window or GPU. Controlled writers use the actual SDK providers,
physical write scope and faulted A/C aliases. The naive early release and
deferred-release control differ by one DisablePhysicalMemoryDataProviders call.
This tests a provider protocol, not a registered native renderer implementation.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--runtime-dir',required=True,type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];sdk=root.parent/'dev-assets/sdk-install';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
fixture=Path(__file__).with_suffix('.cpp');exe=out/'release-order.exe'
for name in ['rexruntime.dll','amd_fidelityfx_dx12.dll']:shutil.copy2(a.runtime_dir/name,out/name)
r=subprocess.run(['clang-cl',str(fixture),'/std:c++latest','/O2','/MD','/EHsc','/D_CRT_SECURE_NO_WARNINGS',
 '/I'+str(root/'rexlego/src'),'/I'+str(sdk/'include'),'/I'+str(root/'rexglue-sdk/include'),'/Fo'+str(out)+'/', '/Fe'+str(exe),
 '/link',str(sdk/'lib/rexruntime.lib'),'kernel32.lib','user32.lib'],capture_output=True,text=True,timeout=90)
(out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
records=[]
for deferred in [False,True]:
 label='deferred' if deferred else 'early-release'
 r=subprocess.run([str(exe),str(int(deferred))],cwd=out,env=dict(os.environ,LEGO_TEST_RUNTIME_EXPECTED=str(out/'rexruntime.dll')),capture_output=True,text=True,timeout=12)
 (out/(label+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,(label,r.returncode,r.stdout,r.stderr)
 assert ('PASS deferred' if deferred else 'REPRODUCED early') in r.stdout,r.stdout
 records.append(dict(label=label,exit=r.returncode,output=r.stdout));print(r.stdout,end='')
sources=[Path(__file__),fixture,root/'rexlego/src/gpu_native/physical_write_scope.h',root/'rexglue-sdk/src/system/xmemory.cpp',root/'rexglue-sdk/src/system/memory_data_provider.cpp']
(out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,gpu_tested=False,actual_sdk_faults=True,
 records=records,sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},runtime_sha256=hashlib.sha256((out/'rexruntime.dll').read_bytes()).hexdigest(),
 finding='A supplemental callback cannot safely disable a shared page before the legacy callback runs if a legacy write was armed during its unlocked GPU/recorder wait. Deferring that release preserves the pending dependency.',
 limits=['Controlled payloads and synchronization replace GPU/recorder work. No native renderer provider is registered and no game visual result is proved.',
 'Native provider design must also handle explicit ProvidePhysicalMemory and shutdown retirement; this fault-path control alone does not prove those lifecycles.']),indent=2))
