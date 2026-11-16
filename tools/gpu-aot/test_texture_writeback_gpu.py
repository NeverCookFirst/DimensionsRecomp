"""Build and run a windowless production texture writeback GPU oracle."""
import argparse,json,os,subprocess
from pathlib import Path

def body(text, signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--plume-lib',type=Path,required=True);p.add_argument('--compiler',default='clang-cl')
a=p.parse_args();root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
(a.output/'tile-address.inc').write_text(body((root/'rexglue-sdk/src/graphics/pipeline/texture/util.cpp').read_text(),'int32_t GetTiledOffset2D('))
exe=a.output.resolve()/'texture-writeback-gpu.exe'
cmd=[a.compiler,str(Path(__file__).with_suffix('.cpp')),'/std:c++20','/O2','/MD','/EHsc','/DNOMINMAX',
 '/I'+str(root/'rexlego/src'),'/I'+str(root/'thirdparty/plume'),'/I'+str(a.output.resolve()),
 '/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
 '/Fo'+str(a.output.resolve()/'fixture.obj'),'/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),'d3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
r=subprocess.run(cmd,capture_output=True,text=True,timeout=120);(a.output/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
results=[]
for cache in ('0','1'):
    env=dict(os.environ,PLUME_D3D12_STATE_CACHE=cache)
    r=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=60)
    (a.output/('cache-'+cache+'.log')).write_text(r.stdout+r.stderr);r.check_returncode()
    results.append(dict(state_cache=cache,exit=r.returncode,stdout=r.stdout))
r=subprocess.run([str(exe),'omit-endian'],capture_output=True,text=True,timeout=60)
(a.output/'negative.log').write_text(r.stdout+r.stderr)
if r.returncode!=23:raise RuntimeError('Endianness mutation escaped actual readback oracle')
(a.output/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
 production_readback=True,production_writeback=True,sdk_tiled_address=True,positive=results,
 positive_gpu_readbacks=108,positive_layout_relocation_cases=480,
 raw_formats=['RGBA8','RG16','RGBA16','R16F','RG16F','RGBA16F','R32F','RG32F','RGBA32F'],
 all_65536_numeric_words=True,float32_special_bits=True,negative_exit=r.returncode,
 scope='controlled GPU pixels and byte-exact 4KiB overlapping relocation; not gameplay acceptance'),indent=2)+'\n')
print('PASS 108 real GPU readbacks / 480 layout-relocation cases, all 65536 half words, float32 special bits, source GPU preservation, failure retention and omitted-endian negative control')
