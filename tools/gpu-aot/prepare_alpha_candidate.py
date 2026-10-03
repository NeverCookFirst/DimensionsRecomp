"""Build inputs for an isolated 624-byte, eight-compare native alpha compiler.

Does not install shader archives or touch the explicit-LOD 752-byte candidate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('scratch', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
checkout = root / 'research/reblue/thirdparty/XenosRecomp'
module = checkout / 'XenosRecomp'
source = a.scratch.resolve() / 'source'
source.mkdir(parents=True, exist_ok=True)
manifest = {}
for name in ('constant_table.h', 'dxc_compiler.cpp', 'dxc_compiler.h', 'main.cpp',
             'pch.h', 'shader.h', 'shader_code.h', 'shader_recompiler.cpp',
             'shader_recompiler.h', 'shader_common.h'):
    path = module / name
    manifest[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    shutil.copyfile(path, source / name)

def replace_once(s, old, new):
    assert s.count(old) == 1, old
    return s.replace(old, new)

s = (source / 'shader_recompiler.cpp').read_text()
s = replace_once(s, 'exportRegister = "oC0";',
    'exportRegister = "oC0";\n                exportsColor0 = true;')
s = replace_once(s, 'out += "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)";',
    'out += "\\n#if !defined(LEGO_NATIVE_GPU) || LEGO_SHADER_WRITES_COLOR0\\n";\n'
    '                    out += "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)";')
s = replace_once(s, 'out += "\\n#ifdef LEGO_NATIVE_GPU\\n";',
    'out += "\\n#endif\\n#ifdef LEGO_NATIVE_GPU\\n";')
# The export flag is known only after the complete control flow has been
# decoded. Gate all return sites, including those preceding later exports.
pos = s.rfind('\n}')
assert pos > 0
s = s[:pos] + ('\n    out.insert(0, exportsColor0 ? "#define LEGO_SHADER_WRITES_COLOR0 1\\n" : '
               '"#define LEGO_SHADER_WRITES_COLOR0 0\\n");\n') + s[pos:]
s = replace_once(s, 'out += "\\tclip(oC0.w - g_AlphaThreshold);\\n";',
    'out += "\\n#ifdef LEGO_NATIVE_GPU\\n";\n'
    '                    out += "\\tif (!LegoAlphaPass(oC0.w, g_AlphaThreshold, g_AlphaFunction)) discard;\\n";\n'
    '                    out += "#else\\n\\tclip(oC0.w - g_AlphaThreshold);\\n#endif\\n";')
(source / 'shader_recompiler.cpp').write_text(s)
s = (source / 'shader_recompiler.h').read_text()
s = replace_once(s, '    bool exportsDepth = false;',
    '    bool exportsDepth = false;\n    bool exportsColor0 = false;')
(source / 'shader_recompiler.h').write_text(s)
s = (source / 'shader_common.h').read_text()
s = replace_once(s,
    '#define g_ColorOutputScale        vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608)',
    '#define g_ColorOutputScale        vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608)\n'
    '#define g_AlphaFunction           vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 600)')
s = replace_once(s,
    '#define LEGO_SHARED_CONSTANTS() float4 g_ColorOutputScale : packoffset(c38);',
    '#define LEGO_SHARED_CONSTANTS() uint g_AlphaFunction : packoffset(c37.z); \\\n'
    '    float4 g_ColorOutputScale : packoffset(c38);')
s = replace_once(s, 'Texture2D<float4> g_Texture2DDescriptorHeap[]',
    '#ifdef LEGO_NATIVE_GPU\n' + (root / 'tools/gpu-aot/alpha_compare.hlsli').read_text() +
    '\n#endif\n\nTexture2D<float4> g_Texture2DDescriptorHeap[]')
(source / 'shader_common.h').write_text(s)
dep = checkout / 'thirdparty'
old_build = checkout / 'out/build/lego-release'
cmake = '''cmake_minimum_required(VERSION 3.25)
project(LegoAlphaCandidate LANGUAGES CXX)
add_executable(alpha_compiler main.cpp dxc_compiler.cpp shader_recompiler.cpp "@SMOL@/smolv.cpp")
target_compile_features(alpha_compiler PRIVATE cxx_std_17)
target_compile_definitions(alpha_compiler PRIVATE REBLUE_RECOMP XENOS_RECOMP_DXIL FMT_SHARED _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(alpha_compiler PRIVATE -fms-extensions -D_DLL -D_MT -Xclang --dependent-lib=msvcrt)
target_include_directories(alpha_compiler PRIVATE "@SMOL@" "@DEP@/xxHash" "@DEP@/zstd/lib" "@DEP@/fmt/include" "@DEP@/dxc-bin/inc")
target_precompile_headers(alpha_compiler PRIVATE pch.h)
target_link_libraries(alpha_compiler PRIVATE "@DEP@/dxc-bin/lib/x64/dxcompiler.lib" "@BUILD@/thirdparty/xxHash/cmake_unofficial/xxhash.lib" "@BUILD@/thirdparty/zstd/build/cmake/lib/zstd_static.lib" "@BUILD@/thirdparty/fmt/fmt.lib")
add_custom_command(TARGET alpha_compiler POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "@DEP@/dxc-bin/bin/x64/dxcompiler.dll" "@DEP@/dxc-bin/bin/x64/dxil.dll" "@BUILD@/thirdparty/fmt/bin/fmt.dll" $<TARGET_FILE_DIR:alpha_compiler>)
'''
cmake = cmake.replace('@SMOL@', (dep / 'smol-v/source').as_posix())
cmake = cmake.replace('@DEP@', dep.as_posix()).replace('@BUILD@', old_build.as_posix())
(source / 'CMakeLists.txt').write_text(cmake)
(a.scratch / 'original-source-hashes.json').write_text(json.dumps(manifest, indent=2))
print(source)
