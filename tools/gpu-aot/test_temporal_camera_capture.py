"""Validate the projection decoder against saved draw constants, offline."""
import argparse
import json
from pathlib import Path
import re
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path);p.add_argument('--capture',type=Path,action='append',required=True)
p.add_argument('--compiler',default='clang++');p.add_argument('--bank',type=Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
draw=(root/'rexlego/src/gpu_native/draw.cpp').read_text()
profiles=draw[draw.index('bool HasVerifiedTemporalCamera('):draw.index('struct SharedConstants')]
verified={int(v,16) for v in re.findall(r'0x([0-9A-Fa-f]+)ull',profiles)}
if a.bank:
    metadata=(a.bank/'temporal-bank.cpp').read_text()
    verified |= {int(v,16) for v in re.findall(r'\{0x([0-9A-F]+)ull,0,0,\d+,\d+,1\}',metadata)}
matrices=[]
for path in a.capture:
    for block in re.split(r'(?=^draw=)',path.read_text(),flags=re.M):
        match=re.search(r'VS=([0-9a-fA-F]+)',block)
        if not match or int(match[1],16) not in verified:continue
        if a.bank and not ('viewport_width_bits=44a00000' in block and 'viewport_height_bits=44340000' in block):continue
        rows=[re.search(r'^VS_C'+str(i)+r'=([0-9a-fA-F,]+)',block,re.M) for i in range(4)]
        if not all(rows):continue
        bits=[int(v,16) for row in rows for v in row[1].split(',') if v]
        assert len(bits)==16;matrices.append(bits)
assert matrices
cpp=a.output/'camera-capture.cpp';exe=a.output/'camera-capture.exe'
cpp.write_text('#include "gpu_native/temporal_camera.h"\n#include <bit>\n#include <cassert>\n#include <cstdio>\nusing namespace legodimensions::gpu_native;\nint main(){\nconst unsigned matrices[][16]={\n'+
 ',\n'.join('{'+','.join(f'0x{v:08X}u' for v in m)+'}' for m in matrices)+
 '};\nfor(auto& words:matrices){std::array<float,16> m;for(int i=0;i<16;++i)m[i]=std::bit_cast<float>(words[i]);auto camera=DecodeTemporalCamera(m);assert(camera && camera->near_plane>0 && camera->far_plane>camera->near_plane);}\nprintf("PASS: %zu saved world camera matrices\\n",std::size(matrices));}\n')
subprocess.run([a.compiler,'-std=c++20','-O2','-UNDEBUG','-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True,timeout=45)
result=subprocess.run([str(exe.resolve())],capture_output=True,text=True,timeout=30);print(result.stdout+result.stderr);result.check_returncode()
(a.output/'verification.json').write_text(json.dumps({'game_launched':False,'matrix_count':len(matrices),'result':'PASS','captures':[str(p) for p in a.capture]},indent=2)+'\n')
