spud_library(spud_physics)
target_link_libraries(spud_physics PUBLIC spud_formats glm::glm PRIVATE Jolt::Jolt)
# vcpkg builds Jolt's Release configuration with JPH_FLOATING_POINT_EXCEPTIONS_ENABLED but only exports that define to
# consumers built as Debug or Release. Jolt refuses to register its types when the defines differ, so the other
# configurations (our "release" preset is RelWithDebInfo) add it here.
target_compile_definitions(spud_physics PRIVATE
    $<$<CONFIG:RelWithDebInfo,MinSizeRel>:JPH_FLOATING_POINT_EXCEPTIONS_ENABLED>)
