"""Execute pre-composition presenter lifecycle, native UI and targeted failures."""
import argparse,hashlib,json,os,struct,subprocess,shutil
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--plume-lib',type=Path,required=True);p.add_argument('--dxc',type=Path,required=True)
p.add_argument('--warp-mesh',type=Path,help='Actual saved post-VS buffer with verified304-byte position/UV packing')
p.add_argument('--original-warp-inputs',type=Path,help='Verified original shader/IA/CBV capture export; post-VS is oracle only')
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
if a.original_warp_inputs:
    a.original_warp_inputs=a.original_warp_inputs.resolve()
    a.warp_mesh=a.original_warp_inputs/'post-vs.bin'
    assert hashlib.sha256((a.original_warp_inputs/'vs.dxil').read_bytes()).hexdigest()=='d4de48c66d9877d36612e1345d7b264b712d5d366316922df4217bd1f09c63b7'
    assert hashlib.sha256((a.original_warp_inputs/'ps.dxil').read_bytes()).hexdigest()=='969196d9445b993d282aece0916fe183a4ab69703670d546126cd24f763fa9df'
    assert struct.unpack_from('<f',(a.original_warp_inputs/'cb1.bin').read_bytes(),45*16+4)==(1.,)
    assert struct.unpack_from('<4f',(a.original_warp_inputs/'cb2.bin').read_bytes(),608)==(1.,1.,1.,1.)
(out/'include/rex').mkdir(parents=True,exist_ok=True)
(out/'include/rex/logging.h').write_text('#pragma once\n#define REXLOG_INFO(...) ((void)0)\n#define REXLOG_WARN(...) ((void)0)\n#define REXLOG_ERROR(...) ((void)0)\n')
shaders=root/'rexlego/src/gpu_native/shaders';programs=[]
for stage,name in [('vs','copy_vs'),('ps','copy_color_ps')]:
    target=out/(stage+'.dxil');subprocess.run([str(a.dxc),str(shaders/(name+'.hlsl')),
        '-T',stage+'_6_0','-E','main','-Fo',str(target)],check=True,capture_output=True)
    programs.append(target)
if a.original_warp_inputs:
    programs=[a.original_warp_inputs/(stage+'.dxil') for stage in ('vs','ps')]
elif a.warp_mesh:
    source=out/'captured-grid-vs.hlsl';source.write_text('void main(float4 position:POSITION,float2 uv:TEXCOORD,out float4 p:SV_Position,out float2 t:TEXCOORD){p=position;t=uv;}')
    subprocess.run([str(a.dxc),str(source),'-T','vs_6_0','-E','main','-Fo',str(programs[0])],check=True,capture_output=True)
