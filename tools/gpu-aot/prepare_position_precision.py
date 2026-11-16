"""Preserve vertex position arithmetic across native depth and color passes.

`precise` propagates through the position dependency graph, preventing DXC
reassociation/contraction from making equivalent passes disagree at depth test.
It does not change depth comparison, add a bias, or alter the shared ABI.
Only the replay-validated wind pair is enabled. A shared prepass must not be
changed independently of every material shader that consumes its depth.
"""
from pathlib import Path
import argparse
import shutil


def apply(source):
    path = source / 'shader_recompiler.cpp'
    text = path.read_text()
    old = '        out += "\\tout float4 oPos : SV_Position";'
    new = '''        // Keep precision changes closed over validated depth/color pairs.
        // E669A744921D81E3 is shared by rocks and static foliage; changing it
        // alone makes unchanged material passes fail their depth test.
        const size_t nativeVirtualSize = shaderContainer->virtualSize;
        const size_t nativePhysicalSize = shaderContainer->physicalSize;
        const bool nativeSizeMarker = nativePhysicalSize >= sizeof(uint32_t) &&
            uint32_t(*reinterpret_cast<const be<uint32_t>*>(shaderData + nativeVirtualSize)) == nativePhysicalSize;
        const auto nativePositionHash = XXH3_64bits(shaderData,
            nativeVirtualSize + nativePhysicalSize + (nativeSizeMarker ? sizeof(uint32_t) : 0));
        if (nativePositionHash == 0x1E13090CD0FA6780ull ||
            nativePositionHash == 0xFFA4DD4B364A999Dull)
            out += "\\n#ifdef LEGO_NATIVE_GPU\\n\\tout precise float4 oPos : SV_Position\\n#else\\n\\tout float4 oPos : SV_Position\\n#endif\\n";
        else
            out += "\\tout float4 oPos : SV_Position";'''
    if text.count(old) != 1 or 'nativePositionHash' in text:
        raise ValueError('Expected exactly one vertex position declaration')
    path.write_text(text.replace(old, new))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('scratch', type=Path)
    args = parser.parse_args()
    source = args.scratch / 'source'
    source.mkdir(parents=True, exist_ok=False)
    for path in args.base.iterdir():
        if path.suffix in ('.cpp', '.h') or path.name == 'CMakeLists.txt':
            shutil.copy2(path, source / path.name)
    apply(source)
    print(source)
