"""Require the real-layout float oracle to reject the old unpadded snapshot extent."""
import argparse
import json
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('fixture',type=Path,help='Completed --float-formats SDK layout fixture directory')
p.add_argument('--plume-lib',required=True,type=Path)
p.add_argument('--runtime-dir',required=True,type=Path)
p.add_argument('--compiler',default='clang-cl')
a=p.parse_args()
root=Path(__file__).resolve().parents[2]
sdk=root.parent/'dev-assets/sdk-install'
out=a.fixture.resolve()
proof=json.loads((out/'verification.json').read_text())
assert proof['float_formats'] and all(x['exit']==0 for x in proof['cases'])
production_path=out/'partial-destination-production.inc'
production=production_path.read_text()
old='const u32 extent=TextureEndianExtent(layout.base.level_data_extent_bytes,TextureEndianXor(u32(fetch.endianness)));'
assert production.count(old)==1
assert old in (root/'rexlego/src/gpu_native/textures.cpp').read_text()
exe=out/'missing-float-word-padding.exe'
cmd=[a.compiler,str(out/'sdk-layout.cpp'),'/std:c++latest','/O2','/MD','/EHsc',
    '/I'+str(out),'/I'+str(root/'rexlego/src'),'/I'+str(sdk/'include'),
    '/I'+str(root/'thirdparty/plume'),'/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
    '/Fo'+str(out/'padding-mutant.obj'),'/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),
    str(sdk/'lib/rexruntime.lib'),'d3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
try:
    production_path.write_text(production.replace(old,'const u32 extent=layout.base.level_data_extent_bytes;'))
    r=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
    (out/'padding-mutant-compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
    r=subprocess.run([str(exe)],env=dict(os.environ,PATH=str(a.runtime_dir.resolve())+os.pathsep+os.environ['PATH'],
        PLUME_D3D12_STATE_CACHE='1'),capture_output=True,text=True,timeout=120)
    (out/'padding-negative.log').write_text(r.stdout+r.stderr)
    if r.returncode!=23:raise RuntimeError('Unpadded odd-width half-float extent escaped the GPU comparison')
    (out/'padding-negative.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
        actual_sdk_layout=True,actual_production_materialization=True,negative_exit=r.returncode,
        stderr=r.stderr,stdout=r.stdout,scope='Old extent rejects endian-swapped final half-float publication'),indent=2))
finally:
    production_path.write_text(production)
print('PASS negative control: omitting endian-word padding breaks real SDK half-float writeback')
