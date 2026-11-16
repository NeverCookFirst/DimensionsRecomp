"""Run actual native readback/publication against SDK read-only physical maps."""
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

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
p.add_argument('--plume-lib',type=Path,required=True)
p.add_argument('--runtime-dir',type=Path,required=True)
p.add_argument('--previous-source',type=Path)
p.add_argument('--provider',action='store_true',help='Drive publication through real SDK guest faults using a prototype owner group')
p.add_argument('--generate-only',action='store_true',help='Generate fixture includes without compiling or running')
p.add_argument('--real-headers',action='store_true',help='Use actual header ABI and placement validation on protected pages')
p.add_argument('--header-source',type=Path,help='Placement validation source for a header-reentry negative control')
p.add_argument('--expect-header-reentry',action='store_true')
p.add_argument('--compiler',default='clang-cl')
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2];sdk=root.parent/'dev-assets/sdk-install'
text=(root/'rexlego/src/gpu_native/textures.cpp').read_text()
signatures=['TextureReadbackFormat ResolvedTextureReadbackFormat(','void RecordResolvedTexture(',
    'bool PrepareResolvedTextureWriteback(','bool EncodeResolvedTextureWriteback(',
    'void PublishResolvedTextureWriteback(','bool MaterializeResolvedTextures(']
def production(source):
    return body(source,'struct ResolvedTextureWriteback {')+';\n'+'\n'.join(body(source,s) for s in signatures)
prefix=Path(__file__).with_name('test_partial_destination_gpu.cpp').read_text().split('int main(){')[0]
prefix=prefix.replace('#include "gpu_native/resolve_region.h"',
    '#include "gpu_native/resolve_region.h"\n#include "gpu_native/physical_write_scope.h"')
start=prefix.index('struct Memory {');end=prefix.index('std::mutex g_textures_mutex;',start)
prefix=prefix[:start]+'''namespace fixture {rex::memory::Memory* sdk_memory=nullptr;}
struct Memory {std::span<u8> data;}memory;
#define REX_KERNEL_MEMORY() (fixture::sdk_memory)
'''+prefix[end:]
start=prefix.index('bool legodimensions::gpu_native::PoolCopyPhysicalRange(')
end=prefix.index('bool legodimensions::gpu_native::GuestRangeOverlapsPoolCopy(',start)
prefix=prefix[:start]+'''bool legodimensions::gpu_native::PoolCopyPhysicalRange(u32 at,u32 n,PhysicalCopyRange&r){
 if(!n||u64(at)+n>memory.data.size())return false;r={at,n};return true;
}
bool legodimensions::gpu_native::PoolCopyRangeReadable(PhysicalCopyRange r){return fixture::sdk_memory->GetPhysicalHeap()->IsRangeCommittedReadable(r.address,r.length);}
bool legodimensions::gpu_native::PoolCopyRangeWritable(PhysicalCopyRange r){return PoolCopyRangeReadable(r)&&fixture::sdk_memory->GetPhysicalHeap()->QueryRangeAccess(r.address,r.address+r.length-1)==rex::memory::PageAccess::kReadWrite;}
auto FakePhysicalWriteback(rex::memory::Memory*,std::span<const PhysicalCopyRange>){return std::make_unique<int>(1);}
'''+prefix[end:]
if a.real_headers:
    if not a.provider:raise SystemExit('--real-headers requires --provider')
    abi=(root/'rexlego/src/gpu_native/d3d.h').read_text()
    headers='\n'.join(body(abi,s)+';' for s in ['struct FetchConstant {','struct D3DResource {','struct D3DTexture {'])
    prefix=prefix.replace('#include "gpu_native/memory_watch.h"',
        '#include "gpu_native/memory_watch.h"\n#include "gpu_native/guest_metadata.h"\n#include <rex/types.h>')
    prefix=prefix.replace('struct D3DTexture {u8 data[52];};',headers+'\nstatic_assert(sizeof(D3DTexture)==52);')
    prefix=prefix.replace('struct TextureResource {','struct TextureResource {\n u32 adopted_header_type=3;',1)
    header_source=a.header_source.read_text() if a.header_source else text
    validation='template<class T>T* GuestAt(u32 address){return REX_KERNEL_MEMORY()->TranslateVirtual<T*>(address);}\n'
    validation+=body(header_source,'bool ReadableUploadSpan(')+'\n'+body(header_source,'bool TexturePlacementHeaderMatches(')
    prefix=prefix.replace('bool TexturePlacementHeaderMatches(const TextureResource&){return true;}',validation)
