spud_library(spud_engine)
target_link_libraries(spud_engine PUBLIC spud_render spud_physics spud_ui spud_audio spud_script spud_anim spud_game SDL3::SDL3
                      imgui::imgui glm::glm)
target_include_directories(spud_engine PRIVATE ${STB_INCLUDE_DIRS})
# Reported in bench results.
target_compile_definitions(spud_engine PRIVATE SPUD_BUILD_CONFIG="$<CONFIG>")
if(WIN32)
    target_link_libraries(spud_engine PRIVATE ws2_32) # --control socket
endif()
