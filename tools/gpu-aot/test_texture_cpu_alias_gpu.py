"""Exercise physical-allocation CPU locks with actual native readback on D3D12."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

def body(text, signature):
    start = text.index(signature)
    end = text.index('{', start)+1
    depth = 1
    while depth:
        depth += (text[end] == '{')-(text[end] == '}')
        end += 1
    return text[start:end]

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--plume-lib', type=Path, required=True)
p.add_argument('--previous-source', type=Path)
p.add_argument('--expect-old-failure', action='store_true')
p.add_argument('--compiler', default='clang-cl')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
text = (root/'rexlego/src/gpu_native/textures.cpp').read_text()
signatures = ['TextureReadbackFormat ResolvedTextureReadbackFormat(', 'void RecordResolvedTexture(',
    'std::array<PhysicalCopyRange, 2> TextureDataRanges(', 'bool TextureDataOverlapsPoolCopy(',
    'bool PrepareResolvedTextureWriteback(', 'bool EncodeResolvedTextureWriteback(',
    'void PublishResolvedTextureWriteback(', 'bool MaterializeResolvedTextures(',
    'bool MaterializeResolvedTexture(const std::shared_ptr<TextureResource>& resource) {',
    'bool MaterializeTextureForCpuRead(']
production = body(text,'struct ResolvedTextureWriteback {')+';\n'+'\n'.join(body(text,s) for s in signatures)
prefix = Path(__file__).with_name('test_partial_destination_gpu.cpp').read_text().split('int main(){')[0]
prefix = prefix.replace('bool cpu_write_during_wait=false;',
    'std::function<void()> during_wait;bool wait_ok=true;u32 invalid_header=0;bool cpu_write_during_wait=false;')
prefix = prefix.replace('if(cpu_write_during_wait)memory.data[cpu_write_address]^=0x80;return true;',
    'if(cpu_write_during_wait)memory.data[cpu_write_address]^=0x80;if(during_wait)during_wait();return wait_ok;')
prefix = prefix.replace('bool TexturePlacementHeaderMatches(const TextureResource&){return true;}',
    'bool TexturePlacementHeaderMatches(const TextureResource& r){return r.guest_address!=invalid_header;}')
prefix = prefix.replace('// Full production coverage,',
    'auto FindTexture(u32 address){const auto it=g_textures.find(address);return it==g_textures.end()?std::shared_ptr<TextureResource>{}:it->second;}\n// Full production coverage,')
(out/'cpu-alias-prefix.inc').write_text(prefix)
batch = Path(__file__).with_name('test_texture_batch_gpu.cpp').read_text()
(out/'make-image.inc').write_text(batch[batch.index('struct Image {'):batch.index('int main(){')])
(out/'tile-address.inc').write_text(body((root/'rexglue-sdk/src/graphics/pipeline/texture/util.cpp').read_text(),
                                       'int32_t GetTiledOffset2D('))

def run(name, code, mode=0):
    (out/'partial-destination-production.inc').write_text(code)
    exe = out/(name+'.exe')
    result = subprocess.run([a.compiler,str(Path(__file__).with_suffix('.cpp')),'/std:c++20','/O2','/MD','/EHsc',
        '/I'+str(root/'rexlego/src'),'/I'+str(root/'thirdparty/plume'),
        '/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),'/I'+str(out),
        '/Fo'+str(out/(name+'.obj')),'/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),
        'd3d12.lib','dxgi.lib','dxguid.lib','user32.lib'],capture_output=True,text=True,timeout=120)
    (out/(name+'-compile.log')).write_text(result.stdout+result.stderr)
    result.check_returncode()
    result = subprocess.run([str(exe)],env=dict(os.environ,PLUME_D3D12_STATE_CACHE=str(mode)),
        capture_output=True,text=True,timeout=90)
    (out/(name+'.log')).write_text(result.stdout+result.stderr)
    return result

result = run('cpu-alias-gpu', production)
if a.expect_old_failure:
    assert result.returncode == 23, (result.stdout,result.stderr)
    print('PASS reproduction: current CPU-lock path fails the physical-alias GPU regression')
    raise SystemExit(0)
if result.returncode:
    raise RuntimeError(result.stdout+result.stderr)
print(result.stdout.strip())
cache = subprocess.run([str(out/'cpu-alias-gpu.exe')],env=dict(os.environ,PLUME_D3D12_STATE_CACHE='1'),
    capture_output=True,text=True,timeout=90)
(out/'state-cache-on.log').write_text(cache.stdout+cache.stderr)
cache.check_returncode()
previous = None
if a.previous_source:
    old_body = body(a.previous_source.read_text(),'bool MaterializeTextureForCpuRead(')
    negative = run('old-cpu-lock',production.replace(body(text,'bool MaterializeTextureForCpuRead('),old_body))
    assert negative.returncode == 23, (negative.stdout,negative.stderr)
    previous = dict(exit_code=negative.returncode,output=negative.stdout+negative.stderr)
    print('PASS negative control: old CPU lock leaves GPU-owned alias backing stale')
(out/'partial-destination-production.inc').write_text(production)
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
    production_source_sha256=hashlib.sha256(text.encode()).hexdigest(),
    state_cache_modes=[0,1],positive=result.stdout,previous=previous,
    scope='Actual materialization and raw GPU copies; synthetic placement/header queries; no game or performance acceptance'),indent=2)+'\n')
