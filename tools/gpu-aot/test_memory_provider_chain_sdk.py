"""Validate supplemental provider dispatch/lifetime against a private SDK DLL."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path);p.add_argument('--runtime-dir',type=Path,required=True)
p.add_argument('--compiler',default='clang-cl');a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2];sdk=root.parent/'dev-assets/sdk-install'
exe=out/'provider-chain.exe'
r=subprocess.run([a.compiler,str(Path(__file__).with_suffix('.cpp')),'/std:c++latest','/O2','/MD','/EHsc',
    '/I'+str(sdk/'include'),'/I'+str(root/'rexglue-sdk/include'),'/I'+str(root/'rexlego/src'),
    '/Fo'+out.as_posix()+'/', '/Fe'+str(exe),'/link',str(sdk/'lib/rexruntime.lib'),'kernel32.lib','user32.lib'],
    capture_output=True,text=True,timeout=90)
(out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
runtime=a.runtime_dir.resolve();env=dict(os.environ,PATH=str(runtime)+os.pathsep+os.environ['PATH'],
    LEGO_TEST_RUNTIME_EXPECTED=str(runtime/'rexruntime.dll'))
r=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=40)
(out/'positive.log').write_text(r.stdout+r.stderr);r.check_returncode();print(r.stdout.strip())
failures={}
for mode in ['failed-provider','stuck-provider']:
    negative=subprocess.run([str(exe),mode],env=env,capture_output=True,text=True,timeout=20)
    (out/(mode+'.log')).write_text(negative.stdout+negative.stderr)
    assert negative.returncode not in (0,23) and 'UNSAFE STALE READ' not in negative.stdout,(negative.returncode,negative.stdout,negative.stderr)
    failures[mode]=negative.returncode
print('PASS failure boundary: an owned dependency failure cannot continue with stale CPU bytes')
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
    actual_sdk_memory=True,actual_faults=True,supplemental_provider_api=True,
    runtime_sha256=hashlib.sha256((runtime/'rexruntime.dll').read_bytes()).hexdigest(),
    positive=r.stdout,failed_provider_exits=failures,
    scope='Synthetic disjoint byte owners and callback timing; real SDK faults/protections, no GPU or native renderer registration'),indent=2)+'\n')