source=root/'rexlego/src/gpu_native/temporal_present.cpp'
original=source.read_text()
def build(name,presenter):
    folder=out/name;folder.mkdir(exist_ok=True);path=folder/'presenter.cpp';path.write_text(presenter)
    exe=folder/'present.exe'
    cmd=['clang-cl',str(Path(__file__).with_name('test_temporal_present_gpu.cpp')),str(path),
        str(root/'rexlego/src/gpu_native/fsr3_upscaler.cpp'),'/std:c++latest','/O2','/MD','/EHsc','/UNDEBUG','/DNOMINMAX']
    for directory in [out/'include',root/'rexlego/src',root/'rexglue-sdk/include',root/'thirdparty/plume',
        root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include',
        root/'rexglue-sdk/thirdparty/FidelityFX-SDK/sdk/include',root/'rexglue-sdk/thirdparty/FidelityFX-SDK/ffx-api/include']:
        cmd.append('/I'+str(directory))
    cmd+=['/Fo'+str(folder)+'/', '/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),
        str(root/'rexglue-sdk/bin/ffx_sdk/ffx_backend_dx12_x64.lib'),
        str(root/'rexglue-sdk/bin/ffx_sdk/ffx_fsr3upscaler_x64.lib'),'d3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
    result=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
    (folder/'compile.log').write_text(result.stdout+result.stderr);result.check_returncode();return exe
def run(exe,w,h,cache,name,mesh=None,original_inputs=None,switch=False):
    selected=mesh or a.warp_mesh
    env=dict(os.environ,PLUME_D3D12_STATE_CACHE=cache)
    env.pop('LEGO_TEST_SWITCH_PRESETS',None)
    if switch:env['LEGO_TEST_SWITCH_PRESETS']='1'
    result=subprocess.run([str(exe),str(w),str(h),*[str(x) for x in programs],'compose',
        *([str(selected.resolve()),str(a.warp_mesh.resolve())] if selected else []),
        *([str(original_inputs or a.original_warp_inputs)] if a.original_warp_inputs else [])],
        capture_output=True,text=True,timeout=120,env=env)
    (out/(name+'.log')).write_text(result.stdout+result.stderr);return result
exe=build('production',original);positive=[]
for cache in ('0','1'):
    for w,h in [(1920,1080),(1280,720),(1129,635),(960,540),(640,360)]:
        result=run(exe,w,h,cache,f'{w}-cache-{cache}');print(result.stdout);result.check_returncode()
        assert 'production presentation' in result.stdout
        positive.append(dict(render=[w,h],state_cache=cache,exit=result.returncode,stdout=result.stdout))
switching=[]
if a.original_warp_inputs:
    for cache in ('0','1'):
        result=run(exe,1920,1080,cache,'preset-switch-cache-'+cache,switch=True)
        print(result.stdout);result.check_returncode()
        assert result.stdout.count('reconstructed immediately')==6
        assert result.stdout.count('presenter retained')==5
        switching.append(dict(state_cache=cache,exit=result.returncode,stdout=result.stdout))
negative=[]
for name,line in [('double-dispatch','if (TemporalSceneComposed(frame)) return color;'),
                  ('late-jitter','if (TemporalSceneComposed(frame)) return false;')]:
    assert line in original
    bad=build(name,original.replace(line,''));result=run(bad,1129,635,'1',name)
    if result.returncode!=23:raise RuntimeError(name+' escaped lifecycle check: '+result.stdout+result.stderr)
    negative.append(dict(name=name,exit=result.returncode,stdout=result.stdout))
if a.original_warp_inputs:
    bad_inputs=out/'wrong-pixel-gain';bad_inputs.mkdir(exist_ok=True)
    for filename in ('vb-0.bin','cb0.bin','cb1.bin','cb2.bin'):
        shutil.copy2(a.original_warp_inputs/filename,bad_inputs/filename)
    raw=bytearray((bad_inputs/'cb1.bin').read_bytes());struct.pack_into('<f',raw,45*16+4,.75)
    (bad_inputs/'cb1.bin').write_bytes(raw)
    result=run(exe,1129,635,'1','wrong-pixel-gain',original_inputs=bad_inputs)
    if result.returncode!=24:raise RuntimeError('Incorrect original PS gain escaped pixel oracle: '+result.stdout+result.stderr)
    negative.append(dict(name='wrong-pixel-gain',exit=result.returncode,stdout=result.stdout))
elif a.warp_mesh:
    raw=bytearray(a.warp_mesh.read_bytes());assert len(raw)==1536*304
    for offset in range(0,len(raw),304):
        x,y,z,w=struct.unpack_from('<4f',raw,offset)
        struct.pack_into('<2f',raw,offset+16,x/w*.5+.5,.5-y/w*.5)
    identity=out/'identity-warp.bin';identity.write_bytes(raw)
    result=run(exe,1129,635,'1','identity-warp',identity)
    if result.returncode!=24:raise RuntimeError('Missing nonlinear UV warp escaped pixel oracle: '+result.stdout+result.stderr)
    negative.append(dict(name='identity-warp',exit=result.returncode,stdout=result.stdout))
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
    production_presenter=True,positive=positive,preset_switching=switching,negative=negative,native_ui_width=1,
    captured_warp_sha256=hashlib.sha256(a.warp_mesh.read_bytes()).hexdigest() if a.warp_mesh else None,
    original_game_shaders=bool(a.original_warp_inputs),
    scope=('real FSR/presenter,24 frames per preset/cache; native one-pixel UI and lifecycle; '+
        ('original executed VS/PS, raw IA, captured constants/sampler, relocated descriptors; post-VS used only as independent oracle; controlled scene/depth/motion; failed-FSR frames check raw jittered-source warp, not full final fallback presentation; not full engine integration or FPS'
        if a.original_warp_inputs else 'captured post-VS warp if supplied, passthrough VS and controlled band-limited source/copy PS, not original game shaders/full engine integration or FPS'))),indent=2))
print('PASS pre-composition presenter across presets/caches, native one-pixel UI, final pass-through and broken lifecycle controls')
