"""Check production memory watcher with the shipped SDK's real protected maps."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang-cl')
p.add_argument('--runtime-dir',type=Path)
p.add_argument('--require-all-aliases',action='store_true')
p.add_argument('--permission-transitions',action='store_true')
a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2];package=root.parent
sdk=package/'dev-assets/sdk-install';native=(a.runtime_dir or package/'build/native').resolve()
include=out/'include/rex/system';include.mkdir(parents=True,exist_ok=True)
(include.parent/'cvar.h').write_text('''#pragma once
namespace rex::cvar {
enum class Lifecycle{kRequiresRestart};
struct FixtureCvar{FixtureCvar lifecycle(Lifecycle){return *this;}};
}
#define REXCVAR_DEFINE_BOOL(name,...) static auto fixture_##name=rex::cvar::FixtureCvar{}
#define REXCVAR_GET(...) true
''')
(include/'kernel_state.h').write_text('''#pragma once
#include <rex/system/xmemory.h>
namespace fixture {extern rex::memory::Memory* memory;}
#define REX_KERNEL_MEMORY() (fixture::memory)
''')
exe=out/'memory-watch-sdk.exe'
source=root/'rexlego/src/gpu_native/memory_watch.cpp'
cmd=[a.compiler,str(Path(__file__).with_suffix('.cpp')),str(source),'/std:c++latest','/O2','/MD','/EHsc',
    '/I'+str(out/'include'),'/I'+str(root/'rexlego/src'),'/I'+str(sdk/'include'),
    '/Fo'+str(out)+'/', '/Fe'+str(exe),'/link',str(sdk/'lib/rexruntime.lib'),
    'kernel32.lib','user32.lib']
r=subprocess.run(cmd,capture_output=True,text=True,timeout=90)
(out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
records=[]
for regions in ['0','1']:
    env=dict(os.environ,PATH=str(native)+os.pathsep+os.environ['PATH'],
        LEGO_TEST_RUNTIME_EXPECTED=str(native/'rexruntime.dll'),LEGO_NATIVE_WATCH_REGIONS=regions,
        LEGO_NATIVE_STATIC_TEXTURE_WATCH='1',LEGO_NATIVE_BUFFER_WATCH='1',
        LEGO_TEST_REQUIRE_ALL_ALIASES='1' if a.require_all_aliases else '0')
    if a.permission_transitions:env['LEGO_TEST_PERMISSION_TRANSITIONS']='1'
    else:env.pop('LEGO_TEST_PERMISSION_TRANSITIONS',None)
    env.pop('LEGO_NATIVE_NO_MEMORY_WATCH',None)
    r=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=30)
    (out/('regions-'+regions+'.log')).write_text(r.stdout+r.stderr)
    r.check_returncode();records.append(dict(regions=regions,exit=r.returncode,stdout=r.stdout.strip(),
        unallocated_alias_notified='UNALLOCATED_ALIAS_NOTIFIED=1' in r.stdout))
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
    actual_production_watcher=True,real_sdk_memory=True,real_protected_fault_callbacks=True,
    runtime_sha256=hashlib.sha256((native/'rexruntime.dll').read_bytes()).hexdigest(),
    production_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),records=records,
    unallocated_alias_visibility_proven=all(record['unallocated_alias_notified'] for record in records),
    all_aliases_required=a.require_all_aliases,
    protection_transitions_tested=a.permission_transitions,
    scope='real protected aliases and metadata/protection guards; fixture CVars/kernel accessor'),indent=2)+'\n')
for record in records:print(record['stdout'])
