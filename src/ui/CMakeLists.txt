# The player (ui/player) is its own library, built from src/CMakeLists.txt.
file(GLOB_RECURSE ui_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
list(FILTER ui_sources EXCLUDE REGEX "/player/")
add_library(spud_ui STATIC ${ui_sources})
target_include_directories(spud_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(spud_ui PUBLIC spud_render spud_ui_player spud_fx PRIVATE SDL3::SDL3 glm::glm)

# UI shaders: GLSL -> SPIR-V into <bin>/shaders, next to the renderer's. Entry: output, source, defines.
set(ui_shader_src ${PROJECT_SOURCE_DIR}/shaders)
set(ui_shader_out ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders)
set(ui_shader_list
    "uimovie.vert|uimovie.vert|"
    "uimovie.frag|uimovie.frag|"
    "uimovie_composite.vert|uimovie_composite.vert|"
    "uimovie_composite.frag|uimovie_composite.frag|"
    "uimovie_filter.vert|uimovie_filter.vert|"
    "uimovie_filter.frag|uimovie_filter.frag|")
set(ui_spirv)
foreach(entry IN LISTS ui_shader_list)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 out_name)
    list(GET parts 1 src_name)
    list(GET parts 2 defines)
    set(out ${ui_shader_out}/${out_name}.spv)
    set(src ${ui_shader_src}/${src_name})
    set(define_args)
    if(NOT defines STREQUAL "")
        set(define_args ${defines})
    endif()
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ui_shader_out}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -Werror -I${ui_shader_src}
                ${define_args} -MD -MF ${out}.d -o ${out} ${src}
        DEPENDS ${src} ${ui_shader_src}/uimovie_common.glsl ${ui_shader_src}/uimovie_filter.glsl
        DEPFILE ${out}.d
        COMMENT "glslc ${out_name}"
        VERBATIM)
    list(APPEND ui_spirv ${out})
endforeach()
add_custom_target(spud_ui_shaders ALL DEPENDS ${ui_spirv})

add_dependencies(spud_ui spud_ui_shaders)
