spud_library(spud_importer)
find_package(unofficial-libtess2 CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
target_link_libraries(spud_importer PUBLIC spud_formats PRIVATE ZLIB::ZLIB lz4::lz4 unofficial::libtess2::libtess2
                      Vorbis::vorbisenc meshoptimizer::meshoptimizer)
target_include_directories(spud_importer PRIVATE ${STB_INCLUDE_DIRS})

# The record schema and condition table (schema/*.txt), compiled in as data.
file(GLOB importer_schema_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/schema/*.txt)
set(importer_schema_cpp ${CMAKE_CURRENT_BINARY_DIR}/schema_embedded.cpp)
add_custom_command(OUTPUT ${importer_schema_cpp}
    COMMAND ${CMAKE_COMMAND} -DOUT=${importer_schema_cpp} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/schema
            -P ${CMAKE_CURRENT_SOURCE_DIR}/schema/embed.cmake
    DEPENDS ${importer_schema_files} ${CMAKE_CURRENT_SOURCE_DIR}/schema/embed.cmake)
target_sources(spud_importer PRIVATE ${importer_schema_cpp})
