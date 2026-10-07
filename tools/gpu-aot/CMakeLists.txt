cmake_minimum_required(VERSION 3.25)
project(NativeGpuPortableChecks LANGUAGES CXX)
include(CTest)

if(BUILD_TESTING)
    find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
endif()

# The private-run harness uses Linux /proc, flock and reflinks. No game assets
# are required by its synthetic preparation, sampling and comparison checks.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(Python3_Interpreter_FOUND)
        add_test(NAME renderer_regression
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/regression/test_regression.py)
        set_tests_properties(renderer_regression PROPERTIES TIMEOUT 30)
    endif()
endif()

# No SDK, generated game code, game assets, DXC, or graphics device required.
set(CHECKS blend_state buffer_header color_resolve_swizzle descriptor_retirement
    pixel_shader_filter polygon_offset pool_copy portrait_identity present_rect
    resolve_region shader_texture_usage texture_2d_upload texture_alpha4_upload texture_number
    texture_volume_upload tile_extent tonemap_dof vertex_byte_order vertex_upload
    window_mode)
foreach(check IN LISTS CHECKS)
    add_executable(test_${check} test_${check}.cpp)
    target_compile_features(test_${check} PRIVATE cxx_std_20)
    target_include_directories(test_${check} PRIVATE ../../rexlego/src)
    # The fixtures use assert; keep them active in Release builds too.
    if(MSVC)
        target_compile_options(test_${check} PRIVATE /UNDEBUG)
    else()
        target_compile_options(test_${check} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${check} COMMAND test_${check})
endforeach()

