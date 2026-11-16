"""Read back device-local geometry through actual Plume vertex and index draws."""
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
helper = (root / 'rexlego/src/gpu_native/geometry_upload.h').read_text()
fixture_header = out / 'gpu_native/geometry_upload.h'
fixture_header.parent.mkdir(exist_ok=True)
fixture_header.write_text(helper)
shaders = {
    'vs': 'void main(float a:TEXCOORD0,float b:TEXCOORD1,uint id:SV_VertexID,out float4 p:SV_Position,out float2 c:TEXCOORD0){p=float4(id==1?3:-1,id==2?3:-1,0,1);c=float2(a,b);}',
    'ps': 'float4 main(float4 p:SV_Position,float2 c:TEXCOORD0):SV_Target{return float4(c,0,1);}',
}
for stage, text in shaders.items():
    source = out / (stage + '.hlsl')
    source.write_text(text)
    subprocess.run([str(args.dxc), str(source), '-T', stage + '_6_0', '-E', 'main', '-Fo', str(out / (stage + '.dxil'))], check=True, timeout=30)
code = Path(__file__).with_name('test_vertex_binding_cache_gpu.cpp').read_text()
code = code.replace('#include "gpu_native/vertex_binding_cache.h"', '#include "gpu_native/vertex_binding_cache.h"\n#include "gpu_native/geometry_upload.h"')
code = code.replace('std::array<std::unique_ptr<plume::RenderBuffer>,5> buffers;', 'std::array<std::shared_ptr<plume::RenderBuffer>,5> buffers;')
needle = '  unsigned totalSlots=0,totalCalls=0,cases=0;'
assert code.count(needle) == 1
code = code.replace(needle, r'''
  std::vector<std::shared_ptr<plume::RenderBuffer>> staging;
  commands->begin();
  for(auto& buffer:buffers){
    auto local=CopyGeometryToDevice(device.get(),commands.get(),buffer.get(),256,false);CHECK(local);
    D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
    CHECK(SUCCEEDED(static_cast<plume::D3D12Buffer*>(local.get())->d3d->GetHeapProperties(&heap,&flags)));
    CHECK(heap.Type==D3D12_HEAP_TYPE_DEFAULT);
    staging.push_back(std::move(buffer));buffer=std::move(local);
  }
  auto index_source=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(256));
  std::array<uint16_t,128> indices{0,1,2};
  auto* index_mapped=index_source->map();CHECK(index_mapped);
  std::memcpy(index_mapped,indices.data(),256);index_source->unmap();
  auto index_local=CopyGeometryToDevice(device.get(),commands.get(),index_source.get(),256,true);CHECK(index_local);
  plume::RenderIndexBufferView index_view(index_local->at(0),256,plume::RenderFormat::R16_UINT);
  commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());
  staging.clear();index_source.reset();
''' + needle)
code = code.replace('commands->begin();commands->setGraphicsPipelineLayout', 'commands->begin();commands->setIndexBuffer(&index_view);commands->setGraphicsPipelineLayout')
assert 'commands->drawInstanced(3,1,0,0)' in code
code = code.replace('commands->drawInstanced(3,1,0,0)', 'commands->drawIndexedInstanced(3,1,0,0,0)')
cpp = out / 'local-geometry.cpp'
cpp.write_text(code)
exe = out / 'local-geometry.exe'
command = ['clang-cl', str(cpp), '/std:c++20', '/O2', '/MD', '/EHsc', '/UNDEBUG',
           '/I' + str(root / 'rexlego/src'), '/I' + str(root / 'thirdparty/plume'),
           '/I' + str(root / 'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
           '/Fo' + str(out) + '/', '/Fe' + str(exe), '/link', str(args.plume_lib.resolve()),
           'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'user32.lib']
compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
(out / 'compile.log').write_text(compiled.stdout + compiled.stderr)
compiled.check_returncode()
records = []
for cache in ('0', '1'):
    result = subprocess.run([str(exe), str(out / 'vs.dxil'), str(out / 'ps.dxil')],
        env=dict(os.environ, PLUME_D3D12_STATE_CACHE=cache), capture_output=True, text=True, timeout=60)
    (out / ('cache-' + cache + '.log')).write_text(result.stdout + result.stderr)
    result.check_returncode()
    assert 'PASS 28 GPU cases' in result.stdout
    records.append(dict(cache=cache, stdout=result.stdout, stderr=result.stderr))
copy = '  commands->copyBufferRegion(destination->at(0), source->at(0), bytes);'
assert helper.count(copy) == 1
fixture_header.write_text(helper.replace(copy, ''))
try:
    negative_exe = out / 'omitted-copy.exe'
    negative_command = ['/Fe' + str(negative_exe) if value.startswith('/Fe') else value for value in command]
    compiled = subprocess.run(negative_command, capture_output=True, text=True, timeout=60)
    compiled.check_returncode()
    negative = subprocess.run([str(negative_exe), str(out / 'vs.dxil'), str(out / 'ps.dxil')],
        env=dict(os.environ, PLUME_D3D12_STATE_CACHE='1'), capture_output=True, text=True, timeout=60)
    (out / 'omitted-copy.log').write_text(negative.stdout + negative.stderr)
    assert negative.returncode == 23, negative.stdout + negative.stderr
finally:
    fixture_header.write_text(helper)
(out / 'verification.json').write_text(json.dumps(dict(passed=True, device_local=True, records=records,
    omitted_copy_rejected=True, scope='56 indexed GPU readbacks from DEFAULT vertex/index buffers after staging release; no gameplay FPS claim'), indent=2))
print('PASS 56 device-local indexed GPU readbacks after staging release; omitted-copy negative rejected')
