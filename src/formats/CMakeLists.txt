spud_library(spud_formats)
target_link_libraries(spud_formats PUBLIC spud_core PRIVATE lz4::lz4)
