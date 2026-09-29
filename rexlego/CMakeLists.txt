# legodimensions - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
# RC compiles res/legodimensions.rc, which carries the executable's icon.
# Enabled on Windows only; the .rc is left out of the sources elsewhere.
if(WIN32)
    project(legodimensions LANGUAGES CXX RC)
else()
    project(legodimensions LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Native D3D12 renderer work stays opt-in until its hook map and presentation
# path are complete. The shipping rexgpu-xenos path is untouched when this is
# disabled.
option(LEGODIMENSIONS_NATIVE_GPU "Build the experimental native D3D12 renderer" OFF)

# Sources
set(LEGODIMENSIONS_SOURCES
    src/main.cpp
    src/discord_presence.cpp
    src/ui_theme.cpp
    src/mod_menu.cpp
    src/main_menu_extras.cpp
    src/skip_cutscenes.cpp
    src/cheat_engine.cpp
    src/cheat_menu.cpp
    src/toypad_app.cpp
    src/updates.cpp
    src/name_trace.cpp
)

if(LEGODIMENSIONS_NATIVE_GPU)
    if(NOT WIN32)
        message(FATAL_ERROR "LEGODIMENSIONS_NATIVE_GPU currently supports Windows D3D12 only")
    endif()

    enable_language(C)
    set(PLUME_D3D12_AGILITY_SDK_ENABLED OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/plume"
                     "${CMAKE_CURRENT_BINARY_DIR}/thirdparty/plume"
                     EXCLUDE_FROM_ALL)

    # Plume enables its Vulkan backend unconditionally upstream. This build is
    # deliberately D3D12-only, so keep the shared interface and Win32 backend
    # but do not compile or link the unused Vulkan implementation.
    get_target_property(LEGO_PLUME_SOURCES plume SOURCES)
    list(FILTER LEGO_PLUME_SOURCES EXCLUDE REGEX "plume_vulkan\\.(cpp|h)$")
    set_property(TARGET plume PROPERTY SOURCES "${LEGO_PLUME_SOURCES}")

    list(APPEND LEGODIMENSIONS_SOURCES
        src/gpu_native/buffers.cpp
        src/gpu_native/device.cpp
        src/gpu_native/format.cpp
        src/gpu_native/hooks_buffer.cpp
        src/gpu_native/hooks_device.cpp
        src/gpu_native/hooks_shader.cpp
        src/gpu_native/hooks_texture.cpp
        src/gpu_native/shader_archive.cpp
        src/gpu_native/shaders.cpp
        src/gpu_native/textures.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/miniz/miniz.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/zstd/zstddeclib.c"
    )
endif()

# Research probes: a guard-page watchpoint on the area flags and the failed
# attempt at forcing the 3D portrait. They are off in a release because one of
# them single-steps every read of a watched page, which is slow enough to look
# like a hang, and neither is any use to a player. Turn them back on with
# -DLEGODIMENSIONS_DEV_PROBES=ON while working on the portrait.
option(LEGODIMENSIONS_DEV_PROBES "Build the area-flag watchpoint and portrait experiments" OFF)
if(LEGODIMENSIONS_DEV_PROBES)
    list(APPEND LEGODIMENSIONS_SOURCES src/icon_probe.cpp src/hub_portrait.cpp
        src/area_watch.cpp)
    add_compile_definitions(LEGODIMENSIONS_DEV_PROBES)
endif()

if(WIN32)
    add_executable(legodimensions WIN32 ${LEGODIMENSIONS_SOURCES}
                   res/legodimensions.rc)
else()
    add_executable(legodimensions ${LEGODIMENSIONS_SOURCES})
endif()

rexglue_setup_target(legodimensions)

if(LEGODIMENSIONS_NATIVE_GPU)
    target_compile_definitions(legodimensions PRIVATE LEGODIMENSIONS_NATIVE_GPU=1)
    target_include_directories(legodimensions PRIVATE src)
    target_include_directories(legodimensions PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/miniz"
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/zstd")
    target_link_libraries(legodimensions PRIVATE plume d3d12 dxgi rex::xxhash)

    # Generated locally by tools/gpu-aot. Keeping it out of Git avoids a
    # multi-megabyte generated source file while still making an enabled build
    # fail loudly if the AOT stage has not been run.
    set(LEGO_NATIVE_SHADER_CACHE
        "${CMAKE_CURRENT_SOURCE_DIR}/out/native-gpu/lego-dxil-cache.cpp")
    if(NOT EXISTS "${LEGO_NATIVE_SHADER_CACHE}")
        message(FATAL_ERROR
            "Native GPU needs ${LEGO_NATIVE_SHADER_CACHE}; run tools/gpu-aot first")
    endif()
    target_sources(legodimensions PRIVATE "${LEGO_NATIVE_SHADER_CACHE}")
    set_source_files_properties("${LEGO_NATIVE_SHADER_CACHE}"
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

    # XenosRecomp emits a DXIL library whenever a shader reads one of its two
    # specialization bits. Link every used value at build time; gameplay then
    # creates complete shaders directly and never invokes DXC on a frame.
    set(LEGO_NATIVE_LINKED_SHADER_CACHE
        "${CMAKE_CURRENT_SOURCE_DIR}/out/native-gpu/lego-linked-dxil-cache.cpp")
    set(LEGO_DXC_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/../research/reblue/thirdparty/XenosRecomp/thirdparty/dxc-bin"
        CACHE PATH "Pinned DXC binary package used by the LEGO GPU AOT prelinker")
    set(LEGO_DXC_ARCH x64)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
        set(LEGO_DXC_ARCH arm64)
    endif()

    add_executable(lego_gpu_prelink EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gpu-aot/prelink_shader_cache.cpp"
        src/gpu_native/dxc_link.cpp
        "${LEGO_NATIVE_SHADER_CACHE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/miniz/miniz.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/zstd/zstddeclib.c")
    target_include_directories(lego_gpu_prelink PRIVATE
        src
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/miniz"
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/zstd"
        "${LEGO_DXC_ROOT}/inc")
    target_link_libraries(lego_gpu_prelink PRIVATE
        "${LEGO_DXC_ROOT}/lib/${LEGO_DXC_ARCH}/dxcompiler.lib")
    add_custom_command(TARGET lego_gpu_prelink POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LEGO_DXC_ROOT}/bin/${LEGO_DXC_ARCH}/dxcompiler.dll"
            "${LEGO_DXC_ROOT}/bin/${LEGO_DXC_ARCH}/dxil.dll"
            "$<TARGET_FILE_DIR:lego_gpu_prelink>"
        VERBATIM)

    add_custom_command(
        OUTPUT "${LEGO_NATIVE_LINKED_SHADER_CACHE}"
        COMMAND "$<TARGET_FILE:lego_gpu_prelink>"
                "${LEGO_NATIVE_LINKED_SHADER_CACHE}"
        DEPENDS lego_gpu_prelink
        COMMENT "Pre-linking LEGO specialization-constant shader variants"
        VERBATIM)
    target_sources(legodimensions PRIVATE "${LEGO_NATIVE_LINKED_SHADER_CACHE}")
    set_source_files_properties("${LEGO_NATIVE_LINKED_SHADER_CACHE}"
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

    # Fingerprint the exact native renderer inputs. CMAKE_CONFIGURE_DEPENDS
    # makes a header-only edit trigger reconfigure as well as recompilation,
    # and the fingerprint printed at startup proves which executable is live.
    set(LEGO_NATIVE_GPU_FINGERPRINT_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/device.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/device.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/d3d.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/format.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/format.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/textures.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/textures.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/hooks_texture.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/buffers.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/buffers.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/hooks_buffer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/hooks_device.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/hooks_shader.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shaders.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shaders.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shader_archive.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shader_archive.h"
        "${LEGO_NATIVE_SHADER_CACHE}"
        "${LEGO_NATIVE_LINKED_SHADER_CACHE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${LEGO_NATIVE_GPU_FINGERPRINT_INPUTS})
    set(LEGO_NATIVE_GPU_FINGERPRINT_MATERIAL "")
    foreach(LEGO_NATIVE_GPU_INPUT IN LISTS LEGO_NATIVE_GPU_FINGERPRINT_INPUTS)
        file(SHA256 "${LEGO_NATIVE_GPU_INPUT}" LEGO_NATIVE_GPU_INPUT_HASH)
        string(APPEND LEGO_NATIVE_GPU_FINGERPRINT_MATERIAL
            "${LEGO_NATIVE_GPU_INPUT_HASH}")
    endforeach()
    string(SHA256 LEGO_NATIVE_GPU_FINGERPRINT_FULL
        "${LEGO_NATIVE_GPU_FINGERPRINT_MATERIAL}")
    string(SUBSTRING "${LEGO_NATIVE_GPU_FINGERPRINT_FULL}" 0 16
        LEGO_NATIVE_GPU_FINGERPRINT)
    configure_file(src/gpu_native/build_info.h.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/native_gpu_build_info.h" @ONLY)
    target_include_directories(legodimensions PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/native-gpu-build.txt"
        "LEGO Dimensions native GPU ${LEGO_NATIVE_GPU_FINGERPRINT}\n")
    add_custom_command(TARGET legodimensions POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/native-gpu-build.txt"
            "$<TARGET_FILE_DIR:legodimensions>/native-gpu-build.txt"
        VERBATIM)
endif()

# The achievement unlock sound, converted from the user-supplied MP3 to a quiet
# 48 kHz WAV. Copied next to the executable because that is where the SDK cvar
# ui_sound_achievement looks by default; point the cvar elsewhere to swap it.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/achievement_unlocked.wav")
    add_custom_command(TARGET legodimensions POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/res/achievement_unlocked.wav"
                "$<TARGET_FILE_DIR:legodimensions>/achievement_unlocked.wav"
        COMMENT "Copying achievement sound next to the executable")
endif()

# The generated translation units (legodimensions_recomp) are where nearly all
# guest CPU time goes, and they were being built for a bare x86-64 without
# AVX2/BMI2/FMA. TheSimpsonsGameRecomp measured this + -O3 as a real win, so
# the shipped builds use x86-64-v3 (Haswell / Zen 1 or newer).
#
# It is a cache variable rather than a hard-coded flag because people do build
# this for older CPUs. Passing -DCMAKE_CXX_FLAGS=-march=... does NOT work for
# that: target_compile_options land after CMAKE_CXX_FLAGS on the command line,
# so the value here would win. Lower the floor with:
#
#   cmake -S rexlego -B ... -DLEGODIMENSIONS_X86_BASELINE=x86-64-v2
#
# x86-64-v2 (Nehalem/Bulldozer) is the lowest that builds - the SDK's byte-swap
# helpers need SSSE3. Set it to "none" to supply your own -march in
# CMAKE_CXX_FLAGS. Whatever is chosen here must also be passed to the SDK
# (-DREXGLUE_X86_BASELINE=...), or the two halves disagree about AVX2 and the
# game crashes with an illegal instruction on the CPU that cannot run it.
set(LEGODIMENSIONS_X86_BASELINE "x86-64-v3" CACHE STRING
    "x86 -march baseline for the game and the recompiled code (or \"none\")")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    if(LEGODIMENSIONS_X86_BASELINE AND NOT LEGODIMENSIONS_X86_BASELINE STREQUAL "none")
        message(STATUS "legodimensions x86 baseline: -march=${LEGODIMENSIONS_X86_BASELINE}")
        target_compile_options(legodimensions PRIVATE -march=${LEGODIMENSIONS_X86_BASELINE})
    endif()
    if(TARGET legodimensions_recomp)
        target_compile_options(legodimensions_recomp PRIVATE
            $<$<CONFIG:RelWithDebInfo,Release>:-O3>)
        if(LEGODIMENSIONS_X86_BASELINE AND NOT LEGODIMENSIONS_X86_BASELINE STREQUAL "none")
            target_compile_options(legodimensions_recomp PRIVATE
                -march=${LEGODIMENSIONS_X86_BASELINE})
        endif()
    endif()
endif()
