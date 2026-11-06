spud_library(spud_render)
target_link_libraries(spud_render
    PUBLIC spud_formats Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator SDL3::SDL3 imgui::imgui)

# GLSL -> SPIR-V into <bin>/shaders at build time. Each entry: output name, source, defines.
set(SPUD_SHADER_SRC ${PROJECT_SOURCE_DIR}/shaders)
set(SPUD_SHADER_OUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders)
set(spud_shader_list
    "sky.vert|sky.vert|"
    "sky.frag|sky.frag|"
    "terrain.vert|terrain.vert|"
    "terrain.frag|terrain.frag|"
    "grass.vert|grass.vert|"
    "grass.frag|grass.frag|"
    "impostor.vert|impostor.vert|"
    "impostor.frag|impostor.frag|"
    "terrain_far.vert|terrain_far.vert|"
    "terrain_far.frag|terrain_far.frag|"
    "shadow_land_far.vert|shadow_land_far.vert|"
    "water.vert|water.vert|"
    "water.frag|water.frag|"
    "water_flow.frag|water.frag|-DFLOW"
    "water_lod.frag|water.frag|-DLOD"
    "water_under.frag|water.frag|-DUNDER"
    "water_under_flow.frag|water.frag|-DUNDER -DFLOW"
    "water_mask.frag|water_mask.frag|"
    "water_resolve.frag|water_resolve.frag|"
    "water_ripple.frag|water_ripple.frag|"
    "particle.vert|particle.vert|"
    "decal.vert|decal.vert|"
    "skin_decal.vert|skin_decal.vert|"
    "skin_decal.frag|skin_decal.frag|"
    "decal.frag|decal.frag|"
    "screen_blood.frag|screen_blood.frag|"
    "particle.frag|particle.frag|"
    "mesh.vert|mesh.vert|"
    "mesh.frag|mesh.frag|"
    "mesh_alpha_test.frag|mesh.frag|-DALPHA_TEST"
    "mesh_blend.frag|mesh.frag|-DBLEND"
    "world_map_land.frag|world_map_land.frag|"
    "world_map_object.frag|mesh.frag|-DWORLD_MAP"
    "world_map_blur.frag|world_map_blur.frag|"
    "local_map.frag|local_map.frag|"
    "effect.frag|effect.frag|"
    "effect_alpha_test.frag|effect.frag|-DALPHA_TEST"
    "effect_blend.frag|effect.frag|-DBLEND"
    "tonemap.vert|tonemap.vert|"
    "tonemap.frag|tonemap.frag|"
    "refraction.frag|refraction.frag|"
    "ao_minify.frag|ao_minify.frag|"
    "ao_raw.frag|ao_raw.frag|"
    "ao_blur.frag|ao_blur.frag|"
    "ssr_trace.frag|ssr_trace.frag|"
    "lens_down.frag|lens_down.frag|"
    "lens_streak.frag|lens_streak.frag|"
    "lens_blur.frag|lens_blur.frag|"
    "ssr_blur.frag|ssr_blur.frag|"
    "composite.frag|composite.frag|"
    "hdr_down4.frag|hdr_down4.frag|"
    "hdr_lum4.frag|hdr_lum4.frag|"
    "hdr_lum16.frag|hdr_lum16.frag|"
    "hdr_adapt.frag|hdr_adapt.frag|"
    "bloom_bright.frag|bloom_bright.frag|"
    "bloom_blur.frag|bloom_blur.frag|"
    "dof_blur.frag|dof_blur.frag|"
    "dof.frag|dof.frag|"
    "radial_blur.frag|radial_blur.frag|"
    "double_vision.frag|double_vision.frag|"
    "sky_mesh.vert|sky_mesh.vert|"
    "sky_mesh.frag|sky_mesh.frag|"
    "blur_mix.frag|blur_mix.frag|"
    "snow_sss.frag|snow_sss.frag|"
    "vl_volume.comp|vl_volume.comp|"
    "skin.comp|skin.comp|"
    "skin_decal.comp|skin_decal.comp|"
    "vl_apply.frag|vl_apply.frag|"
    "vl_blur.frag|vl_blur.frag|"
    "precipitation.vert|precipitation.vert|"
    "precipitation.frag|precipitation.frag|"
    "is_refraction.frag|is_refraction.frag|"
    "shadow.vert|shadow.vert|"
    "shadow_land.vert|shadow_land.vert|"
    "shadow.frag|shadow.frag|"
    "portal.vert|portal.vert|"
    "portal.frag|portal.frag|"
    "portal_copy.frag|portal_copy.frag|"
    "portal_stamp.frag|portal_stamp.frag|")

set(spud_spirv)
foreach(entry IN LISTS spud_shader_list)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 out_name)
    list(GET parts 1 src_name)
    list(GET parts 2 defines)
    set(out ${SPUD_SHADER_OUT}/${out_name}.spv)
    set(src ${SPUD_SHADER_SRC}/${src_name})
    if(defines STREQUAL "")
        set(define_args)
    else()
        set(define_args ${defines})
    endif()
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SPUD_SHADER_OUT}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -Werror -I${SPUD_SHADER_SRC}
                ${define_args} -MD -MF ${out}.d -o ${out} ${src}
        DEPENDS ${src} ${SPUD_SHADER_SRC}/common.glsl ${SPUD_SHADER_SRC}/post.glsl
        DEPFILE ${out}.d
        COMMENT "glslc ${out_name}"
        VERBATIM)
    list(APPEND spud_spirv ${out})
endforeach()
add_custom_target(spud_shaders ALL DEPENDS ${spud_spirv})
add_dependencies(spud_render spud_shaders)