# These fixtures compile production function bodies with fake memory and fences.
# Base fixtures need no SDK, shader archive, generated game code or GPU.
# Optional fixtures below consume actual dependency sources when initialized.
# Their compiler flags target the GNU-style drivers (including clang++ on Windows).
# These Python drivers compile and execute host binaries without CMake's cross
# flags or emulator; only register them for a native host configuration.
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING
    AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    if(Python3_Interpreter_FOUND)
        set(PRODUCTION_CHECKS draw_device_view state_call_diagnostic buffer_alloc_diagnostic
            command_slots completion_callbacks constant_timing cpu_resource_wait cull_state
            draw_diagnostic draw_index_bounds draw_upload frame_cadence frame_ring_transaction
            framebuffer_owners memory_watch nonindexed_buffer_windows quad_buffer_windows
            pipeline_failures shader_binding_failures prelink_emission prelink_workers
            query_retirement shader_bounds submission_waits texture_binding_failures
            texture_source_residency texture_authority_probe)
        find_program(LEGO_GPU_CHECK_NINJA NAMES ninja NO_CMAKE_FIND_ROOT_PATH)
        find_program(LEGO_GPU_CHECK_MAKE NAMES make gmake NO_CMAKE_FIND_ROOT_PATH)
        if(LEGO_GPU_CHECK_NINJA)
            set(LEGO_GPU_CHECK_GENERATOR "Ninja")
        elseif(LEGO_GPU_CHECK_MAKE)
            set(LEGO_GPU_CHECK_GENERATOR "Unix Makefiles")
        endif()
        if(LEGO_GPU_CHECK_GENERATOR)
            list(APPEND PRODUCTION_CHECKS native_build_fingerprint)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../rexglue-sdk/thirdparty/xxHash/xxhash.h")
            list(APPEND PRODUCTION_CHECKS shader_capture_dedup)
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../rexglue-sdk/thirdparty/xxHash/xxhash.c")
                list(APPEND PRODUCTION_CHECKS precompiled_pack)
            endif()
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/texture_load_depth_unorm_cs.h"
            AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../rexglue-sdk/src/graphics/d3d12/texture_cache.cpp"
            AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../rexglue-sdk/src/graphics/pipeline/texture/util.cpp"
            AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../rexglue-sdk/src/graphics/pipeline/texture/cache.cpp")
            list(APPEND PRODUCTION_CHECKS texture_depth_upload)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../research/reblue/thirdparty/XenosRecomp/XenosRecomp/shader_recompiler.cpp"
            AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../research/reblue/thirdparty/XenosRecomp/thirdparty/fmt/include/fmt/format.h")
            list(APPEND PRODUCTION_CHECKS unmapped_vertex_fetch)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/plume/plume_d3d12.cpp")
            list(APPEND PRODUCTION_CHECKS plume_command_creation plume_state_bindings gpu_timestamps resize_fences)
        endif()
        if(UNIX)
            list(APPEND PRODUCTION_CHECKS prelink_publication)
        endif()
        # The cross-build fixture uses Unix host-tool scripts and reads the SDK's
        # actual generated-project template; an asset-free checkout may lack it.
        if(UNIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../rexglue-sdk/resources/templates/init/rexglue_cmake.inja")
            list(APPEND PRODUCTION_CHECKS cross_build_producers)
        endif()
        foreach(check IN LISTS PRODUCTION_CHECKS)
            set(extra_arguments)
            if(check STREQUAL "native_build_fingerprint")
                list(APPEND extra_arguments --generator "${LEGO_GPU_CHECK_GENERATOR}")
            endif()
            add_test(NAME ${check}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_${check}.py
                    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/${check}
                    --compiler ${CMAKE_CXX_COMPILER} ${extra_arguments})
            set_tests_properties(${check} PROPERTIES TIMEOUT 60)
        endforeach()
        # This fixture cross-compiles a tiny SDK-free Windows ABI cache and
        # inspects/archives a real COFF blob, rather than linking a host GPU app.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            set(LEGO_BINARY_CACHE_TEST_TOOLS_FOUND TRUE)
            foreach(tool IN ITEMS mc readobj objcopy ar ranlib)
                find_program(LEGO_BINARY_CACHE_TEST_${tool} NAMES llvm-${tool}
                    NO_CMAKE_FIND_ROOT_PATH)
                if(NOT LEGO_BINARY_CACHE_TEST_${tool})
                    set(LEGO_BINARY_CACHE_TEST_TOOLS_FOUND FALSE)
                endif()
            endforeach()
            if(WIN32)
                find_program(LEGO_BINARY_CACHE_TEST_GENERATOR NAMES ninja
                    NO_CMAKE_FIND_ROOT_PATH)
            else()
                find_program(LEGO_BINARY_CACHE_TEST_GENERATOR NAMES make gmake
                    NO_CMAKE_FIND_ROOT_PATH)
            endif()
            if(LEGO_BINARY_CACHE_TEST_TOOLS_FOUND AND LEGO_BINARY_CACHE_TEST_GENERATOR)
                add_test(NAME binary_cache_build
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_binary_cache_build.py
                        ${CMAKE_CURRENT_BINARY_DIR}/fixtures/binary_cache_build
                        --compiler ${CMAKE_CXX_COMPILER}
                        --llvm-mc ${LEGO_BINARY_CACHE_TEST_mc}
                        --llvm-readobj ${LEGO_BINARY_CACHE_TEST_readobj}
                        --llvm-objcopy ${LEGO_BINARY_CACHE_TEST_objcopy}
                        --llvm-ar ${LEGO_BINARY_CACHE_TEST_ar}
                        --llvm-ranlib ${LEGO_BINARY_CACHE_TEST_ranlib})
                set_tests_properties(binary_cache_build PROPERTIES TIMEOUT 60)
            endif()
        endif()
    else()
        message(STATUS "Python3 unavailable: production-body fixtures are disabled")
    endif()
endif()

# This offline Git preparation check invokes no C++ compiler.
if(BUILD_TESTING AND UNIX AND Python3_Interpreter_FOUND
    AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/plume/.git"
    AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/plume/plume_d3d12.cpp")
    find_package(Git QUIET)
    if(Git_FOUND)
        add_test(NAME prepare_plume
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_prepare_plume.py
                ${CMAKE_CURRENT_BINARY_DIR}/fixtures/prepare_plume)
        set_tests_properties(prepare_plume PROPERTIES TIMEOUT 60)
    endif()
endif()

# Extraction CLI fixtures do not depend on the selected C++ compiler frontend.
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    find_program(LEGO_DOTNET_EXECUTABLE NAMES dotnet NO_CMAKE_FIND_ROOT_PATH)
    if(LEGO_DOTNET_EXECUTABLE)
        foreach(check IN ITEMS extract_paths image_shader_extract embedded_shader_extract)
            add_test(NAME ${check}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_${check}.py
                    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/${check}
                    --dotnet ${LEGO_DOTNET_EXECUTABLE})
            set_tests_properties(${check} PROPERTIES TIMEOUT 70)
        endforeach()
    endif()
endif()
