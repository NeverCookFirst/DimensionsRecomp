cmake_minimum_required(VERSION 3.25)
project(NativeGpuPortableChecks LANGUAGES CXX)
include(CTest)

# The private-run harness uses Linux /proc, flock and reflinks. No game assets
# are required by its synthetic preparation, sampling and comparison checks.
if(BUILD_TESTING AND UNIX)
    find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME renderer_regression
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/regression/test_regression.py)
        set_tests_properties(renderer_regression PROPERTIES TIMEOUT 30)
    endif()
endif()


# No SDK, generated game code, game assets, DXC, or graphics device required.
set(CHECKS blend_state buffer_header color_resolve_swizzle descriptor_retirement
    pixel_shader_filter polygon_offset pool_copy portrait_identity present_rect
    resolve_region shader_texture_usage texture_alpha4_upload texture_number
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

# Validate the public native-renderer dependency with real source and fake COM.
if(BUILD_TESTING AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/plume/plume_d3d12.cpp")
        add_test(NAME plume_command_creation
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_plume_command_creation.py
                ${CMAKE_CURRENT_BINARY_DIR}/fixtures/plume_command_creation
                --compiler ${CMAKE_CXX_COMPILER})
        set_tests_properties(plume_command_creation PROPERTIES TIMEOUT 60)
        add_test(NAME gpu_timestamps
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_gpu_timestamps.py
                ${CMAKE_CURRENT_BINARY_DIR}/fixtures/gpu_timestamps
                --compiler ${CMAKE_CXX_COMPILER})
        set_tests_properties(gpu_timestamps PROPERTIES TIMEOUT 60)
        if(UNIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/plume/.git")
            add_test(NAME prepare_plume
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_prepare_plume.py
                    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/prepare_plume)
            set_tests_properties(prepare_plume PROPERTIES TIMEOUT 60)
        endif()
    endif()
endif()

# Compile production bodies with deterministic allocation and fence failures.
# These use GNU-style compiler arguments and require no SDK or game assets.
if(BUILD_TESTING AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        foreach(check IN ITEMS buffer_alloc_diagnostic native_build_fingerprint command_slots constant_timing frame_cadence cull_state draw_diagnostic prelink_workers prelink_emission
                frame_ring_transaction submission_waits pipeline_failures
                completion_callbacks cpu_resource_wait draw_upload query_retirement)
            add_test(NAME ${check}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_${check}.py
                    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/${check}
                    --compiler ${CMAKE_CXX_COMPILER})
            set_tests_properties(${check} PROPERTIES TIMEOUT 60)
        endforeach()
        # Verify the actual SDK project template when its submodule is present.
        if(UNIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../rexglue-sdk/resources/templates/init/rexglue_cmake.inja")
            add_test(NAME cross_build_producers
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cross_build_producers.py
                    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/cross_build_producers
                    --compiler ${CMAKE_CXX_COMPILER})
            set_tests_properties(cross_build_producers PROPERTIES TIMEOUT 60)
        endif()
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
        if(UNIX)
            add_test(NAME prelink_publication
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_prelink_publication.py
                    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/prelink_publication
                    --compiler ${CMAKE_CXX_COMPILER})
            set_tests_properties(prelink_publication PROPERTIES TIMEOUT 60)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/plume/plume_d3d12.cpp")
            add_test(NAME resize_fences
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_resize_fences.py
                    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/resize_fences
                    --compiler ${CMAKE_CXX_COMPILER})
            set_tests_properties(resize_fences PROPERTIES TIMEOUT 60)
        endif()
    endif()
endif()

# These fixtures exercise the real extraction CLI with synthetic inputs.
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_program(LEGO_DOTNET_EXECUTABLE NAMES dotnet)
    if(Python3_Interpreter_FOUND AND LEGO_DOTNET_EXECUTABLE)
        foreach(check IN ITEMS extract_paths image_shader_extract embedded_shader_extract)
            add_test(NAME ${check}
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_${check}.py
                    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/${check}
                    --dotnet ${LEGO_DOTNET_EXECUTABLE})
            set_tests_properties(${check} PROPERTIES TIMEOUT 70)
        endforeach()
    endif()
endif()
