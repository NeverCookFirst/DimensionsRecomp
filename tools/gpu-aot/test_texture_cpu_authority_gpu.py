"""Real SDK writes invalidate published GPU texture fast paths, without a game."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

def body(text, signature):
    start=text.index(signature); end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('output',type=Path)
parser.add_argument('--plume-lib',type=Path,required=True)
parser.add_argument('--runtime-dir',type=Path,required=True)
parser.add_argument('--previous-source',type=Path,required=True)
parser.add_argument('--compiler',default='clang-cl')
parser.add_argument('--readonly-only',action='store_true',help='Check real write-watch arming across a read-only GPU publication')
a=parser.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2];sdk=root.parent/'dev-assets/sdk-install'
subprocess.run([sys.executable,str(Path(__file__).with_name('test_texture_readonly_sdk_gpu.py')),str(out),
    '--plume-lib',str(a.plume_lib),'--runtime-dir',str(a.runtime_dir),'--generate-only'],check=True)
prefix=(out/'readonly-prefix.inc').read_text()
prefix=prefix.replace('struct TextureResource {','struct TextureResource {\n u32 adopted_header_type=3,guest_format=6;',1)
prefix=prefix.replace(' static int LockRecording(){return 0;}',
    ' static auto LockRecording(){static std::recursive_mutex gate;return std::unique_lock(gate);}')
prefix=prefix.replace('static auto BeginFrameCommands(){CHECK(open);return commands.get();}',
    'static auto BeginFrameCommands(){if(!open){commands->begin();open=true;}return commands.get();}')
(out/'authority-prefix.inc').write_text(prefix)
text=(root/'rexlego/src/gpu_native/textures.cpp').read_text()
publication=(out/'partial-destination-production.inc').read_text()
helper=body(text,'void RefreshPublishedTextureAuthority(')
upload=body(text,'bool UploadTextureResourceStorage(')
upload=upload[:upload.index('  if (resource && resource->guest_format == 22')]
upload=upload.replace('bool UploadTextureResourceStorage(','bool ProductionUploadEntry(',1)+'  return false;\n}'
cached=body(text,'TextureResourceView PrepareCachedTexture(')
resolve=body(text,'TextureResourceView ResolveTextureResource(')
def write_code(pub=publication,entry=upload,cache=cached,direct=resolve):
    (out/'partial-destination-production.inc').write_text(pub)
    (out/'authority-production.inc').write_text(helper+'\n'+entry+'\n'+cache+'\n'+direct)

include=out/'include/rex/system';include.mkdir(parents=True,exist_ok=True)
(include.parent/'cvar.h').write_text('''#pragma once
namespace rex::cvar {enum class Lifecycle{kRequiresRestart};struct FixtureCvar{FixtureCvar lifecycle(Lifecycle){return *this;}};}
#define REXCVAR_DEFINE_BOOL(name,...) static auto fixture_##name=rex::cvar::FixtureCvar{}
#define REXCVAR_GET(...) true
''')
(include/'kernel_state.h').write_text('''#pragma once
#include <rex/system/xmemory.h>
namespace fixture {extern rex::memory::Memory* sdk_memory;}
#define REX_KERNEL_MEMORY() (fixture::sdk_memory)
''')

def compile(name, source=None):
    exe=out/(name+'.exe')
    p=subprocess.run([a.compiler,str(source or Path(__file__).with_suffix('.cpp')),str(root/'rexlego/src/gpu_native/memory_watch.cpp'),
        '/DLEGO_TEST_REAL_MEMORY_WATCH','/DNOMINMAX','/D_CRT_SECURE_NO_WARNINGS','/std:c++latest','/O2','/MD','/EHsc',
        '/I'+str(out/'include'),'/I'+str(out),'/I'+str(root/'rexlego/src'),'/I'+str(sdk/'include'),
        '/I'+str(root/'thirdparty/plume'),'/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
        '/Fo'+str(out)+'/', '/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),str(sdk/'lib/rexruntime.lib'),
        'd3d12.lib','dxgi.lib','dxguid.lib','user32.lib'],capture_output=True,text=True,timeout=120)
    (out/(name+'-compile.log')).write_text(p.stdout+p.stderr)
    if p.returncode:raise RuntimeError(p.stdout+p.stderr)
    return exe

records=[]
def run(exe,name,cache='0',regions='1',watch=True,expect=0,combined=False):
    env=dict(os.environ,PATH=str(a.runtime_dir.resolve())+os.pathsep+os.environ['PATH'],
        LEGO_TEST_RUNTIME_EXPECTED=str(a.runtime_dir.resolve()/'rexruntime.dll'),
        LEGO_NATIVE_STATIC_TEXTURE_WATCH='1' if watch else '0',LEGO_NATIVE_BUFFER_WATCH='1' if watch else '0',
        PLUME_D3D12_STATE_CACHE=cache,LEGO_NATIVE_WATCH_REGIONS=regions)
    env.pop('LEGO_NATIVE_NO_MEMORY_WATCH',None);env.pop('LEGO_NATIVE_AUDIT_TEXTURE_WATCH',None)
    env.pop('LEGO_TEST_WRITECOMBINE',None)
    if combined:env['LEGO_TEST_WRITECOMBINE']='1'
    p=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=90)
    (out/(name+'.log')).write_text(p.stdout+p.stderr)
    if p.returncode!=expect:raise RuntimeError(f'{name}: exit={p.returncode}\n{p.stdout}\n{p.stderr}')
    records.append(dict(name=name,cache=cache,regions=regions,watch=watch,expected_exit=expect,exit=p.returncode,stdout=p.stdout,stderr=p.stderr))
    print(name+': '+p.stdout.strip(),flush=True)

if a.readonly_only:
    write_code();(out/'readonly-authority.inc').write_text(helper)
    readonly_exe=compile('readonly-authority',Path(__file__).with_name('test_texture_cpu_authority_readonly_gpu.cpp'))
    run(readonly_exe,'readonly-authority')
    run(readonly_exe,'readonly-writecombine-authority',combined=True)
    (out/'verification.json').write_text(json.dumps(dict(game_launched=False,actual_sdk_memory=True,
        actual_production_watcher=True,actual_gpu_publication=True,actual_cpu_authority_helper=True,
        records=records,production_source_sha256=hashlib.sha256(text.encode()).hexdigest(),
        scope='One real SDK/GPU RO publication, then RO-to-RW protection change and CPU write with a clean disjoint neighbor'),indent=2)+'\n')
    raise SystemExit(0)

write_code();exe=compile('cpu-authority')
for cache in ['0','1']:
    for regions in ['0','1']:run(exe,f'cache-{cache}-regions-{regions}',cache,regions)
    run(exe,f'cache-{cache}-watch-disabled',cache,watch=False)
negative={
    'missing-upload-check': dict(entry=upload.replace('RefreshPublishedTextureAuthority(*resource);','')),
    'missing-cached-check': dict(cache=cached.replace('RefreshPublishedTextureAuthority(*resource);','')),
    'old-direct-view': dict(direct=body(a.previous_source.read_text(),'TextureResourceView ResolveTextureResource(')),
    'missing-published-stamp': dict(pub=publication.replace('resource->cpu_stamp=std::move(job.cpu_stamp)','resource->cpu_stamp={}')),
}
for name,change in negative.items():
    write_code(**change);run(compile(name),name,expect=23)
write_code()
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
    actual_sdk_faults=True,actual_production_watcher=True,actual_native_publication=True,
    actual_upload_entry_and_cached_and_direct_view_bodies=True,actual_gpu_reupload_and_readback=True,
    cases_per_run=576,positive_runs=6,records=records,
    runtime_sha256=hashlib.sha256((a.runtime_dir/'rexruntime.dll').read_bytes()).hexdigest(),
    production_source_sha256=hashlib.sha256(text.encode()).hexdigest(),
    scope='Controlled header/layout/registry and upload-storage driver; real production fast-path decisions, checked block copier, SDK A/C/E faults and GPU transfers. Not full game/worker/native fault-provider integration.'),indent=2)+'\n')
