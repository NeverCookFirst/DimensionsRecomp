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
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gpu-aot/prepare_plume.py" --check
        RESULT_VARIABLE LEGO_PLUME_PREPARE_RESULT
        OUTPUT_VARIABLE LEGO_PLUME_PREPARE_OUTPUT
        ERROR_VARIABLE LEGO_PLUME_PREPARE_ERROR
        TIMEOUT 30)
    if(NOT LEGO_PLUME_PREPARE_RESULT STREQUAL "0")
        message(FATAL_ERROR "Native Plume dependency needs preparation. Run python tools/gpu-aot/prepare_plume.py.\n${LEGO_PLUME_PREPARE_OUTPUT}${LEGO_PLUME_PREPARE_ERROR}")
    endif()
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
        src/gpu_native/renderer_mode.cpp
        src/gpu_native/memory_watch.cpp
        src/gpu_native/queries.cpp
        src/gpu_native/device.cpp
        src/gpu_native/draw.cpp
        src/gpu_native/format.cpp
        src/gpu_native/hooks_buffer.cpp
        src/gpu_native/hooks_device.cpp
        src/gpu_native/hooks_portrait.cpp
        src/gpu_native/hooks_shader.cpp
        src/gpu_native/hooks_state.cpp
        src/gpu_native/hooks_texture.cpp
        src/gpu_native/shader_archive.cpp
        src/gpu_native/shaders.cpp
        src/gpu_native/state.cpp
        src/gpu_native/textures.cpp
        src/gpu_native/vertex_declarations.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../rexglue-sdk/src/graphics/pipeline/texture/util.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../rexglue-sdk/src/graphics/pipeline/texture/info.cpp"
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
    target_link_libraries(legodimensions PRIVATE plume d3d12 dxgi rex::xxhash renderdoc)

    # Generated locally by tools/gpu-aot. Keeping it out of Git avoids a
    # multi-megabyte generated source file while still making an enabled build
    # fail loudly if the AOT stage has not been run.
    set(LEGO_NATIVE_SHADER_CACHE
        "${CMAKE_CURRENT_SOURCE_DIR}/out/native-gpu/lego-dxil-cache.cpp"
        CACHE FILEPATH "AOT DXIL cache to embed (keep older caches for rollback)")
    if(NOT EXISTS "${LEGO_NATIVE_SHADER_CACHE}")
        message(FATAL_ERROR
            "Native GPU needs ${LEGO_NATIVE_SHADER_CACHE}; run tools/gpu-aot first")
    endif()
    target_sources(legodimensions PRIVATE "${LEGO_NATIVE_SHADER_CACHE}")
    set_source_files_properties("${LEGO_NATIVE_SHADER_CACHE}"
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    set(LEGO_NATIVE_MICROCODE_INDEX
        "${CMAKE_CURRENT_SOURCE_DIR}/out/native-gpu/lego-microcode-index.cpp")
    if(NOT EXISTS "${LEGO_NATIVE_MICROCODE_INDEX}")
        message(FATAL_ERROR
            "Native GPU needs ${LEGO_NATIVE_MICROCODE_INDEX}; run lego_gpu_microcode_index first")
    endif()
    target_sources(legodimensions PRIVATE "${LEGO_NATIVE_MICROCODE_INDEX}")
    set_source_files_properties("${LEGO_NATIVE_MICROCODE_INDEX}"
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    set(LEGO_NATIVE_RUNTIME_SHADER_CACHE
        "${CMAKE_CURRENT_SOURCE_DIR}/out/native-gpu/runtime-cache.cpp")
    if(NOT EXISTS "${LEGO_NATIVE_RUNTIME_SHADER_CACHE}")
        message(FATAL_ERROR
            "Native GPU needs ${LEGO_NATIVE_RUNTIME_SHADER_CACHE}; capture and compile runtime shaders first")
    endif()
    target_sources(legodimensions PRIVATE
        src/gpu_native/runtime_shader_cache.cpp
        src/gpu_native/runtime_linked_shader_cache.cpp)

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

    set(LEGO_NATIVE_HOST_SHADER_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shaders")
    set(LEGO_NATIVE_GENERATED_SHADER_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/gpu_native/shaders")
    set(LEGO_NATIVE_DXC
        "${LEGO_DXC_ROOT}/bin/${LEGO_DXC_ARCH}/dxc.exe")
    # Both archive prelinkers are built for the Windows target even when
    # standalone host shader compilation uses a host-native DXC executable.
    if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
        message(FATAL_ERROR
            "Native GPU cross-build prelinkers require CMAKE_CROSSCOMPILING_EMULATOR, including when LEGO_HOST_DXC_EXECUTABLE is set")
    endif()
    set(LEGO_HOST_DXC_EXECUTABLE "" CACHE FILEPATH
        "Host-native DXC executable for build-time host shader compilation")
    if(LEGO_HOST_DXC_EXECUTABLE)
        if(NOT EXISTS "${LEGO_HOST_DXC_EXECUTABLE}")
            message(FATAL_ERROR "Host DXC not found: ${LEGO_HOST_DXC_EXECUTABLE}")
        endif()
        set(LEGO_NATIVE_DXC "${LEGO_HOST_DXC_EXECUTABLE}")
        set(LEGO_NATIVE_DXC_COMMAND "${LEGO_NATIVE_DXC}")
    elseif(CMAKE_CROSSCOMPILING)
        if(NOT CMAKE_CROSSCOMPILING_EMULATOR)
            message(FATAL_ERROR
                "Cross-building needs LEGO_HOST_DXC_EXECUTABLE or CMAKE_CROSSCOMPILING_EMULATOR for Windows DXC")
        endif()
        set(LEGO_NATIVE_DXC_COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${LEGO_NATIVE_DXC}")
    else()
        set(LEGO_NATIVE_DXC_COMMAND "${LEGO_NATIVE_DXC}")
    endif()
    foreach(LEGO_HOST_SHADER IN ITEMS copy_vs copy_color_ps resolve_color_ps depth_pack_ps depth_restore_ps)
        if(LEGO_HOST_SHADER STREQUAL "copy_vs")
            set(LEGO_HOST_SHADER_PROFILE vs_6_6)
        else()
            set(LEGO_HOST_SHADER_PROFILE ps_6_6)
        endif()
        set(LEGO_HOST_SHADER_HEADER
            "${LEGO_NATIVE_GENERATED_SHADER_DIR}/${LEGO_HOST_SHADER}.h")
        add_custom_command(
            OUTPUT "${LEGO_HOST_SHADER_HEADER}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${LEGO_NATIVE_GENERATED_SHADER_DIR}"
            COMMAND ${LEGO_NATIVE_DXC_COMMAND}
                    -T ${LEGO_HOST_SHADER_PROFILE} -E main -HV 2021
                    -all-resources-bound -Wno-ignored-attributes
                    -I "${LEGO_NATIVE_HOST_SHADER_DIR}"
                    -Fh "${LEGO_HOST_SHADER_HEADER}"
                    -Vn g_${LEGO_HOST_SHADER}_dxil
                    "${LEGO_NATIVE_HOST_SHADER_DIR}/${LEGO_HOST_SHADER}.hlsl"
            DEPENDS
                "${LEGO_NATIVE_DXC}"
                "${LEGO_NATIVE_HOST_SHADER_DIR}/${LEGO_HOST_SHADER}.hlsl"
                "${LEGO_NATIVE_HOST_SHADER_DIR}/copy_common.hlsli"
            COMMENT "Compiling ${LEGO_HOST_SHADER}.hlsl (DXIL)"
            VERBATIM)
        target_sources(legodimensions PRIVATE "${LEGO_HOST_SHADER_HEADER}")
    endforeach()
    target_include_directories(legodimensions PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated")

    add_executable(lego_gpu_microcode_index EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gpu-aot/generate_microcode_index.cpp")
    target_link_libraries(lego_gpu_microcode_index PRIVATE rex::xxhash)

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

    add_executable(lego_gpu_prelink_runtime EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gpu-aot/prelink_shader_cache.cpp"
        src/gpu_native/dxc_link.cpp
        "${LEGO_NATIVE_RUNTIME_SHADER_CACHE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/miniz/miniz.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/zstd/zstddeclib.c")
    target_include_directories(lego_gpu_prelink_runtime PRIVATE
        src
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/miniz"
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/zstd"
        "${LEGO_DXC_ROOT}/inc")
    target_link_libraries(lego_gpu_prelink_runtime PRIVATE
        "${LEGO_DXC_ROOT}/lib/${LEGO_DXC_ARCH}/dxcompiler.lib")
    add_custom_command(TARGET lego_gpu_prelink_runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LEGO_DXC_ROOT}/bin/${LEGO_DXC_ARCH}/dxcompiler.dll"
            "${LEGO_DXC_ROOT}/bin/${LEGO_DXC_ARCH}/dxil.dll"
            "$<TARGET_FILE_DIR:lego_gpu_prelink_runtime>"
        VERBATIM)

    add_custom_command(
        OUTPUT "${LEGO_NATIVE_LINKED_SHADER_CACHE}"
        COMMAND lego_gpu_prelink
                "${LEGO_NATIVE_LINKED_SHADER_CACHE}"
        DEPENDS lego_gpu_prelink
        COMMENT "Pre-linking LEGO specialization-constant shader variants"
        VERBATIM)
    add_custom_target(lego_main_linked_cache
        DEPENDS "${LEGO_NATIVE_LINKED_SHADER_CACHE}")
    add_dependencies(legodimensions lego_main_linked_cache)
    # Large compressed banks otherwise become millions of C++ initializer
    # values. Keep their metadata in C++, but embed their unchanged bytes in a
    # target COFF object when host LLVM tools support the target's MSVC ABI.
    find_program(LEGO_HOST_LLVM_MC NAMES llvm-mc llvm-mc-22 llvm-mc-21 llvm-mc-20
        NO_CMAKE_FIND_ROOT_PATH)
    set(LEGO_NATIVE_BINARY_CACHE_ARCH "")
    # Windows native generators can retain the host CMAKE_SYSTEM_PROCESSOR
    # when clang-cl targets another architecture. Use the compiler's proven
    # architecture and pointer width; ARM64EC/X86/unknown retain C++ embedding.
    if(CMAKE_CXX_SIZEOF_DATA_PTR EQUAL 8)
        if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
            set(LEGO_NATIVE_BINARY_CACHE_ARCH x86_64)
        elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
            set(LEGO_NATIVE_BINARY_CACHE_ARCH aarch64)
        endif()
    endif()
    set(LEGO_NATIVE_BINARY_CACHE_SUPPORTED OFF)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
       CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC" AND
       LEGO_NATIVE_BINARY_CACHE_ARCH AND LEGO_HOST_LLVM_MC AND
       EXISTS "${LEGO_HOST_LLVM_MC}" AND NOT IS_DIRECTORY "${LEGO_HOST_LLVM_MC}" AND
       Python3_Interpreter_FOUND)
        set(LEGO_NATIVE_BINARY_CACHE_SUPPORTED ON)
    endif()
    option(LEGO_NATIVE_BINARY_CACHE
        "Embed the main linked DXIL bytes as COFF instead of a C++ initializer"
        ${LEGO_NATIVE_BINARY_CACHE_SUPPORTED})
    if(LEGO_NATIVE_BINARY_CACHE)
        if(NOT LEGO_NATIVE_BINARY_CACHE_SUPPORTED)
            message(FATAL_ERROR
                "LEGO_NATIVE_BINARY_CACHE needs x64/ARM64 Windows Clang with the MSVC ABI, Python3, and host llvm-mc; set LEGO_NATIVE_BINARY_CACHE=OFF for C++ embedding")
        endif()
        set(LEGO_NATIVE_BINARY_CACHE_SCRIPT
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gpu-aot/embed_linked_bank.py")
        set(LEGO_NATIVE_BINARY_CACHE_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/generated/gpu_native/linked-bank")
        set(LEGO_NATIVE_BINARY_CACHE_METADATA
            "${LEGO_NATIVE_BINARY_CACHE_DIR}/linked-bank-metadata.cpp")
        set(LEGO_NATIVE_BINARY_CACHE_OBJECT
            "${LEGO_NATIVE_BINARY_CACHE_DIR}/linked-bank.obj")
        add_custom_command(
            OUTPUT "${LEGO_NATIVE_BINARY_CACHE_METADATA}" "${LEGO_NATIVE_BINARY_CACHE_OBJECT}"
            BYPRODUCTS
                "${LEGO_NATIVE_BINARY_CACHE_DIR}/linked-bank.bin"
                "${LEGO_NATIVE_BINARY_CACHE_DIR}/linked-bank.s"
                "${LEGO_NATIVE_BINARY_CACHE_DIR}/linked-bank-manifest.json"
            COMMAND "${Python3_EXECUTABLE}" "${LEGO_NATIVE_BINARY_CACHE_SCRIPT}"
                    "${LEGO_NATIVE_LINKED_SHADER_CACHE}" "${LEGO_NATIVE_BINARY_CACHE_DIR}"
                    --llvm-mc "${LEGO_HOST_LLVM_MC}"
                    --arch "${LEGO_NATIVE_BINARY_CACHE_ARCH}"
            DEPENDS "${LEGO_NATIVE_LINKED_SHADER_CACHE}"
                    "${LEGO_NATIVE_BINARY_CACHE_SCRIPT}" "${LEGO_HOST_LLVM_MC}"
            COMMENT "Embedding main linked DXIL bank as ${LEGO_NATIVE_BINARY_CACHE_ARCH} COFF"
            VERBATIM)
        target_sources(legodimensions PRIVATE
            "${LEGO_NATIVE_BINARY_CACHE_METADATA}" "${LEGO_NATIVE_BINARY_CACHE_OBJECT}")
        set_source_files_properties("${LEGO_NATIVE_BINARY_CACHE_METADATA}"
            PROPERTIES GENERATED TRUE SKIP_PRECOMPILE_HEADERS ON)
        set_source_files_properties("${LEGO_NATIVE_BINARY_CACHE_OBJECT}"
            PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    else()
        target_sources(legodimensions PRIVATE "${LEGO_NATIVE_LINKED_SHADER_CACHE}")
        set_source_files_properties("${LEGO_NATIVE_LINKED_SHADER_CACHE}"
            PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endif()
    set(LEGO_NATIVE_RUNTIME_LINKED_SHADER_CACHE
        "${CMAKE_CURRENT_SOURCE_DIR}/out/native-gpu/runtime-linked-dxil-cache.cpp")
    add_custom_command(
        OUTPUT "${LEGO_NATIVE_RUNTIME_LINKED_SHADER_CACHE}"
        COMMAND lego_gpu_prelink_runtime
                "${LEGO_NATIVE_RUNTIME_LINKED_SHADER_CACHE}"
        DEPENDS lego_gpu_prelink_runtime
        COMMENT "Pre-linking LEGO runtime shader variants"
        VERBATIM)
    add_custom_target(lego_runtime_linked_cache
        DEPENDS "${LEGO_NATIVE_RUNTIME_LINKED_SHADER_CACHE}")
    add_dependencies(legodimensions lego_runtime_linked_cache)
    set_source_files_properties(
        src/gpu_native/runtime_linked_shader_cache.cpp
        PROPERTIES OBJECT_DEPENDS "${LEGO_NATIVE_RUNTIME_LINKED_SHADER_CACHE}")

    # Fingerprint the exact native renderer inputs after generated shader banks
    # exist. Hashing the linked cache during configure prevented a fresh build
    # from reaching the prelink target that creates that same file.
    set(LEGO_NATIVE_GPU_FINGERPRINT_INPUTS
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/plume/plume_d3d12.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/device.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/device.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/renderer_route.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/renderer_mode.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/renderer_mode.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/renderdoc_probe.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/legodimensions_app.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/window_mode.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/long_probe.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/memory_watch.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/memory_watch.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/descriptor_retirement.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/resolve_region.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/pixel_shader_filter.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/pool_copy.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/sampler_state.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/texture_swizzle.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/texture_alpha4_upload.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/cheat_menu.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/draw.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/draw.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/alpha_test.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/tonemap_dof.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shaders/copy_common.hlsli"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shaders/copy_vs.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shaders/copy_color_ps.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shaders/resolve_color_ps.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shaders/depth_pack_ps.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shaders/depth_restore_ps.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/depth_alias.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/d3d.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/depth_state.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/blend_state.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/texture_number.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/texture_volume_upload.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/tile_extent.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shader_texture_usage.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/texture_upload_cache.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/vertex_byte_order.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/format.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/format.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/textures.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/textures.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/hooks_texture.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/state.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/state.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/hooks_state.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/vertex_declarations.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/vertex_declarations.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/buffers.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/buffers.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/queries.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/queries.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/buffer_header.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/hooks_buffer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/hooks_device.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/hooks_portrait.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/portrait_probe.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/hooks_shader.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shaders.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shaders.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shader_archive.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/shader_archive.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/runtime_shader_cache.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/runtime_linked_shader_cache.cpp"
        "${LEGO_NATIVE_SHADER_CACHE}"
        "${LEGO_NATIVE_MICROCODE_INDEX}"
        "${LEGO_NATIVE_LINKED_SHADER_CACHE}"
        "${LEGO_NATIVE_RUNTIME_SHADER_CACHE}"
        "${LEGO_NATIVE_RUNTIME_LINKED_SHADER_CACHE}")
    # Include every native helper header so changes to upload, bounds or
    # completion logic cannot retain an older executable fingerprint.
    file(GLOB_RECURSE LEGO_NATIVE_GPU_LOCAL_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/*.h")
    list(APPEND LEGO_NATIVE_GPU_FINGERPRINT_INPUTS ${LEGO_NATIVE_GPU_LOCAL_HEADERS})
    list(REMOVE_DUPLICATES LEGO_NATIVE_GPU_FINGERPRINT_INPUTS)
    set(LEGO_NATIVE_GPU_FINGERPRINT_SCRIPT
        "${CMAKE_CURRENT_BINARY_DIR}/generate-native-gpu-fingerprint.cmake")
    file(CONFIGURE OUTPUT "${LEGO_NATIVE_GPU_FINGERPRINT_SCRIPT}" CONTENT [=[
set(LEGO_NATIVE_GPU_FINGERPRINT_INPUTS "@LEGO_NATIVE_GPU_FINGERPRINT_INPUTS@")
set(LEGO_NATIVE_GPU_FINGERPRINT_MATERIAL "")
foreach(LEGO_NATIVE_GPU_INPUT IN LISTS LEGO_NATIVE_GPU_FINGERPRINT_INPUTS)
    file(SHA256 "${LEGO_NATIVE_GPU_INPUT}" LEGO_NATIVE_GPU_INPUT_HASH)
    string(APPEND LEGO_NATIVE_GPU_FINGERPRINT_MATERIAL "${LEGO_NATIVE_GPU_INPUT_HASH}")
endforeach()
string(SHA256 LEGO_NATIVE_GPU_FINGERPRINT_FULL "${LEGO_NATIVE_GPU_FINGERPRINT_MATERIAL}")
string(SUBSTRING "${LEGO_NATIVE_GPU_FINGERPRINT_FULL}" 0 16 LEGO_NATIVE_GPU_FINGERPRINT)
configure_file("@CMAKE_CURRENT_SOURCE_DIR@/src/gpu_native/build_info.h.in"
    "@CMAKE_CURRENT_BINARY_DIR@/generated/native_gpu_build_info.h" @ONLY)
file(WRITE "@CMAKE_CURRENT_BINARY_DIR@/native-gpu-build.txt"
    "LEGO Dimensions native GPU ${LEGO_NATIVE_GPU_FINGERPRINT}\n")
]=] @ONLY)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/native_gpu_build_info.h"
               "${CMAKE_CURRENT_BINARY_DIR}/native-gpu-build.txt"
        COMMAND ${CMAKE_COMMAND} -P "${LEGO_NATIVE_GPU_FINGERPRINT_SCRIPT}"
        DEPENDS ${LEGO_NATIVE_GPU_FINGERPRINT_INPUTS}
                "${LEGO_NATIVE_GPU_FINGERPRINT_SCRIPT}"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu_native/build_info.h.in"
        COMMENT "Fingerprinting native renderer sources and generated shader banks"
        VERBATIM)
    add_custom_target(lego_native_gpu_fingerprint
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated/native_gpu_build_info.h"
                "${CMAKE_CURRENT_BINARY_DIR}/native-gpu-build.txt")
    # Make generators must complete both bank producers before the fingerprint
    # and metadata compilation consume their outputs through separate paths.
    add_dependencies(lego_native_gpu_fingerprint
        lego_main_linked_cache lego_runtime_linked_cache)
    add_dependencies(legodimensions lego_native_gpu_fingerprint)
    target_sources(legodimensions PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/native_gpu_build_info.h")
    target_include_directories(legodimensions PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
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