(out/'readonly-prefix.inc').write_text(prefix)
batch=Path(__file__).with_name('test_texture_batch_gpu.cpp').read_text()
(out/'make-image.inc').write_text(batch[batch.index('struct Image {'):batch.index('int main(){')])
(out/'tile-address.inc').write_text(body((root/'rexglue-sdk/src/graphics/pipeline/texture/util.cpp').read_text(),'int32_t GetTiledOffset2D('))
env=dict(os.environ,PATH=str(a.runtime_dir.resolve())+os.pathsep+os.environ['PATH'],
         LEGO_TEST_RUNTIME_EXPECTED=str(a.runtime_dir.resolve()/'rexruntime.dll'))
if a.generate_only:
    (out/'partial-destination-production.inc').write_text(production(text))
    raise SystemExit(0)

def run(name,code,cache='0'):
    (out/'partial-destination-production.inc').write_text(code)
    exe=out/(name+'.exe')
    r=subprocess.run([a.compiler,str(Path(__file__).with_suffix('.cpp')),'/std:c++latest','/O2','/MD','/EHsc']+
        (['/DLEGO_TEST_NATIVE_PROVIDER'] if a.provider else [])+
        (['/DLEGO_TEST_REAL_HEADERS'] if a.real_headers else [])+[
        '/I'+str(root/'rexlego/src'),'/I'+str(sdk/'include'),'/I'+str(root/'rexglue-sdk/include'),'/I'+str(root/'thirdparty/plume'),
        '/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),'/I'+str(out),
        '/Fo'+str(out/(name+'.obj')),'/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),
        str(sdk/'lib/rexruntime.lib'),'d3d12.lib','dxgi.lib','dxguid.lib','user32.lib'],capture_output=True,text=True,timeout=120)
    (out/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
    r=subprocess.run([str(exe)],env=dict(env,PLUME_D3D12_STATE_CACHE=cache),capture_output=True,text=True,timeout=90)
    (out/(name+'.log')).write_text(r.stdout+r.stderr);return r

code=production(text);positive=run('readonly-gpu',code)
if a.expect_header_reentry:
    assert a.real_headers and positive.returncode==23 and 'REENTERED_NATIVE_PROVIDER_DURING_HEADER_VALIDATION' in positive.stderr,(positive.returncode,positive.stdout,positive.stderr)
    (out/'verification.json').write_text(json.dumps(dict(game_launched=False,negative_header_reentry=True,
        exit_code=positive.returncode,stdout=positive.stdout,stderr=positive.stderr),indent=2)+'\n')
    print('PASS expected native provider reentry with previous placement-header implementation')
    raise SystemExit(0)
if positive.returncode:raise RuntimeError(positive.stdout+positive.stderr)
print(positive.stdout.strip())
cached=subprocess.run([str(out/'readonly-gpu.exe')],env=dict(env,PLUME_D3D12_STATE_CACHE='1'),capture_output=True,text=True,timeout=90)
(out/'state-cache-on.log').write_text(cached.stdout+cached.stderr);cached.check_returncode()
old=None
if a.previous_source and not a.provider:
    old=run('previous-readonly',production(a.previous_source.read_text()))
    assert old.returncode==23,(old.returncode,old.stdout,old.stderr)
    print('PASS negative control: previous publication rejects valid read-only GPU backing')
mutant=None
if not a.provider:
    mutant=run('missing-physical-window',code.replace('OpenPhysicalWriteback(REX_KERNEL_MEMORY(),ranges)','FakePhysicalWriteback(REX_KERNEL_MEMORY(),ranges)'))
    assert mutant.returncode==0xC0000005,(mutant.returncode,mutant.stdout,mutant.stderr)
    print('PASS negative control: omitting the physical write window faults during publication')
(out/'partial-destination-production.inc').write_text(code)
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
    production_source_sha256=hashlib.sha256(text.encode()).hexdigest(),actual_sdk_memory=True,
    actual_gpu_readback=True,actual_native_publication=True,state_cache_modes=[0,1],positive=positive.stdout,
    previous_exit=old.returncode if old else None,missing_scope_exit=mutant.returncode if mutant else None,
    prototype_fault_provider=a.provider,
    actual_header_abi_and_placement_validation=a.real_headers,
    runtime_sha256=hashlib.sha256((a.runtime_dir/'rexruntime.dll').read_bytes()).hexdigest(),
    scope='Real SDK physical/guest protections and raw GPU transfer; synthetic texture header/layout registry'),indent=2)+'\n')
