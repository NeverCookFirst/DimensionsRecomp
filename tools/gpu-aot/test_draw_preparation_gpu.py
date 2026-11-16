"""Compare retained and legacy constants through real D3D12 readback."""
import argparse
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--plume-lib', type=Path, required=True)
parser.add_argument('--dxc', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
shaders = {
    'vs': 'cbuffer V:register(b0){float4 v[74];} void main(uint id:SV_VertexID,out float4 p:SV_Position,out float c:TEXCOORD0){p=float4(id==1?3:-1,id==2?3:-1,0,1);c=v[73].x;}',
    'ps': 'cbuffer P:register(b1){float4 p[192];} cbuffer S:register(b2){float s;} float4 main(float4 position:SV_Position,float c:TEXCOORD0):SV_Target{return float4(c,p[191].x,s,1);}',
}
for stage, text in shaders.items():
    source = out / (stage + '.hlsl')
    source.write_text(text)
    subprocess.run([str(args.dxc), str(source), '-T', stage + '_6_0', '-E', 'main', '-Fo', str(out / (stage + '.dxil'))], check=True, timeout=30)
exe = out / 'draw-preparation.exe'
command = ['clang-cl', str(Path(__file__).with_suffix('.cpp')), str(root / 'rexlego/src/gpu_native/native_recording.cpp'), '/std:c++20', '/O2', '/MD', '/EHsc', '/UNDEBUG',
           '/I' + str(root / 'rexlego/src'), '/I' + str(root / 'thirdparty/plume'),
           '/I' + str(root / 'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
           '/Fo' + str(out) + '/', '/Fe' + str(exe), '/link', str(args.plume_lib.resolve()),
           'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'user32.lib']
compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
(out / 'compile.log').write_text(compiled.stdout + compiled.stderr)
compiled.check_returncode()
records = []
for cache, prepare, mutant in [('0', '0', False), ('1', '0', False), ('1', '0', True), ('0', '1', False), ('1', '1', False), ('1', '1', True), ('0', '3', False), ('1', '3', False), ('1', '3', True)]:
    result = subprocess.run([str(exe), str(out / 'vs.dxil'), str(out / 'ps.dxil'), *(['stale-serial'] if mutant else [])],
                            env=dict(os.environ, PLUME_D3D12_STATE_CACHE=cache, LEGO_NATIVE_PREPARE_THREADS=prepare), capture_output=True, text=True, timeout=60)
    (out / f'cache-{cache}-prepare-{prepare}-mutant-{int(mutant)}.log').write_text(result.stdout + result.stderr)
    assert result.returncode == (23 if mutant else 0), result.stdout + result.stderr
    if not mutant:
        assert 'PASS 256 GPU readbacks' in result.stdout
    records.append(dict(cache=cache, prepare=prepare, stale_serial=mutant, exit=result.returncode, stdout=result.stdout, stderr=result.stderr))
for cache in ('0', '1'):
    result = subprocess.run([str(exe), str(out / 'vs.dxil'), str(out / 'ps.dxil')],
        env=dict(os.environ, PLUME_D3D12_STATE_CACHE=cache, LEGO_NATIVE_PREPARE_THREADS='3', LEGO_NATIVE_FULL_DRAW_PREPARATION='1'),
        capture_output=True, text=True, timeout=60)
    (out / f'detached-cache-{cache}.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0 and 'PASS 256 GPU readbacks' in result.stdout, result.stdout + result.stderr
    records.append(dict(cache=cache, prepare='3', detached=True, exit=result.returncode, stdout=result.stdout))
for retained_blend in ('0', '1'):
    result = subprocess.run([str(exe), str(out / 'vs.dxil'), str(out / 'ps.dxil')],
        env=dict(os.environ, PLUME_D3D12_STATE_CACHE='1', LEGO_NATIVE_PREPARE_THREADS='3',
                 LEGO_NATIVE_FULL_DRAW_PREPARATION='1', LEGO_TEST_BLEND_FACTORS='1',
                 LEGO_NATIVE_RETAIN_BLEND_STATE=retained_blend, LEGO_NATIVE_SKIP_EMPTY_COMMAND_TAPES=retained_blend),
        capture_output=True, text=True, timeout=60)
    (out / f'blend-retained-{retained_blend}.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0 and 'PASS 256 GPU readbacks' in result.stdout, result.stdout + result.stderr
    records.append(dict(blend_retained=retained_blend, detached=True, exit=result.returncode, stdout=result.stdout))
(out / 'verification.json').write_text(json.dumps(dict(passed=True, real_d3d12=True, records=records,
    scope='constant preparation and D3D12 binding/lifetime readback; no gameplay FPS claim'), indent=2))
print('PASS 2560 GPU readbacks: direct/worker/detached preparation, changing blend factors, external invalidation, empty tapes and stale-recording negative controls')
