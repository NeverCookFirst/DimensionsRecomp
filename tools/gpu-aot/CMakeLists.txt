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
