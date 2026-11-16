"""Exercise production geometry reuse and upload order with real D3D12 tapes."""
import argparse
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
p.add_argument('output',type=Path);p.add_argument('--plume-lib',type=Path,required=True)
p.add_argument('--compiler',default='clang-cl');a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
source=(root/'rexlego/src/gpu_native/buffers.cpp').read_text()
(out/'geometry-resource.inc').write_text(source[source.index('struct BufferResource {'):source.index('std::mutex g_buffers_mutex;')])
(out/'geometry-production.inc').write_text('\n'.join(body(source,signature) for signature in [
    'void ByteSwapElements(','std::shared_ptr<plume::RenderBuffer> CreateImmutableUploadBuffer(',
    'std::shared_ptr<GeometryPublication> PromoteImmutableGeometry(',
    'plume::RenderBuffer* ResolveBufferContents(']))
header=(root/'rexlego/src/gpu_native/geometry_content_cache.h').read_text()

def run(name,cache_header):
    include=out/name/'gpu_native';include.mkdir(parents=True,exist_ok=True)
    (include/'geometry_content_cache.h').write_text(cache_header)
    exe=out/(name+'.exe')
    cmd=[a.compiler,str(Path(__file__).with_suffix('.cpp')),
        str(root/'rexlego/src/gpu_native/native_recording.cpp'),'/std:c++20','/O2','/MD','/EHsc','/UNDEBUG',
        '/I'+str(include.parent),'/I'+str(out),'/I'+str(root/'rexlego/src'),
        '/I'+str(root/'thirdparty/plume'),'/I'+str(root/'rexglue-sdk/thirdparty/xxHash'),
        '/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
        '/Fo'+str(out/name)+'/', '/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),
        'd3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
    r=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
    (out/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
    records=[]
    for cache in ['0','1'] if name=='positive' else ['1']:
        env=dict(os.environ,PLUME_D3D12_STATE_CACHE=cache,LEGO_NATIVE_GEOMETRY_REUSE='1',LEGO_NATIVE_LOCAL_GEOMETRY='1')
        r=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=60)
        (out/(name+'-cache-'+cache+'.log')).write_text(r.stdout+r.stderr)
        records.append(dict(cache=cache,exit=r.returncode,stdout=r.stdout.strip()))
    return records

positive=run('positive',header)
assert all(record['exit']==0 for record in positive),positive
pending=run('omit-ready-guard',header.replace('!entry->publication->ready.load(std::memory_order_acquire)','false'))
assert pending[0]['exit']==23,pending
abandoned=run('publish-abandoned-tape',header.replace('if (state->recorded.load(std::memory_order_acquire))','if (true)'))
assert abandoned[0]['exit']==23,abandoned
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
    actual_production_resolve=True,actual_production_promotion=True,actual_production_tapes=True,
    actual_gpu=True,guest_memory_and_retirement_queue_stubbed=True,
    positive=positive,pending_negative=pending,abandoned_negative=abandoned),indent=2)+'\n')
for record in positive:print(record['stdout'])
print('PASS negative controls: premature reuse and publication of abandoned tape are rejected')
