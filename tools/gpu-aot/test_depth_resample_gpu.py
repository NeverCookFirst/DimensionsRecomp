"""Compile and execute actual depth/stencil resize code, with targeted mutants."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--dxc',type=Path,required=True);p.add_argument('--captured-depth',type=Path,required=True)
p.add_argument('--production-shader-dir',type=Path,
    help='Execute the exact arrays generated for the application, rather than separately compiled shaders')
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2];shaders=root/'rexlego/src/gpu_native/shaders'
def compile_shader(source,name,profile,include=shaders):
    target=out/(name+'.dxil')
    r=subprocess.run([str(a.dxc),str(source),'-T',profile,'-E','main','-HV','2021',
        '-all-resources-bound','-Wno-ignored-attributes','-I',str(include),'-Fo',str(target)],capture_output=True,text=True,timeout=120)
    (out/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode();return target
names=['depth_resample_ps','depth_resample_depth_ps','depth_resample_stencil_ps']
def production(name,stage):
    if not a.production_shader_dir:
        return compile_shader(shaders/(name+'.hlsl'),name,stage+'_6_6')
    text=(a.production_shader_dir/(name+'.h')).read_text()
    match=re.search(r'const unsigned char g_'+re.escape(name)+r'_dxil\[\]\s*=\s*\{([^}]+)\};',text)
    if not match:raise RuntimeError('Missing exact production array: '+name)
    tokens=match[1].split(',');data=bytearray()
    for token in tokens:
        token=token.strip()
        if not token:continue
        if not re.fullmatch(r'0x[0-9a-fA-F]{2}',token):raise RuntimeError('Unexpected shader array token')
        data.append(int(token,16))
    if data[:4]!=b'DXBC':raise RuntimeError('Invalid shader container')
    path=out/(name+'.dxil');path.write_bytes(data);return path
vs=production('copy_vs','vs')
programs=[production(n,'ps') for n in names]
exe=out/'depth-resample-gpu.exe'
r=subprocess.run(['clang-cl',str(Path(__file__).with_suffix('.cpp')),
    str(root/'rexlego/src/gpu_native/depth_resample.cpp'),'/std:c++20','/O2','/MD','/EHsc','/DNOMINMAX',
    '/I'+str(root/'rexlego/src'),'/Fo'+str(out)+'/', '/Fe'+str(exe),'/link',
    'd3d12.lib','dxgi.lib','dxguid.lib'],capture_output=True,text=True,timeout=120)
(out/'fixture-compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
def run(paths,name,warp=False):
    r=subprocess.run([str(exe),str(vs),*[str(x) for x in paths],str(a.captured_depth.resolve()),*(['warp'] if warp else [])],
        capture_output=True,text=True,timeout=120)
    (out/(name+'.log')).write_text(r.stdout+r.stderr);return r
positive=run(programs,'positive');print(positive.stdout);positive.check_returncode()
assert 'PASS cases=14' in positive.stdout and 'errors=0 warnings=0' in positive.stdout
warp=run(programs,'warp',True);print(warp.stdout);warp.check_returncode()
assert 'PASS cases=14' in warp.stdout
negative=[]
for name,mutation,expected in [
    ('omit-stencil',lambda s:s.replace('SourceStencil.Load(int3(pixel,0)).y','0u').replace('SourceStencil.Load(int3(SourcePixel(position),0)).y','0u'),'stencil mismatch'),
    ('edge-sample',lambda s:s.replace('((pixel*2+1)*uint2(SourceWidth,SourceHeight)) /\n        (2*uint2(DestinationWidth,DestinationHeight))','(pixel*uint2(SourceWidth,SourceHeight)) /\n        uint2(DestinationWidth,DestinationHeight)'),'depth mismatch')]:
    folder=out/name;folder.mkdir(exist_ok=True)
    for source in [shaders/'depth_resample_common.hlsli',*[shaders/(n+'.hlsl') for n in names]]:
        (folder/source.name).write_text(mutation(source.read_text()))
    bad=[compile_shader(folder/(n+'.hlsl'),name+'-'+n,'ps_6_6',folder) for n in names]
    result=run(bad,name)
    if result.returncode!=23 or expected not in result.stdout:
        raise RuntimeError(name+' escaped GPU oracle: '+result.stdout+result.stderr)
    bad_warp=run(bad,name+'-warp',True)
    if bad_warp.returncode!=23 or expected not in bad_warp.stdout:
        raise RuntimeError(name+' escaped WARP GPU oracle: '+bad_warp.stdout+bad_warp.stderr)
    negative.append(dict(name=name,exit=result.returncode,stdout=result.stdout,
        warp_exit=bad_warp.returncode,warp_stdout=bad_warp.stdout))
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
report=dict(game_launched=False,window_opened=False,production_recorder=True,
    exact_application_shader_arrays=bool(a.production_shader_dir),
    executed_shaders=[dict(name=x.name,sha256=sha(x)) for x in [vs,*programs]],
    positive=dict(exit=positive.returncode,stdout=positive.stdout),warp=dict(exit=warp.returncode,stdout=warp.stdout),negative=negative,
    captured_depth=dict(path=str(a.captured_depth),sha256=sha(a.captured_depth)),
    scope='14 cases per adapter: automatic and portable strategies, high-performance DXGI adapter and WARP; controlled all-bit stencil and actual saved depth; optional fast branch only proven if pixel_stencil_reference=1; debug diagnostics only when debug_layer=1; not game integration or frame performance')
(out/'verification.json').write_text(json.dumps(report,indent=2))
print('PASS production depth/stencil resize and two targeted broken-shader controls')
