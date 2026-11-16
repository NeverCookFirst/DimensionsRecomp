"""Build the production native fingerprint rule against synthetic shader banks."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator", default="Ninja" if os.name == "nt" else "Unix Makefiles")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    text = (root / "rexlego/CMakeLists.txt").read_text()
    begin = text.index("    # Fingerprint the exact native renderer inputs")
    end = text.index("\nendif()", begin)
    production = text[begin:end]
    output = args.output.resolve()
    # The test needs a fresh tree: generated archives must be absent at configure.
    output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="fixture-", dir=output))
    source = output / "source/rexlego"
    build = output / "build"
    source.mkdir(parents=True)
    for header in (root / "rexlego/src/gpu_native").rglob("*.h"):
        path = source / "src/gpu_native" / header.relative_to(root / "rexlego/src/gpu_native")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("// synthetic native helper header\n")
    input_files = []
    for relative in re.findall(r'"\$\{CMAKE_CURRENT_SOURCE_DIR\}/([^"\n]+)"', production):
        if "*" in relative:
            continue
        path = (source / relative).resolve()
        if path in input_files or path.suffix not in (".cpp", ".h", ".hlsl", ".hlsli", ".in", ".py", ".patch", ".toml"):
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("// synthetic fingerprint input\n")
        input_files.append(path)
    template = source / "src/gpu_native/build_info.h.in"
    template.write_text((root / "rexlego/src/gpu_native/build_info.h.in").read_text())
    raw = source / "main-raw.cpp"
    runtime = source / "runtime-raw.cpp"
    microcode_index = source / "main-index.cpp"
    linked = source / "main-linked.cpp"
    runtime_linked = source / "runtime-linked.cpp"
    raw.write_text("main archive v1\n")
    runtime.write_text("runtime archive v1\n")
    microcode_index.write_text("microcode lookup index v1\n")
    (source / "main.cpp").write_text(
        '#include "native_gpu_build_info.h"\n#include <cstdio>\n'
        'int main(){std::puts(legodimensions::gpu_native::kNativeGpuBuildFingerprint);}\n')
    (source / "copy-bank.cmake").write_text('''
file(APPEND "${OUTPUT}.calls" "built\\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${INPUT}" "${OUTPUT}"
    COMMAND_ERROR_IS_FATAL ANY)
''')
    cmake = '''cmake_minimum_required(VERSION 3.25)
project(NativeFingerprintFixture LANGUAGES CXX)
add_executable(legodimensions main.cpp)
set(LEGO_NATIVE_SHADER_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/main-raw.cpp")
set(LEGO_NATIVE_MICROCODE_INDEX "${CMAKE_CURRENT_SOURCE_DIR}/main-index.cpp")
set(LEGO_NATIVE_RUNTIME_SHADER_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/runtime-raw.cpp")
set(LEGO_NATIVE_LINKED_SHADER_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/main-linked.cpp")
set(LEGO_NATIVE_RUNTIME_LINKED_SHADER_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/runtime-linked.cpp")
foreach(pair IN ITEMS "main" "runtime")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/${pair}-linked.cpp"
        COMMAND ${CMAKE_COMMAND}
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/${pair}-raw.cpp"
            "-DOUTPUT=${CMAKE_CURRENT_SOURCE_DIR}/${pair}-linked.cpp"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/copy-bank.cmake"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${pair}-raw.cpp"
        VERBATIM)
endforeach()
add_custom_target(lego_runtime_linked_cache DEPENDS "${LEGO_NATIVE_RUNTIME_LINKED_SHADER_CACHE}")
add_custom_target(lego_main_linked_cache DEPENDS "${LEGO_NATIVE_LINKED_SHADER_CACHE}")
add_dependencies(legodimensions lego_runtime_linked_cache)
'''
    bank = source / "temporal-bank"
    bank.mkdir()
    (bank / "temporal-bank.cpp").write_text("synthetic temporal metadata v1\n")
    (bank / "temporal-bank.obj").write_text("synthetic temporal shader object v1\n")
    cmake += 'set(LEGO_TEMPORAL_SHADER_BANK "${CMAKE_CURRENT_SOURCE_DIR}/temporal-bank")\n'
    typed_bank = source / "float-blend-indices-bank"
    typed_bank.mkdir()
    (typed_bank / "float-blend-indices-bank.cpp").write_text("synthetic typed metadata v1\n")
    (typed_bank / "float-blend-indices-bank.bin").write_text("synthetic typed shader bytes v1\n")
    (typed_bank / "float-blend-indices-bank.obj").write_text("synthetic typed object v1\n")
    cmake += 'set(LEGO_FLOAT_BLEND_INDICES_BANK "${CMAKE_CURRENT_SOURCE_DIR}/float-blend-indices-bank")\n'
    selected_texture = source / "src/gpu_native/textures-selected.cpp"
    selected_texture.write_text("// selected texture implementation v1\n")
    unselected_texture = source / "src/gpu_native/textures.cpp"
    unselected_texture.write_text("// unselected texture implementation v1\n")
    cmake += 'set(LEGO_NATIVE_TEXTURE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/textures-selected.cpp")\n'
    (source / "CMakeLists.txt").write_text(cmake + production)

    def run(command):
        result = subprocess.run(command, text=True, capture_output=True, timeout=30)
        if result.returncode:
            (output / "failure.log").write_text(result.stdout + result.stderr)
            result.check_returncode()
        return result

    configure = run([args.cmake, "-S", str(source), "-B", str(build), "-G", args.generator,
                     "-DCMAKE_CXX_COMPILER=" + args.compiler, "-DCMAKE_BUILD_TYPE=Release"])
    (output / "configure.log").write_text(configure.stdout + configure.stderr)
    assert not linked.exists() and not runtime_linked.exists()
    assert not (build / "generated/native_gpu_build_info.h").exists()

    def check_build():
        result = run([args.cmake, "--build", str(build), "--parallel", "2"])
        with (output / "build.log").open("a") as log:
            log.write(result.stdout + result.stderr)
        assert linked.read_bytes() == raw.read_bytes()
        assert runtime_linked.read_bytes() == runtime.read_bytes()
        # Parse the actual generated hash script's ordered input list rather
        # than assuming that the fixture knows which files production fingerprints.
        script = (build / "generate-native-gpu-fingerprint.cmake").read_text()
        paths = re.search(r'set\(LEGO_NATIVE_GPU_FINGERPRINT_INPUTS "([^"]+)"\)', script).group(1).split(";")
        assert microcode_index.as_posix() in paths
        assert selected_texture.as_posix() in paths
        assert unselected_texture.as_posix() not in paths
        hashes = "".join(hashlib.sha256(Path(path).read_bytes()).hexdigest() for path in paths)
        expected = hashlib.sha256(hashes.encode()).hexdigest()[:16]
        header = (build / "generated/native_gpu_build_info.h").read_text()
        assert f'"{expected}"' in header
        assert (build / "native-gpu-build.txt").read_text() == f"LEGO Dimensions native GPU {expected}\n"
        binary = build / ("legodimensions.exe" if (build / "legodimensions.exe").exists() else "legodimensions")
        assert run([str(binary)]).stdout.strip() == expected
        return expected

    first = check_build()
    assert Path(str(linked) + ".calls").read_text().splitlines() == ["built"]
    assert Path(str(runtime_linked) + ".calls").read_text().splitlines() == ["built"]
    # Changing either bank must re-link, regenerate provenance, and recompile
    # the executable that includes the fingerprint header.
    raw.write_text("main archive v2\n")
    second = check_build()
    assert second != first
    assert Path(str(linked) + ".calls").read_text().splitlines() == ["built", "built"]
    assert Path(str(runtime_linked) + ".calls").read_text().splitlines() == ["built"]
    runtime.write_text("runtime archive v2\n")
    third = check_build()
    assert third != second
    assert Path(str(runtime_linked) + ".calls").read_text().splitlines() == ["built", "built"]
    # An index-only edit changes which shader an existing bank supplies. Its
    # provenance must change independently of any bank producer invocation.
    microcode_index.write_text("microcode lookup index v2\n")
    fourth = check_build()
    assert fourth != third
    assert Path(str(linked) + ".calls").read_text().splitlines() == ["built", "built"]
    assert Path(str(runtime_linked) + ".calls").read_text().splitlines() == ["built", "built"]
    # Header-only changes are build dependencies without a reconfigure/read
    # of not-yet-generated banks during the initial CMake configure.
    (source / "src/gpu_native/index_draw.h").write_text("// synthetic bounds helper v2\n")
    fifth = check_build()
    assert fifth != fourth
    assert check_build() == fifth
    (typed_bank / "float-blend-indices-bank.bin").write_text("synthetic typed shader bytes v2\n")
    sixth = check_build()
    assert sixth != fifth
    assert check_build() == sixth
    (source / "src/settings_registry.cpp").write_text("// synthetic settings backend v2\n")
    seventh = check_build()
    assert seventh != sixth
    (source / "src/native_frontend_guest.h").write_text("// synthetic native frontend bridge v2\n")
    eighth = check_build()
    assert eighth != seventh
    assert check_build() == eighth
    unselected_texture.write_text("// unselected texture implementation v2\n")
    assert check_build() == eighth
    selected_texture.write_text("// selected texture implementation v2\n")
    ninth = check_build()
    assert ninth != eighth
    assert check_build() == ninth
    (source / "src/native_frontend_binding.h").write_text("// synthetic native frontend registration v2\n")
    tenth = check_build()
    assert tenth != ninth
    assert check_build() == tenth
    (source / "src/native_frontend_component.h").write_text("// synthetic owned native component v2\n")
    eleventh = check_build()
    assert eleventh != tenth
    assert check_build() == eleventh
    (source / "src/settings_frontend.cpp").write_text("// synthetic native frontend controller v2\n")
    twelfth = check_build()
    assert twelfth != eleventh
    (source / "src/settings_frontend.h").write_text("// synthetic native frontend model ABI v2\n")
    thirteenth = check_build()
    assert thirteenth != twelfth
    assert check_build() == thirteenth
    (source / "src/settings_native_bridge.cpp").write_text("// synthetic native hookup v2\n")
    fourteenth = check_build()
    assert fourteenth != thirteenth
    (source / "src/settings_native_bridge.h").write_text("// synthetic native hookup ABI v2\n")
    fifteenth = check_build()
    assert fifteenth != fourteenth
    (source / "src/settings_native_hooks.cpp").write_text("// synthetic native hook adapter v2\n")
    sixteenth = check_build()
    assert sixteenth != fifteenth
    assert check_build() == sixteenth
    revision_markers = []
    previous = sixteenth
    for relative in ["src/settings_revision.cpp", "src/settings_revision.h",
                     "src/gpu_native/settings_snapshot.cpp", "src/gpu_native/settings_snapshot.h",
                     "src/gpu_native/settings_runtime.cpp", "src/gpu_native/settings_runtime.h",
                     "src/gpu_native/fsr3_settings.h", "src/native_effects.cpp", "src/gpu_native/visual_effects.h",
                     "legodimensions_config.toml", "generated/default/legodimensions_recomp.61.cpp",
                     "generated/default/legodimensions_funcs.61.h"]:
        (source / relative).write_text("// synthetic copied settings revision input v2\n")
        marker = check_build()
        assert marker != previous, relative
        assert check_build() == marker
        revision_markers.append(marker)
        previous = marker
    (output / "verification.json").write_text(json.dumps(dict(passed=True, game_launched=False,
        production_cmake_sha256=hashlib.sha256((root / "rexlego/CMakeLists.txt").read_bytes()).hexdigest(),
        markers=[first, second, third, fourth, fifth, sixth, seventh, eighth, ninth, tenth, eleventh, twelfth, thirteenth, fourteenth, fifteenth, sixteenth, *revision_markers],
        scope="Production CMake fingerprint rule on synthetic banks/inputs and a tiny executable. Actual game executable was not rebuilt or launched."), indent=2))
    print("PASS: fresh native fingerprint configure; generated banks precede hashing; bank/index/header/settings/guest-bridge/registration/selected-texture changes update executable provenance; unselected texture changes do not")


if __name__ == "__main__":
    main()
