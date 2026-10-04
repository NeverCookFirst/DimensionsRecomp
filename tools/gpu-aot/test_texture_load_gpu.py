"""Run actual SDK texture loader bytecode on D3D12 with synthetic input only."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
s=(root/'rexglue-sdk/src/graphics/pipeline/texture/util.cpp').read_text()
start=s.index('int32_t GetTiledOffset2D(')
end=s.index('\n}\n',start)+3
body=s[start:end].replace('rex::align(pitch, xenos::kTextureTileWidthHeight)','((pitch + 31u) & ~31u)')
(a.output/'oracle_tiled_offsets.h').write_text('#include <cstdint>\n'+body)
exe=a.output/'test.exe'
subprocess.run(['clang++','-std=c++20','-O2','-I'+str(a.output.resolve()),
    str(root/'tools/gpu-aot/test_texture_load_gpu.cpp'),'-ld3d12','-ldxgi','-o',str(exe)],check=True)
result = subprocess.run([str(exe.resolve())], check=True, capture_output=True, text=True)
print(result.stdout, end='')
(a.output/'gpu-result.txt').write_text(result.stdout + result.stderr)
(a.output/'verification.json').write_text(json.dumps({
    'passed': True,
    'output': result.stdout.strip(),
    'oracle_headers': {name: hashlib.sha256((root/'rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1'/name).read_bytes()).hexdigest()
        for name in ('texture_load_64bpb_cs.h', 'texture_load_dxt3a_cs.h')},
    'game_launched': False,
    'game_visual_validation': False,
}, indent=2) + '\n')
