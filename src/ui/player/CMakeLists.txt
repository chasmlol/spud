# The movie player has no renderer or windowing dependencies, so tools (skytool ui play) can run movies too.
spud_library(spud_ui_player)
target_link_libraries(spud_ui_player PUBLIC spud_formats)
