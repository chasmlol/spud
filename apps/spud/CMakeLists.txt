file(GLOB spud_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
add_executable(spud ${spud_sources})
target_link_libraries(spud PRIVATE spud_engine)
