"""Execute production composition transactions/copy functions on real GPU memory."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--plume-lib',type=Path,required=True);p.add_argument('--production-shader-dir',type=Path,required=True)
p.add_argument('--captured-color','--source-color',dest='captured_color',type=Path,required=True);p.add_argument('--captured-depth','--source-depth',dest='captured_depth',type=Path,required=True)
p.add_argument('--input-kind',choices=['saved-capture','synthetic'],default='saved-capture')
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
textures=(root/'rexlego/src/gpu_native/textures.cpp').read_text();device=(root/'rexlego/src/gpu_native/device.cpp').read_text()
def block(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
(out/'composition-storage-struct.inc').write_text(block(textures,'struct SurfaceStorage {')+';')
recorders='\n'.join([block(device,'static bool TextureSlotLive('),block(device,'bool HostDevice::ResolveHdrColor('),block(device,'bool HostDevice::ResampleDepthSurface('),
    block(textures,'struct PreparedNativeStorage {')+';',
    *[block(textures,s) for s in ['void PublishPromotedViewport(','bool PrepareNativeStorage(', 'void CommitNativeStorage(',
        'bool ResizeNativeStorage(','void SwapCompositionStorage(','bool CopyCompositionStorage(',
        'bool BeginCompositionSurface(','bool EndNativeCompositionTargets(','bool RetryNativeCompositionTargets(','bool BeginNativeCompositionTargets(',
        'bool ReconfigureNativeSceneSurfaces(NativeRenderExtent extent)']]])
(out/'production-composition-recorders.inc').write_text(recorders)
exe=out/'composition-storage-gpu.exe'
cmd=['clang-cl',str(Path(__file__).with_suffix('.cpp')),str(root/'rexlego/src/gpu_native/depth_resample.cpp'),
    '/std:c++latest','/O2','/MD','/EHsc','/UNDEBUG','/DNOMINMAX']
for folder in [out,root/'rexlego/src',root/'rexglue-sdk/include',root/'thirdparty/plume',
    root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include',a.production_shader_dir.resolve().parents[1]]:
    cmd.append('/I'+str(folder))
cmd+=['/Fo'+str(out)+'/', '/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),'d3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
r=subprocess.run(cmd,capture_output=True,text=True,timeout=120);(out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
results=[]
for cache in ('0','1'):
    r=subprocess.run([str(exe),str(a.captured_color.resolve()),str(a.captured_depth.resolve())],
        env=dict(os.environ,PLUME_D3D12_STATE_CACHE=cache),capture_output=True,text=True,timeout=120)
    (out/('cache-'+cache+'.log')).write_text(r.stdout+r.stderr);print(r.stdout);r.check_returncode()
    assert 'PASS 15 real GPU transaction cases' in r.stdout
    results.append(dict(state_cache=cache,exit=r.returncode,stdout=r.stdout))
needle='if(preserve_writes && !CopyCompositionStorage(*resource,true))'
assert needle in recorders
(out/'production-composition-recorders.inc').write_text(recorders.replace(needle,'if(false)'))
bad=out/'omit-copyback.exe';bad_cmd=[('/Fe'+str(bad)) if item=='/Fe'+str(exe) else item for item in cmd]
r=subprocess.run(bad_cmd,capture_output=True,text=True,timeout=120)
(out/'negative-compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
r=subprocess.run([str(bad),str(a.captured_color.resolve()),str(a.captured_depth.resolve())],
    env=dict(os.environ,PLUME_D3D12_STATE_CACHE='1'),capture_output=True,text=True,timeout=120)
(out/'omit-copyback.log').write_text(r.stdout+r.stderr)
if r.returncode not in (24,25) or 'mismatch' not in r.stdout:
    raise RuntimeError('Omitted copyback escaped real GPU content oracle: '+r.stdout+r.stderr)
(out/'production-composition-recorders.inc').write_text(recorders)
(out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,window_opened=False,
    production_storage_and_copy_bodies=True,real_plume=True,positive=results,
    grouped_scene_reconfiguration=True,
    omitted_copyback_negative=dict(exit=r.returncode,stdout=r.stdout),
    input_data_kind=a.input_kind,source_color_sha256=hashlib.sha256(a.captured_color.read_bytes()).hexdigest(),
    source_depth_sha256=hashlib.sha256(a.captured_depth.read_bytes()).hexdigest(),
    sources={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in [root/'rexlego/src/gpu_native/textures.cpp',
        root/'rexlego/src/gpu_native/textures.h',root/'rexlego/src/gpu_native/device.cpp',root/'rexlego/src/gpu_native/depth_resample.cpp',
        Path(__file__).resolve(),Path(__file__).with_suffix('.cpp')]},
    scope='actual production allocation transaction, color/depth copy and backing swap on GPU, five presets/native writes/copyback; see input_data_kind for raw input provenance; host registry/fence harness, not full original game or FPS'),indent=2))
print('PASS production composition storage GPU transaction, caches off/on')
