cmake_minimum_required(VERSION 3.28)
project(spud VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# OFF builds only the importer and skytool (no Vulkan/SDL), e.g. for import-only machines or CI.
option(SPUD_BUILD_ENGINE "Build the renderer, engine and spud viewer" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /wd4324 /wd4201)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    # Release is fully optimised (/O2 /Ob2, unlike RelWithDebInfo's /Ob1) and still writes PDBs, so
    # profilers and crash dumps resolve symbols in the build that perf runs measure.
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo,Release>:ProgramDatabase>")
    add_compile_options($<$<CONFIG:Release>:/Gw>)
    add_link_options($<$<CONFIG:Release>:/DEBUG:FULL> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

find_package(glm CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(Vorbis CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED COMPONENTS glslc)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image_write.h" REQUIRED)

# Every library under src/ globs its own sources so new files need no CMake edits.
function(spud_library name)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
    add_library(${name} STATIC ${sources})
    target_include_directories(${name} PUBLIC ${PROJECT_SOURCE_DIR}/src)
endfunction()

add_subdirectory(src)
add_subdirectory(apps)
