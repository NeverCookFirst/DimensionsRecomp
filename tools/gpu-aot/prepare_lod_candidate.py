"""Prepare an isolated explicit-LOD compiler and ABI candidate, not a live fix.

Copies only the XenosRecomp module sources; dependencies use the existing local
build. All outputs stay under scratch. Does not change the compiler checkout,
native source, runtime shader bank, config, or game executable.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f'Expected one source anchor: {old[:100]!r}')
    return source.replace(old, new)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scratch', type=Path)
    parser.add_argument('--base-source', type=Path, help='Preserve an already validated alpha/viewport compiler source')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    checkout = root / 'research/reblue/thirdparty/XenosRecomp'
    module = args.base_source.resolve() if args.base_source else checkout / 'XenosRecomp'
    source_dir = args.scratch.resolve() / 'source'
    source_dir.mkdir(parents=True, exist_ok=True)
    manifest = {}
    for name in ('constant_table.h', 'dxc_compiler.cpp', 'dxc_compiler.h',
                 'main.cpp', 'pch.h', 'shader.h', 'shader_code.h',
                 'shader_recompiler.cpp', 'shader_recompiler.h', 'shader_common.h'):
        path = module / name
        manifest[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
        shutil.copyfile(path, source_dir / name)
    compiler = (source_dir / 'shader_recompiler.cpp').read_text()
    compiler = replace_once(compiler,
        'if (instr.opcode != FetchOpcode::TextureFetch && instr.opcode != FetchOpcode::GetTextureWeights)\n        return;',
        'if (instr.opcode != FetchOpcode::TextureFetch && instr.opcode != FetchOpcode::GetTextureWeights &&\n'
        '        instr.opcode != FetchOpcode::SetTextureLod)\n        return;')
    # The same register LOD state is shared by all texture units. It is updated
    # under instruction predication, not under the predicate of the later fetch.
    compiler = replace_once(compiler, '    std::string constName;\n', '''    if (instr.opcode == FetchOpcode::SetTextureLod)
    {
        if (instr.srcRegisterAm)
            throw std::runtime_error("Explicit LOD candidate: relative setTexLOD unsupported");
        indent();
        out += "textureLod = ";
        printSrcRegister(1);
        out += ";\\n";
        if (instr.isPredicated)
        {
            --indentation;
            indent();
            out += "}\\n";
        }
        return;
    }
    const bool computedLod = instr.useCompLod && (isPixelShader || instr.useRegGradients);
    const bool explicitLod = instr.opcode == FetchOpcode::TextureFetch && !computedLod;

    std::string constName;
''')
    compiler = replace_once(compiler,
        '        out += isPixelShader ? "tfetch" : "tfetchVertex";',
        '        out += explicitLod ? "tfetchLevel" : (isPixelShader ? "tfetch" : "tfetchVertex");')
    compiler = replace_once(compiler, '    out += ").";\n', '''    if (explicitLod)
    {
        // Instruction bias is signed fixed point with four fractional bits.
        print(", {} + g_FetchLodBias({}) + {}", instr.useRegLod ? "textureLod" : "0.0",
            uint32_t(instr.constIndex), float(instr.lodBias) / 16.0f);
    }
    out += ").";
''')
    compiler = replace_once(compiler,
        '    // Xenos exposes 64 temporary vector registers and 16 integer constants.\n',
        '    out += "\\tfloat textureLod = 0.0;\\n";\n\n'
        '    // Xenos exposes 64 temporary vector registers and 16 integer constants.\n')
    (source_dir / 'shader_recompiler.cpp').write_text(compiler)

    common = (source_dir / 'shader_common.h').read_text()
    common = replace_once(common,
        '#define g_ColorOutputScale        vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608)',
        '#define g_ColorOutputScale        vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 608)\n'
        '#define g_FetchLodBias(i)          vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 624 + (i)*4)')
    color_anchor = ('#define LEGO_SHARED_CONSTANTS() float4 g_ColorOutputScale : packoffset(c38);'
                    if not args.base_source else '    float4 g_ColorOutputScale : packoffset(c38);')
    common = replace_once(common,
        color_anchor,
        color_anchor + ' \\\n'
        '    float4 g_FetchLodBiasArr[8] : packoffset(c39);\n'
        '#define g_FetchLodBias(i) (g_FetchLodBiasArr[(i) / 4][(i) % 4])')
    common = replace_once(common, 'float4 tfetchVertex2D(', '''float4 tfetchLevel2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float lod)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture), lod);
}

float4 tfetchVertex2D(''')
    common = replace_once(common, 'float4 tfetchVertex3D(', '''float4 tfetchLevel3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, float lod)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord, lod);
}

float4 tfetchVertex3D(''')
    common = replace_once(common, 'float4 tfetchVertexCube(', '''float4 tfetchLevelCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData, float lod)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z], lod);
}

float4 tfetchVertexCube(''')
    (source_dir / 'shader_common.h').write_text(common)

    # This ABI change must accompany the candidate shaders at eventual runtime.
    # Keep it as an isolated source copy until the sampler-only A/B is observed.
    draw = (root / 'rexlego/src/gpu_native/draw.cpp').read_text()
    if 'float fetch_lod_bias[32]' not in draw:
        draw = replace_once(draw, '  float color_output_scale[4]{1, 1, 1, 1};\n',
                            '  float color_output_scale[4]{1, 1, 1, 1};\n  float fetch_lod_bias[32]{};\n')
        draw = replace_once(draw, 'static_assert(sizeof(SharedConstants) == 624);',
                            'static_assert(sizeof(SharedConstants) == 752);\n'
                            'static_assert(offsetof(SharedConstants, fetch_lod_bias) == 624);')
        draw = replace_once(draw, '    shared.samplers[i] = HostDevice::RegisterSampler(fetch);',
                            '    shared.samplers[i] = HostDevice::RegisterSampler(fetch);\n'
                            '    const int32_t lod_bias = int32_t((fetch[4] >> 12) & 1023);\n'
                            '    shared.fetch_lod_bias[i] = float(lod_bias >= 512 ? lod_bias - 1024 : lod_bias) / 32.0f;')
    else:
        assert 'sizeof(SharedConstants) == 752' in draw
        assert 'offsetof(SharedConstants, fetch_lod_bias) == 624' in draw
    draw = draw.replace('shared_bytes=624', 'shared_bytes=752')
    (source_dir / 'draw-candidate.cpp').write_text(draw)

    dep = checkout / 'thirdparty'
    old_build = checkout / 'out/build/lego-release'
    cmake = '''cmake_minimum_required(VERSION 3.25)
project(LegoExplicitLodCandidate LANGUAGES CXX)
add_executable(lod_compiler main.cpp dxc_compiler.cpp shader_recompiler.cpp "@SMOL@/smolv.cpp")
target_compile_features(lod_compiler PRIVATE cxx_std_17)
target_compile_definitions(lod_compiler PRIVATE REBLUE_RECOMP XENOS_RECOMP_DXIL FMT_SHARED _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(lod_compiler PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions -D_DLL -D_MT -Xclang --dependent-lib=msvcrt)
target_include_directories(lod_compiler PRIVATE "@SMOL@" "@DEP@/xxHash" "@DEP@/zstd/lib" "@DEP@/fmt/include" "@DEP@/dxc-bin/inc")
target_precompile_headers(lod_compiler PRIVATE pch.h)
target_link_libraries(lod_compiler PRIVATE "@DEP@/dxc-bin/lib/x64/dxcompiler.lib" "@BUILD@/thirdparty/xxHash/cmake_unofficial/xxhash.lib" "@BUILD@/thirdparty/zstd/build/cmake/lib/zstd_static.lib" "@BUILD@/thirdparty/fmt/fmt.lib")
add_custom_command(TARGET lod_compiler POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "@DEP@/dxc-bin/bin/x64/dxcompiler.dll" "@DEP@/dxc-bin/bin/x64/dxil.dll" "@BUILD@/thirdparty/fmt/bin/fmt.dll" $<TARGET_FILE_DIR:lod_compiler>)
'''
    cmake = cmake.replace('@SMOL@', (dep / 'smol-v/source').as_posix())
    cmake = cmake.replace('@DEP@', dep.as_posix()).replace('@BUILD@', old_build.as_posix())
    (source_dir / 'CMakeLists.txt').write_text(cmake)
    (source_dir / 'README.txt').write_text('''ISOLATED CANDIDATE — NOT APPLIED TO NATIVE.
Compile with XENOS_RECOMP_LEGO_NATIVE_SCALE=1. New HLSL requires an extra
32 float fetch biases at SharedConstants offset624, total752 bytes.
draw-candidate.cpp shows the matching upload change. Do not replace the live
shader cache without this ABI update. Current sampler-only executable and
archives remain unchanged. Supports explicit LOD on currently supported
2D/3D/cube helpers; relative setTexLOD, explicit gradients, instruction filter
overrides, texcoord-denorm, and 1D helper coverage remain separate limitations.
''')
    (args.scratch / 'original-source-hashes.json').write_text(json.dumps(manifest, indent=2))
    print(f'Isolated LOD candidate prepared: {source_dir}')


if __name__ == '__main__':
    main()
