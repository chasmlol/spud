file(GLOB skytool_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
add_executable(skytool ${skytool_sources})
target_link_libraries(skytool PRIVATE spud_importer spud_script spud_game spud_ui_player spud_fx spud_game)
