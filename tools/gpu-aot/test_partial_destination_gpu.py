"""Exercise actual materialization/copy/invalidation bodies on a windowless GPU."""
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
a.output = a.output.resolve()
a.output.mkdir(parents=True, exist_ok=True)
textures = (root / 'rexlego/src/gpu_native/textures.cpp').read_text()
hooks = (root / 'rexlego/src/gpu_native/hooks_buffer.cpp').read_text()
production = '\n'.join(body(textures, s) for s in (
    'TextureReadbackFormat ResolvedTextureReadbackFormat(', 'void RecordResolvedTexture(',
    'std::array<PhysicalCopyRange, 2> TextureDataRanges(',
    'bool TextureDataOverlapsPoolCopy(', 'bool TextureDataCoveredByPoolCopy(',
    'bool PoolCopyGpuSourceIndexed(', 'bool PoolCopyGpuDestinationIndexed(',
    'bool PrepareResolvedTextureWriteback(', 'bool EncodeResolvedTextureWriteback(',
    'void PublishResolvedTextureWriteback(', 'bool MaterializeResolvedTextures(',
    'bool MaterializeResolvedTexture(const std::shared_ptr<TextureResource>& resource) {',
    'bool MaterializePoolCopyTextureDependencies(',
    'bool MaterializePoolCopyTextureSource(', 'bool MaterializePoolCopyTextureDestination(',
    'template<class Iterator>\nbool InvalidatePoolCopyTexture(',
    'void RetirePoolCopyTextures(', 'u32 InvalidatePoolCopyTexturesIndexed('))
production=body(textures,'struct ResolvedTextureWriteback {')+';\n'+production
production += '\n' + body(hooks, 'void MergeRanges(')
production += '\n' + body(hooks, 'void FlushPoolCopyBatch(').replace(
    'void FlushPoolCopyBatch(', 'void legodimensions::gpu_native::FlushPoolCopyBatch(', 1)
production += '\n' + body(hooks, 'void BeginPoolCopyBatch(')
production += '\n' + body(hooks, 'void PoolCopyRecorded(')
(a.output / 'tile-address.inc').write_text(body(
    (root / 'rexglue-sdk/src/graphics/pipeline/texture/util.cpp').read_text(), 'int32_t GetTiledOffset2D('))


def compile_run(name, code):
    (a.output / 'partial-destination-production.inc').write_text(code)
    exe = a.output / (name + '.exe')
    cmd = [a.compiler, str(Path(__file__).with_suffix('.cpp')), '/std:c++20', '/O2', '/MD', '/EHsc',
           '/I' + str(root / 'rexlego/src'), '/I' + str(root / 'thirdparty/plume'),
           '/I' + str(root / 'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
           '/I' + str(a.output), '/Fo' + str(a.output / (name + '.obj')), '/Fe' + str(exe), '/link',
           str(a.plume_lib.resolve()), 'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'user32.lib']
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    (a.output / (name + '-compile.log')).write_text(r.stdout + r.stderr)
    r.check_returncode()
    r = subprocess.run([str(exe)], env=dict(os.environ, PLUME_D3D12_STATE_CACHE='0'),
                       capture_output=True, text=True, timeout=60)
    (a.output / (name + '.log')).write_text(r.stdout + r.stderr)
    return r


positive = compile_run('partial-destination-gpu', production)
if positive.returncode:
    raise RuntimeError(positive.stdout + positive.stderr)
print(positive.stdout.strip())
cache_on = subprocess.run([str(a.output / 'partial-destination-gpu.exe')],
    env=dict(os.environ, PLUME_D3D12_STATE_CACHE='1'), capture_output=True, text=True, timeout=60)
(a.output / 'state-cache-on.log').write_text(cache_on.stdout + cache_on.stderr)
if cache_on.returncode:
    raise RuntimeError(cache_on.stdout + cache_on.stderr)
print('PASS with Plume state cache enabled')
negative = compile_run('lost-destination', production.replace('PoolCopyGpuDestinationIndexed(dst)', 'false'))
if negative.returncode != 23:
    raise RuntimeError('Omitting destination preservation escaped GPU untouched-pixel comparison')
scaled_mutant = production.replace('},resource->HostWidth(),resource->HostHeight(),&resource->writeback_coverage,texel_bytes);',
                                   '},resource->width,resource->height,&resource->writeback_coverage,texel_bytes);')
if scaled_mutant == production:
    raise RuntimeError('Scaled writeback mutation did not change production body')
scaled_negative = compile_run('lost-scale-selection', scaled_mutant)
if scaled_negative.returncode != 23:
    raise RuntimeError('Omitting source scale selection escaped the GPU comparison')
coverage_mutant = production.replace(',&resource->writeback_coverage,texel_bytes);', ',nullptr,texel_bytes);')
coverage_negative = compile_run('lost-resolve-coverage', coverage_mutant)
if coverage_negative.returncode != 23:
    raise RuntimeError('Publishing unwritten host texels escaped the GPU comparison')
merge_mutant = production.replace(
    'const auto merged=MergeTextureWriteback(std::span<u8>(guest,backing.length),job.staged,job.snapshot);',
    'std::memcpy(guest,job.staged.data(),job.staged.size()); const auto merged = TextureWritebackMergeResult{true,false};')
if merge_mutant == production:
    raise RuntimeError('Byte merge mutation did not change production body')
merge_negative = compile_run('lost-later-cpu-write', merge_mutant)
if merge_negative.returncode != 23:
    raise RuntimeError('Overwriting later CPU writes escaped the real GPU comparison')
width_mutant = production.replace('job.texel_bytes=TextureReadbackTexelBytes(job.format);',
                                   'job.texel_bytes=4;')
if width_mutant == production:
    raise RuntimeError('Raw texel-width mutation did not change production body')
width_negative = compile_run('lost-64bit-texels', width_mutant)
if width_negative.returncode != 23:
    raise RuntimeError('Truncating RGBA16 texels escaped the GPU comparison')
(a.output / 'partial-destination-production.inc').write_text(production)
(a.output / 'verification.json').write_text(json.dumps(dict(game_launched=False, window_opened=False,
    actual_gpu=True, production_materialization=True, production_copy=True, production_invalidation=True,
    cases=3840, readbacks=1920, raw_formats=['RGBA8','RG16','RGBA16'], numeric_packing=[0,1], numeric_swizzles=['688','B48'], state_cache_modes=[0, 1], scales=[1, 1.5, 2], later_cpu_write_during_gpu_wait=True, positive=positive.stdout.strip(), negative_exit=negative.returncode,
    scaled_negative_exit=scaled_negative.returncode,
    coverage_negative_exit=coverage_negative.returncode,
    merge_negative_exit=merge_negative.returncode,
    texel_width_negative_exit=width_negative.returncode,
    scope='Controlled RGBA8/RG16/RGBA16 textures at 1x/1.5x/2x; guest allocation/header queries stubbed'), indent=2))
print('PASS negative control: omitted partial preservation loses untouched GPU pixels')
print('PASS negative control: omitted source scale selection loses correct logical texels')
print('PASS negative control: publishing unwritten host texels corrupts untouched guest bytes')
print('PASS negative control: unconditional GPU publication loses later CPU writes')
print('PASS negative control: treating 64-bit RGBA16 as 32-bit texels corrupts guest bytes')
