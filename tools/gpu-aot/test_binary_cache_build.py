"""Build actual native binary-cache CMake rules with a tiny Windows COFF bank."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--generator', default='Ninja' if os.name == 'nt' else 'Unix Makefiles')
    parser.add_argument('--source-root', type=Path,
                        default=Path(__file__).resolve().parents[2],
                        help='Production source root, or an explicit isolated review snapshot')
    for tool in ('mc', 'readobj', 'objcopy', 'ar', 'ranlib'):
        parser.add_argument('--llvm-' + tool)
    args = parser.parse_args()
    root = args.source_root.resolve()
    text = (root / 'rexlego/CMakeLists.txt').read_text()
    start = text.index('    add_custom_target(lego_main_linked_cache')
    end = text.index('    set(LEGO_NATIVE_RUNTIME_LINKED_SHADER_CACHE', start)
    integration = text[start:end]
    dependency_start = text.index('    add_dependencies(lego_native_gpu_fingerprint\n')
    dependency_end = text.index(')', dependency_start) + 1
    fingerprint_order = text[dependency_start:dependency_end]
    tools = {name: getattr(args, name.replace('-', '_')) or shutil.which(name)
             for name in ('llvm-mc', 'llvm-readobj', 'llvm-objcopy', 'llvm-ar', 'llvm-ranlib')}
    assert all(tools.values()), 'host LLVM assembler/object/archive tools are required'
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='fixture with spaces ', dir=args.output.resolve()))
    source = output / 'source with spaces/rexlego'
    build = output / 'build with spaces'
    source.mkdir(parents=True)
    script = source.parent / 'tools/gpu-aot/embed_linked_bank.py'
    script.parent.mkdir(parents=True)
    shutil.copyfile(root / 'tools/gpu-aot/embed_linked_bank.py', script)
    include = source / 'include'
    header = include / 'gpu/shaders/linked_shader_cache.h'
    header.parent.mkdir(parents=True)
    shutil.copyfile(root / 'rexlego/src/gpu_native/linked_shader_cache.h', header)
    # The actual ABI header is compiled unchanged. These minimal platform
    # standard-header shims remove any dependency on a Windows SDK installation.
    (include / 'cstddef').write_text('#pragma once\nusing size_t=__SIZE_TYPE__;\n')
    (include / 'cstdint').write_text('#pragma once\nusing uint8_t=unsigned char;\n'
                                   'using uint32_t=unsigned int;\nusing uint64_t=unsigned long long;\n')
    (source / 'probe.cpp').write_text('#include "gpu/shaders/linked_shader_cache.h"\n'
                                    'const uint8_t* bank_address(){return g_compressedLinkedDxilCache;}\n')
    (source / 'copy-bank.cmake').write_text('''
file(APPEND "${OUTPUT}.calls" "built\\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${INPUT}" "${OUTPUT}"
    COMMAND_ERROR_IS_FATAL ANY)
''')
    raw = source / 'main-raw.cpp'
    linked = source / 'main-linked.cpp'

    def bank(version):
        expanded = bytes(range(256))[::version] * 4
        payload = zlib.compress(expanded)
        prefix = ('#include "gpu/shaders/linked_shader_cache.h"\n'
                  'LinkedShaderCacheEntry g_linkedShaderCacheEntries[] = {\n'
                  f'{{ 0x1234, 0, 0, {len(expanded)} }},\n}};\n'
                  'const size_t g_linkedShaderCacheEntryCount = 1;\n')
        footer = (f'const size_t g_linkedDxilCacheCompressedSize = {len(payload)};\n'
                  f'const size_t g_linkedDxilCacheDecompressedSize = {len(expanded)};\n')
        body = ''.join(','.join(str(value) for value in payload[at:at+32]) + ',\n'
                       for at in range(0, len(payload), 32))
        raw.write_text(prefix + 'const uint8_t g_compressedLinkedDxilCache[] = {\n' + body + '};\n' + footer)
        return payload, (prefix + footer).encode()

    payload, metadata = bank(1)
    cmake = '''cmake_minimum_required(VERSION 3.25)
project(BinaryCacheFixture LANGUAGES CXX)
if(DEFINED FIXTURE_ARCHITECTURE)
    set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "${FIXTURE_ARCHITECTURE}")
endif()
if(DEFINED FIXTURE_POINTER_BYTES)
    set(CMAKE_CXX_SIZEOF_DATA_PTR "${FIXTURE_POINTER_BYTES}")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_library(legodimensions STATIC probe.cpp)
target_include_directories(legodimensions PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(LEGO_NATIVE_LINKED_SHADER_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/main-linked.cpp")
add_custom_command(OUTPUT "${LEGO_NATIVE_LINKED_SHADER_CACHE}"
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/main-raw.cpp"
        "-DOUTPUT=${LEGO_NATIVE_LINKED_SHADER_CACHE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/copy-bank.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/main-raw.cpp"
    VERBATIM)
'''
    after = '''
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fingerprint.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${LEGO_NATIVE_LINKED_SHADER_CACHE}"
        "${CMAKE_CURRENT_BINARY_DIR}/fingerprint.txt"
    DEPENDS "${LEGO_NATIVE_LINKED_SHADER_CACHE}"
    VERBATIM)
add_custom_target(lego_native_gpu_fingerprint
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/fingerprint.txt")
add_custom_target(lego_runtime_linked_cache)
add_dependencies(legodimensions lego_native_gpu_fingerprint)
get_target_property(fixture_sources legodimensions SOURCES)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sources.txt" "${fixture_sources}")
'''
    (source / 'CMakeLists.txt').write_text(cmake + integration + after + fingerprint_order)
    configure = [args.cmake, '-S', str(source), '-B', str(build), '-G', args.generator,
                 '-DCMAKE_SYSTEM_NAME=Windows', '-DCMAKE_SYSTEM_PROCESSOR=AMD64',
                 '-DCMAKE_CXX_COMPILER=' + args.compiler,
                 '-DCMAKE_CXX_COMPILER_TARGET=x86_64-pc-windows-msvc',
                 '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
                 '-DCMAKE_CXX_FLAGS=-nostdinc++', '-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_AR=' + tools['llvm-ar'], '-DCMAKE_RANLIB=' + tools['llvm-ranlib'],
                 '-DLEGO_HOST_LLVM_MC=' + tools['llvm-mc']]

    def run(command, success=True):
        result = subprocess.run(command, text=True, capture_output=True, timeout=30)
        with (output / 'commands.log').open('a') as log:
            log.write(json.dumps(command) + '\n' + result.stdout + result.stderr)
        assert (result.returncode == 0) == success, result.stdout + result.stderr
        return result

    run(configure)
    assert not linked.exists(), 'configure must not run the bank producer'
    assert 'linked-bank-metadata.cpp' in (build / 'sources.txt').read_text()
    assert 'linked-bank.obj' in (build / 'sources.txt').read_text()
    assert str(linked) not in (build / 'sources.txt').read_text()
    artifacts = build / 'generated/gpu_native/linked-bank'
    archive = build / 'legodimensions.lib'
    calls = Path(str(linked) + '.calls')

    def verify(expected_payload, expected_metadata, expected_calls, target_build=None,
               machine='AMD64', target='x86_64-pc-windows-msvc'):
        current = target_build or build
        current_artifacts = current / 'generated/gpu_native/linked-bank'
        current_archive = current / 'legodimensions.lib'
        run([args.cmake, '--build', str(current), '--target', 'legodimensions', '--parallel', '1'])
        assert calls.read_text().splitlines() == ['built'] * expected_calls
        assert (current / 'fingerprint.txt').read_bytes() == linked.read_bytes() == raw.read_bytes()
        assert (current_artifacts / 'linked-bank.bin').read_bytes() == expected_payload
        assert (current_artifacts / 'linked-bank-metadata.cpp').read_bytes() == expected_metadata
        manifest = json.loads((current_artifacts / 'linked-bank-manifest.json').read_text())
        assert manifest['target'] == target
        assert manifest['compressed_payload']['bytes'] == len(expected_payload)
        assert manifest['compressed_payload']['sha256'] == hashlib.sha256(expected_payload).hexdigest()
        assert not manifest['implicit_string_nul_embedded']
        symbols = run([tools['llvm-readobj'], '--file-headers', '--symbols',
                       str(current_artifacts / 'linked-bank.obj')]).stdout
        assert 'IMAGE_FILE_MACHINE_' + machine in symbols
        assert '?g_compressedLinkedDxilCache@@3QBEB' in symbols
        metadata_objects = list(current.rglob('linked-bank-metadata.cpp.obj'))
        assert len(metadata_objects) == 1
        metadata_headers = run([tools['llvm-readobj'], '--file-headers',
                                str(metadata_objects[0])]).stdout
        assert 'IMAGE_FILE_MACHINE_' + machine in metadata_headers
        extracted = current_artifacts / 'extracted-rdata.bin'
        run([tools['llvm-objcopy'], '--dump-section', '.rdata=' + str(extracted),
             str(current_artifacts / 'linked-bank.obj'),
             str(current_artifacts / 'inspection-copy.obj')])
        assert extracted.read_bytes() == expected_payload
        # Actual target archiver must consume the generated external object,
        # not merely produce independent files that never reach the target.
        members = run([tools['llvm-ar'], 't', str(current_archive)]).stdout
        assert 'linked-bank.obj' in members and 'linked-bank-metadata.cpp.obj' in members

    verify(payload, metadata, 1)
    tracked = [archive, artifacts / 'linked-bank.obj', artifacts / 'linked-bank-metadata.cpp',
               build / 'fingerprint.txt']
    stamps = [path.stat().st_mtime_ns for path in tracked]
    verify(payload, metadata, 1)
    assert [path.stat().st_mtime_ns for path in tracked] == stamps, 'unchanged build must be a no-op'
    time.sleep(1.1)  # Make timestamp resolution; no compiler runs during this wait.
    payload, metadata = bank(2)
    verify(payload, metadata, 2)
    assert archive.stat().st_mtime_ns != stamps[0]
    (artifacts / 'linked-bank.obj').unlink()
    verify(payload, metadata, 2)

    # Target architecture comes from compiler detection, independently of the
    # host CPU. A spoofed/unknown system processor must not reject proven x64.
    wrong_host = output / 'x64 unknown host'
    command = list(configure)
    command[command.index('-B') + 1] = str(wrong_host)
    run(command + ['-DCMAKE_SYSTEM_PROCESSOR=unknown', '-DLEGO_NATIVE_BINARY_CACHE=ON'])
    verify(payload, metadata, 2, target_build=wrong_host)

    # An actual ARM64 compiler target on an AMD64 host reproduces the old
    # architecture-selection error. Inspect both real COFF producers, not
    # merely configure variables or the assembly text.
    compiler_targets = run([args.compiler, '--print-targets']).stdout
    assembler_targets = run([tools['llvm-mc'], '--version']).stdout
    arm64_tested = 'aarch64' in compiler_targets and 'aarch64' in assembler_targets
    if arm64_tested:
        arm64 = output / 'arm64 target amd64 host'
        command = list(configure)
        command[command.index('-B') + 1] = str(arm64)
        run(command + ['-DCMAKE_CXX_COMPILER_TARGET=aarch64-pc-windows-msvc',
                       '-DCMAKE_SYSTEM_PROCESSOR=AMD64', '-DLEGO_NATIVE_BINARY_CACHE=ON'])
        verify(payload, metadata, 2, target_build=arm64,
               machine='ARM64', target='aarch64-pc-windows-msvc')
    else:
        print('UNTESTED: supplied compiler/assembler do not both advertise AArch64')

    # Actual i686 detection and controlled unsupported compiler-attribute
    # cases exercise the exact production gate; host CPU names cannot mask
    # ARM64EC/X86/unknown architectures or a non-64-bit pointer ABI.
    for name, arguments in (
            ('missing tools', ['-DLEGO_HOST_LLVM_MC=' + str(output / 'absent llvm-mc')]),
            ('actual x86 target', ['-DCMAKE_CXX_COMPILER_TARGET=i686-pc-windows-msvc']),
            ('arm64ec attribute', ['-DFIXTURE_ARCHITECTURE=ARM64EC']),
            ('x86 attribute', ['-DFIXTURE_ARCHITECTURE=X86']),
            ('unknown attribute', ['-DFIXTURE_ARCHITECTURE=']),
            ('wrong pointer ABI', ['-DFIXTURE_POINTER_BYTES=4'])):
        target = output / name
        command = list(configure)
        command[command.index('-B') + 1] = str(target)
        run(command + ['-DLEGO_NATIVE_BINARY_CACHE=ON'] + arguments, success=False)

    # Direct production-parser cases cover both accepted source formats,
    # branch mismatch and failed assembly without running DXC/prelinking.
    parser_source = output / 'parser input.cpp'
    parser_output = output / 'parser outputs'
    declaration = 'const uint8_t g_compressedLinkedDxilCache[] = {\n'
    prefix, remainder = raw.read_text().split(declaration, 1)
    numeric, footer = remainder.split('};\n', 1)
    literals = ''.join('"' + ''.join('\\x%02X' % byte for byte in payload[at:at+256]) + '"\n'
                       for at in range(0, len(payload), 256))
    dual = (prefix + 'const uint8_t g_compressedLinkedDxilCache[] =\n'
            '#if defined(_MSC_VER) && !defined(__clang__)\n{\n' + numeric + '}\n#else\n'
            '#if defined(__clang__)\n#pragma clang diagnostic push\n'
            '#pragma clang diagnostic ignored "-Woverlength-strings"\n#endif\n' + literals +
            '#if defined(__clang__)\n#pragma clang diagnostic pop\n#endif\n#endif\n;\n' + footer)
    invoke = [sys.executable, str(script), str(parser_source), str(parser_output),
              '--llvm-mc', tools['llvm-mc']]
    for label, contents in (('numeric', raw.read_text()), ('numeric-with-literal-fallback', dual)):
        parser_source.write_text(contents)
        run(invoke)
        record = json.loads((parser_output / 'linked-bank-manifest.json').read_text())
        assert record['input_format'] == label
        assert record['numeric_and_literal_identical'] == (label != 'numeric')
        assert (parser_output / 'linked-bank.bin').read_bytes() == payload
        assert (parser_output / 'linked-bank-metadata.cpp').read_bytes() == metadata
        extracted = parser_output / 'inspection.bin'
        run([tools['llvm-objcopy'], '--dump-section', '.rdata=' + str(extracted),
             str(parser_output / 'linked-bank.obj'), str(parser_output / 'inspection.obj')])
        assert extracted.read_bytes() == payload

    output_names = ('linked-bank-metadata.cpp', 'linked-bank.bin', 'linked-bank.s',
                    'linked-bank.obj', 'linked-bank-manifest.json')
    def prior_outputs():
        return {name: (hashlib.sha256((parser_output / name).read_bytes()).hexdigest(),
                       (parser_output / name).stat().st_mtime_ns) for name in output_names}
    preserved = prior_outputs()
    for invalid in (dual.replace('\\x%02X' % payload[0], '\\x%02X' % (payload[0] ^ 1), 1),
                    raw.read_text().replace(declaration, declaration + '256,\n', 1),
                    raw.read_text().replace('g_linkedDxilCacheCompressedSize = %d;' % len(payload),
                                           'g_linkedDxilCacheCompressedSize = %d;' % (len(payload)+1))):
        parser_source.write_text(invalid)
        run(invoke, success=False)
        assert prior_outputs() == preserved, 'parser failure modified published outputs'
    parser_source.write_text(dual)
    failed_assembler = list(invoke)
    # Python rejects llvm-mc's -triple option on every host; this is a real
    # nonzero subprocess result, without shell scripts or platform launchers.
    failed_assembler[-1] = sys.executable
    run(failed_assembler, success=False)
    assert prior_outputs() == preserved, 'assembler failure modified published outputs'
    assert not list(parser_output.glob('.linked-bank-stage-*')), 'failed staging directory leaked'
    print('PASS: real binary-cache CMake rules produce byte-exact MSVC ABI COFF and metadata; '
          'Make consumers share one producer, target archives external object, unchanged no-op, '
          'input/object changes rebuild, compiler architecture/pointer ABI checked; '
          'numeric/dual parser and failed publication preserved all prior outputs; '
          'ARM64 target verified=' + str(arm64_tested))


if __name__ == '__main__':
    main()
