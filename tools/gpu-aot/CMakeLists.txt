cmake_minimum_required(VERSION 3.25)
project(NativeGpuPortableChecks LANGUAGES CXX)
include(CTest)

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
