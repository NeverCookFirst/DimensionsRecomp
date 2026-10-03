"""Prepare a separate ABI624 compiler for TU23's 0x43F/0x400 viewport modes.

Requires the already tested alpha candidate; does not install shader banks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('alpha_source', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
source = a.output.resolve() / 'source'
source.mkdir(parents=True, exist_ok=True)
hashes = {}
for f in a.alpha_source.iterdir():
    if f.is_file():
        hashes[f.name] = hashlib.sha256(f.read_bytes()).hexdigest()
        shutil.copyfile(f, source/f.name)

common = source/'shader_common.h'
s = common.read_text()
old = '#define LEGO_SHARED_CONSTANTS() uint g_AlphaFunction : packoffset(c37.z);'
assert s.count(old) == 1
s = s.replace(old, '#define LEGO_SHARED_CONSTANTS() float2 g_ViewportScale : packoffset(c37); \\\n    uint g_ViewportMode : packoffset(c37.w); \\\n    uint g_AlphaFunction : packoffset(c37.z);')
old = '#define g_AlphaFunction           vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 600)'
assert s.count(old) == 1
s = s.replace(old, old+'\n#define g_ViewportScale           vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)\n#define g_ViewportMode            vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 604)')
old = 'Texture2D<float4> g_Texture2DDescriptorHeap[]'
assert s.count(old) == 1
s = s.replace(old, '''// TU23 83FB8FD0 writes 0x43F (viewport on) or 0x400 (pixel space).
// Convert homogeneous pixel coordinates to host NDC before the host viewport.
#ifdef LEGO_NATIVE_GPU
float4 LegoViewportPosition(float4 position, float2 scale, uint mode)
{
    if (mode == 1)
        position.xy = position.xy * scale + float2(-1, 1) * position.w;
    return position;
}
#endif

'''+old)
common.write_text(s)
compiler = source/'shader_recompiler.cpp'
s = compiler.read_text()
old = 'out += "\\toPos.xy += g_HalfPixelOffset * oPos.w;\\n";'
assert s.count(old) == 2
s = s.replace(old, 'out += "\\n#ifdef LEGO_NATIVE_GPU\\n\\toPos = LegoViewportPosition(oPos, g_ViewportScale, g_ViewportMode);\\n#endif\\n";\n                        '+old)
compiler.write_text(s)
(a.output/'input-source-hashes.json').write_text(json.dumps(hashes, indent=2)+'\n')
print(source)
