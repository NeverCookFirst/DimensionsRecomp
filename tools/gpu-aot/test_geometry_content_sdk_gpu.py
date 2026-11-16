"""Prove alias invalidation through actual SDK memory, native cache and GPU tapes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

def body(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

def replace(text,old,new):
    assert text.count(old)==1,old
    return text.replace(old,new,1)

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path);p.add_argument('--plume-lib',type=Path,required=True)
p.add_argument('--runtime-dir',type=Path,required=True);p.add_argument('--compiler',default='clang-cl')
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2];package=root.parent;sdk=package/'dev-assets/sdk-install'
source=(root/'rexlego/src/gpu_native/buffers.cpp').read_text()
(out/'geometry-resource.inc').write_text(source[source.index('struct BufferResource {'):source.index('std::mutex g_buffers_mutex;')])
(out/'geometry-production.inc').write_text('\n'.join(body(source,signature) for signature in [
    'void ByteSwapElements(','std::shared_ptr<plume::RenderBuffer> CreateImmutableUploadBuffer(',
    'std::shared_ptr<GeometryPublication> PromoteImmutableGeometry(',
    'plume::RenderBuffer* ResolveBufferContents(']))
code=Path(__file__).with_name('test_geometry_content_gpu.cpp').read_text()
code=replace(code,'using be_u32=uint32_t;','')
code=replace(code,'#include <immintrin.h>', '#include <immintrin.h>\n#include <rex/system/xmemory.h>')
start=code.index('struct Memory {');end=code.index('struct BufferUploadTiming',start)
code=code[:start]+'''namespace fixture {rex::memory::Memory* memory=nullptr;}
struct Memory {std::span<u8> bytes;} memory;
#define REX_KERNEL_MEMORY() (fixture::memory)
'''+code[end:]
start=code.index('std::array<u64,16> versions{};');end=code.index('void TraceBufferAllocation',start)
code=code[:start]+code[end:]
code=replace(code,'r->mirror_address=address;', 'r->mirror_address=0xA2000000u+address;')
code=replace(code,'int main(){', '''int main(){
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));
 CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 rex::memory::Memory sdk_memory;CHECK(sdk_memory.Initialize());fixture::memory=&sdk_memory;
 auto* heap=sdk_memory.LookupHeap(0xA2000000);CHECK(heap);
 CHECK(heap->AllocFixed(0xA2000000,65536,65536,
   rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
   rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 memory.bytes={sdk_memory.TranslatePhysical<u8*>(0x02000000),65536};
''')
code=replace(code,'memory.bytes[4096+7]^=0x80;++versions[1];',
    '*sdk_memory.TranslateVirtual<volatile u8*>(0xC2000000+4096+7)^=0x80;')
code=replace(code,'g_geometry_content_cache.Reset();g_upload_pool.Reset();g_resident_pool.Reset();',
    'g_geometry_content_cache.Reset();g_upload_pool.Reset();g_resident_pool.Reset();ShutdownCpuMemoryWatch();')
code=replace(code,'PASS %u GPU geometry readbacks:', 'PASS %u real-SDK alias-to-GPU geometry readbacks:')
(out/'geometry-sdk.cpp').write_text(code)
include=out/'include/rex/system';include.mkdir(parents=True,exist_ok=True)
(include.parent/'cvar.h').write_text('''#pragma once
namespace rex::cvar {enum class Lifecycle{kRequiresRestart};
struct FixtureCvar{FixtureCvar lifecycle(Lifecycle){return *this;}};}
#define REXCVAR_DEFINE_BOOL(name,...) static auto fixture_##name=rex::cvar::FixtureCvar{}
#define REXCVAR_GET(...) true
''')
(include/'kernel_state.h').write_text('''#pragma once
#include <rex/system/xmemory.h>
namespace fixture {extern rex::memory::Memory* memory;}
#define REX_KERNEL_MEMORY() (fixture::memory)
''')
exe=out/'geometry-sdk.exe'
cmd=[a.compiler,str(out/'geometry-sdk.cpp'),str(root/'rexlego/src/gpu_native/memory_watch.cpp'),
    str(root/'rexlego/src/gpu_native/native_recording.cpp'),'/std:c++latest','/O2','/MD','/EHsc','/UNDEBUG',
    '/I'+str(out/'include'),'/I'+str(out),'/I'+str(root/'rexlego/src'),'/I'+str(sdk/'include'),
    '/I'+str(root/'thirdparty/plume'),'/I'+str(root/'rexglue-sdk/thirdparty/xxHash'),
    '/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
    '/Fo'+str(out)+'/', '/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),str(sdk/'lib/rexruntime.lib'),
    'd3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
r=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
(out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
records=[]
for runtime,negative in [(a.runtime_dir.resolve(),False),((package/'build/native').resolve(),True)]:
    for cache in ['0','1'] if not negative else ['1']:
        env=dict(os.environ,PATH=str(runtime)+os.pathsep+os.environ['PATH'],
            LEGO_TEST_RUNTIME_EXPECTED=str(runtime/'rexruntime.dll'),
            PLUME_D3D12_STATE_CACHE=cache,LEGO_NATIVE_GEOMETRY_REUSE='1',
            LEGO_NATIVE_LOCAL_GEOMETRY='1',LEGO_NATIVE_BUFFER_WATCH='1',LEGO_NATIVE_STATIC_TEXTURE_WATCH='1')
        env.pop('LEGO_NATIVE_NO_MEMORY_WATCH',None);env.pop('LEGO_NATIVE_AUDIT_BUFFER_WATCH',None)
        r=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=60)
        (out/(('baseline' if negative else 'fixed-cache-'+cache)+'.log')).write_text(r.stdout+r.stderr)
        assert r.returncode==(23 if negative else 0),(r.returncode,r.stdout,r.stderr)
        records.append(dict(runtime=str(runtime),runtime_sha256=hashlib.sha256((runtime/'rexruntime.dll').read_bytes()).hexdigest(),
            cache=cache,negative=negative,exit=r.returncode,stdout=r.stdout.strip(),stderr=r.stderr.strip()))
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
    actual_sdk_memory=True,actual_production_watch=True,actual_production_resolve_and_promotion=True,
    actual_native_tapes=True,actual_gpu=True,records=records,
    scope='real C-alias CPU write invalidation through native geometry cache to GPU, controlled draw view and retirement'),indent=2)+'\n')
for record in records:print(('EXPECTED BASELINE FAILURE ' if record['negative'] else '')+record['stdout']+record['stderr'])
