"""Run provider fault/delivery cases against an explicitly selected local DLL."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
p.add_argument('--runtime-dir',type=Path,required=True)
p.add_argument('--baseline-diagnostic',action='store_true')
p.add_argument('--read-only-probe',action='store_true',help='Record the unresolved physical write-access failure separately')
p.add_argument('--physical-write-scope',action='store_true')
p.add_argument('--compiler',default='clang-cl')
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2];sdk=root.parent/'dev-assets/sdk-install'
runtime=a.runtime_dir.resolve();exe=out/'memory-provider-sdk.exe'
r=subprocess.run([a.compiler,str(Path(__file__).with_suffix('.cpp')),'/std:c++latest','/O2','/MD','/EHsc',
    '/I'+str(sdk/'include'),'/I'+str(root/'rexlego/src')]+(['/DLEGO_TEST_PHYSICAL_WRITE_SCOPE'] if a.physical_write_scope else [])+[
    '/Fo'+out.as_posix()+'/', '/Fe'+str(exe),'/link',str(sdk/'lib/rexruntime.lib'),
    'kernel32.lib','user32.lib'],capture_output=True,text=True,timeout=90)
(out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
assert not (a.baseline_diagnostic and a.read_only_probe)
r=subprocess.run([str(exe)]+(['readonly-probe'] if a.read_only_probe else ['diagnostic'] if a.baseline_diagnostic else []),
    env=dict(os.environ,PATH=str(runtime)+os.pathsep+os.environ['PATH'],
        LEGO_TEST_RUNTIME_EXPECTED=str(runtime/'rexruntime.dll')),capture_output=True,text=True,timeout=40)
(out/'results.log').write_text(r.stdout+r.stderr)
if a.read_only_probe and not a.physical_write_scope:
    assert r.returncode==0xC0000005, (r.returncode,r.stdout,r.stderr)
else:
    r.check_returncode()
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
    actual_sdk_memory=True,actual_protected_faults=True,baseline_diagnostic=a.baseline_diagnostic,
    read_only_probe=a.read_only_probe,physical_write_scope=a.physical_write_scope,exit_code=r.returncode,
    unresolved_expected_failure=a.read_only_probe and not a.physical_write_scope,
    runtime_sha256=hashlib.sha256((runtime/'rexruntime.dll').read_bytes()).hexdigest(),
    result=r.stdout,scope='CPU synthetic pending pages; no graphics processor, native provider integration or GPU byte-generation proof'),indent=2)+'\n')
print(r.stdout.strip())
