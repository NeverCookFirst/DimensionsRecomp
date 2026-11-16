"""Real UNORM24 upload + pinned SDK layout + headless D3D12 native-tape integration.

Guest memory/watch callbacks, descriptor index allocation and fence-retirement
registration are injected. The production upload/mirror/publication bodies,
depth conversion, exported SDK layout/tiling functions, native command tapes,
GPU texture/buffer/view allocation, copies and completed-fence readbacks execute.
No game entry point, launcher, capture or window is used.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess

def block(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path)
p.add_argument('--plume-lib',required=True,type=Path);p.add_argument('--runtime',required=True,type=Path)
p.add_argument('--reference-textures',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];sdk=root.parent/'dev-assets/sdk-install'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
src=root/'rexlego/src/gpu_native';fixture=Path(__file__).with_suffix('.cpp')
text=(src/'textures.cpp').read_text()
shutil.copyfile(a.runtime,out/'rexruntime.dll')
shutil.copyfile(a.runtime.parent/'amd_fidelityfx_dx12.dll',out/'amd_fidelityfx_dx12.dll')
def bodies(source):
    names=['bool EnsureDepthSamplingMirror(','bool UploadGuestDepthTexture(']
    if 'bool QueuePreparedTextureUpload(' in source:
        names=['bool QueuePreparedTextureUpload(','void PublishPreparedTextureUpload(']+names
    return '\n'.join(block(source,n) for n in names)

def run(name,body):
    folder=out/name;folder.mkdir(exist_ok=True)
    (folder/'depth-publication-production.inc').write_text(body)
    exe=out/(name+'.exe')
    command=['clang-cl',str(fixture),str(src/'native_recording.cpp'),'/std:c++latest','/O2','/MD','/EHsc','/UNDEBUG',
        '/DSPDLOG_FMT_EXTERNAL','/I'+str(folder),'/I'+str(root/'rexlego/src'),'/I'+str(sdk/'include'),
        '/I'+str(root/'thirdparty/plume'),'/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
        '/Fo'+str(folder)+'/', '/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),str(sdk/'lib/rexruntime.lib'),
        'd3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
    result=subprocess.run(command,capture_output=True,text=True,timeout=150)
    (out/(name+'-compile.log')).write_text(result.stdout+result.stderr);result.check_returncode()
    results=[]
    for cache in ['0','1'] if name=='production' else ['1']:
        result=subprocess.run([str(exe)],env=dict(os.environ,PLUME_D3D12_STATE_CACHE=cache),capture_output=True,text=True,timeout=60)
        (out/(name+'-cache-'+cache+'.log')).write_text(result.stdout+result.stderr)
        results.append(dict(cache=cache,exit=result.returncode,stdout=result.stdout,stderr=result.stderr))
    return results

body=bodies(text);positive=run('production',body)
assert all(r['exit']==0 for r in positive),positive
negative=[]
for name,old,new in [
    ('cache-hit-without-consumer','QueuePreparedTextureUpload(resource, commands)','true'),
    ('decode-live-not-retained','const auto* source = retained->Base().data();','const auto* source = guest.bytes.data();'),
    ('ignore-fetch-endian','row_pitch, endian_xor, source_offset','row_pitch, 0, source_offset'),
    ('ignore-packed-tail','if (fetch.packed_mips)','if (false && fetch.packed_mips)'),
]:
    if name=='ignore-fetch-endian':
        # This is the production helper's actual caller, not a copied decoder.
        old,new='pitch, endian_xor,','pitch, 0,'
    assert old in body,(name,old)
    result=run(name,body.replace(old,new))
    assert result[0]['exit']==23,(name,result)
    negative.append(dict(name=name,results=result))
reference=None
if a.reference_textures:
    reference_text=a.reference_textures.read_text();reference=run('frozen-native-reference',bodies(reference_text))
    assert reference[0]['exit']==23,reference
sources=[src/n for n in ['textures.cpp','native_recording.cpp','native_recording.h','texture_upload_publication.h',
    'texture_upload_source.h','texture_upload_cache.h','texture_depth_upload.h','texture_volume_upload.h','depth_alias.h','texture_swizzle.h']]
sources += [fixture,Path(__file__).resolve()]
report=dict(passed=True,game_launched=False,window_opened=False,actual_gpu=True,scope=__doc__,
    sources={str(f):sha(f) for f in sources},runtime_sha256=sha(a.runtime),runtime_dependency_sha256=sha(a.runtime.parent/'amd_fidelityfx_dx12.dll'),runtime_import_sha256=sha(sdk/'lib/rexruntime.lib'),
    plume_sha256=sha(a.plume_lib),positive=positive,negative=negative,
    reference=None if not reference else dict(path=str(a.reference_textures),sha256=sha(a.reference_textures),results=reference),
    limits=['CPU byte capture occurs at upload time, not guest draw-capture time; data-provider registration/producer ownership remain unwired.',
        'Actual guest write-fault handling and frame submission/retirement lifecycle are injected boundaries.',
        'R32 mirror data is checked by real GPU readbacks; real scene depth rendering, stencil, resolve/alias transfers and sampled swizzle are not exercised.',
        'No world visual acceptance, AMD/Intel device certification, Linux or whole-game performance claim.'])
(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
for r in positive:print(r['stdout'].strip())
print('PASS four broken variants'+(' and frozen native reference' if reference else '')+' rejected; no game or window.')
