"""Configure and run production codegen/DXC/prelink rules with synthetic tools."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def rule(text, output):
    start = text.index('    add_custom_command(\n        OUTPUT "${' + output + '}"')
    end = text.index("VERBATIM)", start) + len("VERBATIM)")
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator", default="Ninja" if os.name == "nt" else "Unix Makefiles")
    args = parser.parse_args()
    if os.name == "nt":
        parser.error("This Linux-to-Windows cross-build fixture requires Unix executable scripts")
    root = Path(__file__).resolve().parents[2]
    native = (root / "rexlego/CMakeLists.txt").read_text()
    sdk_template = root / "rexglue-sdk/resources/templates/init/rexglue_cmake.inja"
    if not sdk_template.exists():
        parser.error("Initialize rexglue-sdk to verify its actual codegen template")
    template = sdk_template.read_text()
    codegen = template[template.index("# A cross-build can reuse"):
                       template.index("# Include DLL module shared library targets")]
    codegen = re.sub(r'\{\{ cmake_var\("([^"\n]+)"\) \}\}',
                     lambda match: "${" + match[1] + "}", codegen)
    codegen = codegen.replace("{{ names.snake_case }}", "legodimensions").replace(
        "{{ entrypoint_out_dir }}", "generated/default")
    assert "{{" not in codegen
    shaders = native[native.index("    set(LEGO_NATIVE_HOST_SHADER_DIR"):
                     native.index("    add_executable(lego_gpu_microcode_index")]
    main_prelink = rule(native, "LEGO_NATIVE_LINKED_SHADER_CACHE")
    runtime_prelink = rule(native, "LEGO_NATIVE_RUNTIME_LINKED_SHADER_CACHE")
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="producers-", dir=args.output.resolve()))
    source = output / "source with spaces"
    source.mkdir()
    shader_dir = source / "src/gpu_native/shaders"
    shader_dir.mkdir(parents=True)
    for shader in ("copy_vs", "copy_color_ps", "resolve_color_ps", "depth_pack_ps", "depth_restore_ps"):
        (shader_dir / (shader + ".hlsl")).write_text("// synthetic shader\n")
    (shader_dir / "copy_common.hlsli").write_text("// synthetic shared shader header\n")
    (source / "legodimensions_manifest.toml").write_text("# synthetic manifest\n")
    (source / "main.cpp").write_text("int main() {}\n")
    (source / "producer.cpp").write_text(r'''
#include <filesystem>
#include <fstream>
#include <string>
int main(int argc,char** argv) {
  if(argc<2)return 2;
  std::filesystem::path output=argv[argc-1];
  bool codegen=std::string(argv[1])=="codegen";
  if(codegen) {
    if(argc!=4||std::string(argv[2])!="--pointer_table_scan")return 3;
    output=output.parent_path()/"generated/default/codegen.build.stamp";
  }
  std::filesystem::create_directories(output.parent_path());
  std::ofstream(output)<<"// synthetic producer output\n";
  if(codegen) {
    std::string path=output.generic_string(),escaped;
    for(char ch:path){if(ch==' ')escaped+='\\';escaped+=ch;}
    std::ofstream(output.parent_path()/"codegen.d")<<escaped<<":\n";
  }
}
''')
    # Executable scripts are host tools; the emulator spy wraps synthetic target
    # tools. No Proton, Windows compiler, SDK library, GPU, or game assets needed.
    (source / "emulator.py").write_text('''
import json,subprocess,sys
with open(sys.argv[1], 'a') as log: log.write(json.dumps(sys.argv[2:])+'\\n')
raise SystemExit(subprocess.run(sys.argv[2:]).returncode)
''')
    (source / "host-codegen").write_text("#!" + sys.executable + "\n" + '''
from pathlib import Path
import sys
assert sys.argv[1:3]==['codegen','--pointer_table_scan']
manifest=Path(sys.argv[3]); assert manifest.is_file()
with (manifest.parent/'host-codegen.calls').open('a') as log: log.write('host\\n')
stamp=manifest.parent/'generated/default/codegen.build.stamp'
stamp.parent.mkdir(parents=True,exist_ok=True);stamp.write_text('// host codegen\\n')
(stamp.parent/'codegen.d').write_text(str(stamp).replace(' ',r'\\ ')+':\\n')
''')
    fake_dxc = "#!" + sys.executable + "\n" + '''
from pathlib import Path
import sys
assert '-T' in sys.argv and '-E' in sys.argv and '-Vn' in sys.argv
assert Path(sys.argv[-1]).is_file()
output=Path(sys.argv[sys.argv.index('-Fh')+1]);output.parent.mkdir(parents=True,exist_ok=True)
output.write_text('// synthetic DXIL header\\n')
with (output.parent/'dxc.calls').open('a') as log:log.write('dxc\\n')
'''
    pinned = source / "dxc package/bin/x64/dxc.exe"
    pinned.parent.mkdir(parents=True)
    pinned.write_text(fake_dxc)
    host_dxc = source / "host-dxc"
    host_dxc.write_text(fake_dxc)
    for path in (source / "host-codegen", pinned, host_dxc):
        path.chmod(0o755)
    cmake = r'''
cmake_minimum_required(VERSION 3.25)
project(CrossBuildProducers LANGUAGES CXX)
if(NOT FIXTURE_NO_EMULATOR)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${FIXTURE_PYTHON};${CMAKE_CURRENT_SOURCE_DIR}/emulator.py;${CMAKE_CURRENT_BINARY_DIR}/emulated.jsonl")
endif()
add_executable(legodimensions EXCLUDE_FROM_ALL main.cpp)
add_executable(rexglue EXCLUDE_FROM_ALL producer.cpp)
add_executable(rex::rexglue ALIAS rexglue)
add_executable(lego_gpu_prelink EXCLUDE_FROM_ALL producer.cpp)
add_executable(lego_gpu_prelink_runtime EXCLUDE_FROM_ALL producer.cpp)
set(LEGO_DXC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/dxc package")
set(LEGO_DXC_ARCH x64)
set(LEGO_NATIVE_LINKED_SHADER_CACHE "${CMAKE_CURRENT_BINARY_DIR}/main-linked.cpp")
set(LEGO_NATIVE_RUNTIME_LINKED_SHADER_CACHE "${CMAKE_CURRENT_BINARY_DIR}/runtime-linked.cpp")
'''
    cmake += "\n".join((codegen, shaders, main_prelink, runtime_prelink)) + r'''
add_custom_target(producers ALL
    DEPENDS "${LEGO_NATIVE_LINKED_SHADER_CACHE}" "${LEGO_NATIVE_RUNTIME_LINKED_SHADER_CACHE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp")
foreach(shader IN ITEMS copy_vs copy_color_ps resolve_color_ps depth_pack_ps depth_restore_ps)
    add_custom_target(producer_${shader} DEPENDS "${LEGO_NATIVE_GENERATED_SHADER_DIR}/${shader}.h")
    add_dependencies(producers producer_${shader})
endforeach()
'''
    (source / "CMakeLists.txt").write_text(cmake)

    def run(command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result

    for name, host_codegen, native_dxc in (("host-cli-pinned-dxc", True, False),
                                         ("host-cli-host-dxc", True, True),
                                         ("target-cli-pinned-dxc", False, False)):
        # Stamp lives in the source directory in the actual generated template.
        for path in (source / "generated/default/codegen.build.stamp", source / "generated/default/codegen.d",
                     source / "host-codegen.calls"):
            path.unlink(missing_ok=True)
        build = output / name
        command = [args.cmake, "-S", str(source), "-B", str(build), "-G", args.generator,
                   "-DCMAKE_CXX_COMPILER=" + args.compiler, "-DCMAKE_SYSTEM_NAME=Generic",
                   "-DCMAKE_BUILD_TYPE=Release", "-DFIXTURE_PYTHON=" + sys.executable]
        if host_codegen:
            command.append("-DREXGLUE_HOST_CODEGEN_EXECUTABLE=" + str(source / "host-codegen"))
        if native_dxc:
            command.append("-DLEGO_HOST_DXC_EXECUTABLE=" + str(host_dxc))
        result = run(command)
        (output / (name + ".configure.log")).write_text(result.stdout + result.stderr)
        assert not (build / "main-linked.cpp").exists()
        result = run([args.cmake, "--build", str(build), "--target", "producers", "--parallel", "2"])
        (output / (name + ".build.log")).write_text(result.stdout + result.stderr)
        for bank in ("main-linked.cpp", "runtime-linked.cpp"):
            assert (build / bank).read_text() == "// synthetic producer output\n"
        assert (source / "generated/default/codegen.build.stamp").exists()
        headers = build / "generated/gpu_native/shaders"
        assert len(list(headers.glob("*.h"))) == 5
        assert (headers / "dxc.calls").read_text().splitlines() == ["dxc"] * 5
        calls = [json.loads(line) for line in (build / "emulated.jsonl").read_text().splitlines()]
        executables = [Path(call[0]).name for call in calls]
        assert sum(name.startswith("lego_gpu_prelink") for name in executables) == 2
        assert executables.count("dxc.exe") == (0 if native_dxc else 5)
        if host_codegen:
            assert not any(name.startswith("rexglue") for name in executables)
            assert not (build / ("rexglue.exe" if os.name == "nt" else "rexglue")).exists()
            assert (source / "host-codegen.calls").read_text().splitlines() == ["host"]
        else:
            assert sum(name.startswith("rexglue") for name in executables) == 1
        before = (build / "emulated.jsonl").read_bytes()
        run([args.cmake, "--build", str(build), "--target", "producers", "--parallel", "2"])
        assert (build / "emulated.jsonl").read_bytes() == before
    # Host codegen and DXC avoid emulating those tools, while both archive
    # prelinkers are still target executables and must retain an emulator.
    command = [args.cmake, "-S", str(source), "-B", str(output / "missing-prelink-emulator"),
               "-G", args.generator, "-DCMAKE_CXX_COMPILER=" + args.compiler,
               "-DCMAKE_SYSTEM_NAME=Generic", "-DFIXTURE_NO_EMULATOR=ON",
               "-DREXGLUE_HOST_CODEGEN_EXECUTABLE=" + str(source / "host-codegen"),
               "-DLEGO_HOST_DXC_EXECUTABLE=" + str(host_dxc)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=20)
    (output / "missing-prelink-emulator.configure.log").write_text(result.stdout + result.stderr)
    assert result.returncode != 0
    assert "Native GPU cross-build prelinkers require" in result.stdout + result.stderr
    print("PASS: production host CLI override, target CLI/prelink emulator prefixes, pinned/host DXC, spaced paths, incremental no-op, missing prelink emulator rejected")


if __name__ == "__main__":
    main()
