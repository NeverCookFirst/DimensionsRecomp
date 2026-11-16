"""Headless D3D12 checks of actual upload publication helpers and native tapes.

Executes actual production decoded-cache-hit control flow. Texture decoding,
guest memory, resource storage/descriptor lifetimes and the frame-fence queue
are injected boundaries. This does not prove draw-capture source versions or
all GPU producer ordering, and never launches the game or a window.
"""
from pathlib import Path
import argparse, hashlib, json, os, subprocess

def block(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
def sha(p):return hashlib.file_digest(p.open('rb'),'sha256').hexdigest()

p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path)
p.add_argument('--plume-lib',required=True,type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
src=root/'rexlego/src/gpu_native';production=src/'textures.cpp';fixture=Path(__file__).with_suffix('.cpp')
text=production.read_text();header=(src/'texture_upload_publication.h').read_text()
recording=(src/'native_recording.cpp').read_text()
viewport='c->setViewports(v.data(),uint32_t(v.size()));'
assert recording.count(viewport)==1
# Count only commands that the production replay-state cache actually emits.
# No control flow or binding/cache behavior is substituted.
instrumented='unsigned g_native_test_viewport_binds=0;\n'+recording.replace(viewport,'++g_native_test_viewport_binds; '+viewport)
helper='\n'.join(block(text,s) for s in ['bool QueuePreparedTextureUpload(', 'void PublishPreparedTextureUpload('])
hit=block(text,'if (resource->guest_uploaded && resource->guest_content_hash == content_hash &&')
(out/'texture-publication-production.inc').write_text(helper)

def run(name,contents,hit_contents=hit):
    include=out/name/'gpu_native';include.mkdir(parents=True,exist_ok=True)
    (include/'texture_upload_publication.h').write_text(contents)
    recording_source=include.parent/'native_recording.cpp'
    recording_source.write_text(instrumented)
    (out/'texture-publication-cache-hit.inc').write_text(hit_contents)
    exe=out/(name+'.exe')
    cmd=['clang-cl',str(fixture),str(recording_source),'/std:c++20','/O2','/MD','/EHsc','/UNDEBUG',
         '/I'+str(include.parent),'/I'+str(out),'/I'+str(root/'rexlego/src'),
         '/I'+str(root/'thirdparty/plume'),'/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
         '/Fo'+str(include.parent)+'/', '/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),
         'd3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
    r=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
    (out/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
    results=[]
    for cache in ['0','1'] if name=='production' else ['1']:
        r=subprocess.run([str(exe)],env=dict(os.environ,PLUME_D3D12_STATE_CACHE=cache),capture_output=True,text=True,timeout=60)
        (out/(name+'-cache-'+cache+'.log')).write_text(r.stdout+r.stderr)
        results.append(dict(cache=cache,exit=r.returncode,stdout=r.stdout,stderr=r.stderr))
    return results

positive=run('production',header)
assert all(r['exit']==0 for r in positive),positive
negative=[]
for name,contents,hit_contents in [
    ('old-skip-without-consumer',header,hit.replace('QueuePreparedTextureUpload(*resource, commands)','true')),
    ('preparation-publishes-early',header.replace('deferred->ResourceCommand([self = shared_from_this()]','state_->version = version_; deferred->ResourceCommand([self = shared_from_this()]'),hit),
    ('host-write-without-invalidation',header.replace('else state->version = 0;','else (void)state;'),hit),
    ('unbounded-staging-cache',header.replace('while (!entries_.empty() && bytes_ > budget_ - publication->Bytes())','while (false)'),hit),
    ('resource-callback-forgets-bindings',header.replace('deferred->ResourceCommand(', 'deferred->External('),hit),
]:
    assert contents!=header or hit_contents!=hit,name
    result=run(name,contents,hit_contents);assert result[0]['exit']==23,(name,result)
    negative.append(dict(name=name,results=result))
(out/'texture-publication-cache-hit.inc').write_text(hit)
sources=[production,src/'texture_upload_publication.h',src/'native_recording.cpp',src/'native_recording.h',fixture,Path(__file__).resolve()]
report=dict(passed=True,game_launched=False,window_opened=False,actual_gpu=True,actual_native_tapes=True,
    actual_upload_publication_helpers=True,actual_decoded_cache_hit=True,actual_binding_cache_checked=True,
    instrumentation='One emission counter immediately before the unchanged production viewport API call.',
    scope=__doc__,sources={str(p):sha(p) for p in sources},positive=positive,negative=negative,
    limits=['The full texture decoder, guest CPU writes and draw-capture sources are not exercised by this GPU fixture.',
            'Retirement and target ownership are injected boundaries. Full native resolve/alias ordering still needs integration coverage.',
            'Cache budget correctness is tested, but whole-game memory/performance and world visuals are not established.'])
(out/'verification.json').write_text(json.dumps(report,indent=2))
for r in positive:print(r['stdout'].strip())
print('PASS: five broken variants rejected; no game or window launched.')
