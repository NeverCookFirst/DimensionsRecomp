"""Add uniform native raster-scale correction to a prepared native compiler.

The C++ host's formerly unused byte588 is a float inverse raster scale. ABI624
and all existing offsets are retained. Non-native output is unchanged.
"""
from pathlib import Path
import argparse
import shutil

def apply(source):
    common=source/'shader_common.h';s=common.read_text()
    old='#define g_ViewportMode            vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 604)'
    assert s.count(old)==1
    s=s.replace(old,old+'\n#define g_PixelPositionScale      vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 588)')
    old='#define LEGO_SHARED_CONSTANTS() float2 g_ViewportScale : packoffset(c37);'
    assert s.count(old)==1
    s=s.replace(old,'#define LEGO_SHARED_CONSTANTS() float g_PixelPositionScale : packoffset(c36.w); \\\n    float2 g_ViewportScale : packoffset(c37);')
    common.write_text(s)
    compiler=source/'shader_recompiler.cpp';s=compiler.read_text()
    old='                out += "float4((iPos.xy - 0.5) * float2(iFace ? 1.0 : -1.0, 1.0), 0.0, 0.0);\\n";'
    assert s.count(old)==1
    s=s.replace(old,'''                out += "float4((iPos.xy - 0.5) * float2(iFace ? 1.0 : -1.0, 1.0)";
                // Xenos param-gen positions are guest pixels, even when the
                // host raster target is larger. Revert scale after removing
                // the host half pixel, as in the SDK's DXBC translator.
                out += "\\n#ifdef LEGO_NATIVE_GPU\\n * (g_PixelPositionScale > 0.0 ? g_PixelPositionScale : 1.0)\\n#endif\\n";
                out += ", 0.0, 0.0);\\n";''')
    compiler.write_text(s)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('base',type=Path);p.add_argument('scratch',type=Path);a=p.parse_args()
    source=a.scratch/'source';source.mkdir(parents=True,exist_ok=False)
    for path in a.base.iterdir():
        if path.suffix in ('.cpp','.h') or path.name=='CMakeLists.txt':shutil.copy2(path,source/path.name)
    apply(source)
    print(source)
