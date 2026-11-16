"""Check production multi-texture dependency transactions on windowless D3D12."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def body(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--plume-lib', required=True, type=Path)
p.add_argument('--compiler', default='clang-cl')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
textures = (root / 'rexlego/src/gpu_native/textures.cpp').read_text()
hooks = (root / 'rexlego/src/gpu_native/hooks_buffer.cpp').read_text()
production = body(textures, 'struct ResolvedTextureWriteback {') + ';\n'
production += '\n'.join(body(textures, s) for s in (
    'TextureReadbackFormat ResolvedTextureReadbackFormat(', 'void RecordResolvedTexture(', 'std::array<PhysicalCopyRange, 2> TextureDataRanges(',
    'bool TextureDataOverlapsPoolCopy(', 'bool TextureDataCoveredByPoolCopy(',
    'bool PoolCopyGpuSourceIndexed(', 'bool PoolCopyGpuDestinationIndexed(',
    'bool PrepareResolvedTextureWriteback(', 'bool EncodeResolvedTextureWriteback(',
    'void PublishResolvedTextureWriteback(', 'bool MaterializeResolvedTextures(',
    'bool MaterializeResolvedTexture(const std::shared_ptr<TextureResource>& resource) {',
    'bool MaterializePoolCopyTextureDependencies(', 'bool MaterializePoolCopyTextureSource(',
    'bool MaterializePoolCopyTextureDestination(',
    'template<class Iterator>\nbool InvalidatePoolCopyTexture(',
    'void RetirePoolCopyTextures(', 'u32 InvalidatePoolCopyTexturesIndexed('))
production += '\n' + body(hooks, 'void MergeRanges(')
production += '\n' + body(hooks, 'void FlushPoolCopyBatch(').replace(
    'void FlushPoolCopyBatch(', 'void legodimensions::gpu_native::FlushPoolCopyBatch(', 1)
production += '\n' + body(hooks, 'void BeginPoolCopyBatch(') + '\n' + body(hooks, 'void PoolCopyRecorded(')
prefix = Path(__file__).with_name('test_partial_destination_gpu.cpp').read_text().split('int main(){')[0]
prefix = prefix.replace('Layout layout;', 'Layout layout;u32 packed_offset=0;')
prefix = prefix.replace('x=y=z=0;', 'x=packed_offset;y=z=0;')
prefix = prefix.replace('bool cpu_write_during_wait=false;',
    'std::function<void()> during_wait;bool wait_ok=true;u32 invalid_header=0;bool cpu_write_during_wait=false;')
prefix = prefix.replace('if(cpu_write_during_wait)memory.data[cpu_write_address]^=0x80;return true;',
    'if(cpu_write_during_wait)memory.data[cpu_write_address]^=0x80;if(during_wait)during_wait();return wait_ok;')
prefix = prefix.replace('bool TexturePlacementHeaderMatches(const TextureResource&){return true;}',
    'bool TexturePlacementHeaderMatches(const TextureResource& r){return r.guest_address!=invalid_header;}')
(out / 'batch-fixture-prefix.inc').write_text(prefix)
(out / 'tile-address.inc').write_text(body(
    (root / 'rexglue-sdk/src/graphics/pipeline/texture/util.cpp').read_text(), 'int32_t GetTiledOffset2D('))


def run(name, code):
    (out / 'partial-destination-production.inc').write_text(code)
    exe = out / (name + '.exe')
    cmd = [a.compiler, str(Path(__file__).with_suffix('.cpp')), '/std:c++20', '/O2', '/MD', '/EHsc',
           '/I' + str(root / 'rexlego/src'), '/I' + str(root / 'thirdparty/plume'),
           '/I' + str(root / 'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
           '/I' + str(out), '/Fo' + str(out / (name + '.obj')), '/Fe' + str(exe), '/link',
           str(a.plume_lib.resolve()), 'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'user32.lib']
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    (out / (name + '-compile.log')).write_text(r.stdout + r.stderr)
    r.check_returncode()
    r = subprocess.run([str(exe)], env=dict(os.environ, PLUME_D3D12_STATE_CACHE='0'),
                       capture_output=True, text=True, timeout=60)
    (out / (name + '.log')).write_text(r.stdout + r.stderr)
    return r


positive = run('texture-batch-gpu', production)
if positive.returncode:
    raise RuntimeError(positive.stdout + positive.stderr)
print(positive.stdout.strip())
on = subprocess.run([str(out / 'texture-batch-gpu.exe')], env=dict(os.environ, PLUME_D3D12_STATE_CACHE='1'),
                    capture_output=True, text=True, timeout=60)
(out / 'state-cache-on.log').write_text(on.stdout + on.stderr)
if on.returncode:
    raise RuntimeError(on.stdout + on.stderr)
encode = 'for(size_t i=0;i<jobs.size();++i) if(!EncodeResolvedTextureWriteback(jobs[i],pixels[i])) return false;'
publish = 'for(auto& job:jobs) PublishResolvedTextureWriteback(job);'
assert production.count(encode)==production.count(publish)==1
mutations = {
    'early-publication': production.replace(encode,'').replace(publish,
        'for(size_t i=0;i<jobs.size();++i){if(!EncodeResolvedTextureWriteback(jobs[i],pixels[i]))return false;PublishResolvedTextureWriteback(jobs[i]);}'),
    'lost-overlap-guard': production.replace('if(CopyRangesOverlap(ranges[i-1],ranges[i])) return false;', 'if(false) return false;'),
    'lost-header-recheck': production.replace('if (!TexturePlacementHeaderMatches(*resource) || !PoolCopyRangeReadable(backing)) return false;',
        'if (!PoolCopyRangeReadable(backing)) return false;'),
    'lost-cpu-merge': production.replace('const auto merged=MergeTextureWriteback(std::span<u8>(guest,backing.length),job.staged,job.snapshot);',
        'std::memcpy(guest,job.staged.data(),job.staged.size());const auto merged=TextureWritebackMergeResult{true,false};'),
    'lost-write-window': production.replace('if(!writable) return false;', ''),
}
negative = {}
for name, code in mutations.items():
    assert code!=production,name
    result = run(name,code)
    assert result.returncode == 23,(name,result.stdout,result.stderr)
    negative[name] = result.returncode
(out / 'partial-destination-production.inc').write_text(production)
(out / 'verification.json').write_text(json.dumps(dict(game_launched=False, window_opened=False,
    cases=391, state_cache_modes=[0, 1], actual_gpu=True, production_readback=True,
    shared_wait=True, duplicate_owners=True, mixed_formats=['RGBA8', 'RG16', 'RGBA16'],
    failure_atomic_publication=True, failed_wait_retains_all_buffers=True, negative_controls=negative,
    scope='Controlled disjoint allocations; layout/header queries stubbed; overlapping GPU ownership rejected'), indent=2))
print('PASS state-cache-on and overlap/header/CPU-merge negative controls')
