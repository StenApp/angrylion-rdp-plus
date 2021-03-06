cmake_minimum_required(VERSION 2.8)

option(RETRACER "Set to ON to build the retracer tool" ${RETRACER})

project(angrylion-plus)

# check for INTERPROCEDURAL_OPTIMIZATION support
if((${CMAKE_VERSION} VERSION_EQUAL 3.9) OR (${CMAKE_VERSION} VERSION_GREATER 3.9))
    cmake_policy(SET CMP0069 NEW)

    include(CheckIPOSupported)
    check_ipo_supported(RESULT ENABLE_IPO)
    if(ENABLE_IPO)
        message("Interprocedural optimizations enabled")
    endif(ENABLE_IPO)
endif()

# C++14 is required for the Parallel utility class
set(CMAKE_CXX_STANDARD 14)

# default to release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif(NOT CMAKE_BUILD_TYPE)

find_package(Git REQUIRED)
find_package(PythonInterp 3.2 REQUIRED)
find_package(OpenGL REQUIRED)

# RDP core library and shared plugin library
set(PATH_CORE "core")
set(PATH_PLUGIN_COMMON "plugin-common")

# run script to generate version.h
set(PATH_VERSION_IN "${PATH_CORE}/version.h.in")
set(PATH_VERSION_OUT "${PATH_CORE}/version.h")

add_custom_command(
    OUTPUT ${PATH_VERSION_OUT}
    COMMAND
        ${PYTHON_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/make_version.py"
        "${CMAKE_SOURCE_DIR}/${PATH_VERSION_IN}"
        "${CMAKE_SOURCE_DIR}/${PATH_VERSION_OUT}"
    COMMENT "Generate Git version"
)

file(GLOB SOURCES_CORE "${PATH_CORE}/*.c" "${PATH_CORE}/*.cpp")
file(GLOB SOURCES_PLUGIN_COMMON "${PATH_PLUGIN_COMMON}/*.c")

add_library(alp-core STATIC ${SOURCES_CORE} ${PATH_VERSION_OUT})
add_library(alp-plugin-common STATIC ${SOURCES_PLUGIN_COMMON})

if(MINGW)
    # link libgcc/libstdc++ statically, fixes cryptic "_ZNSt13runtime_errorC1EPKc" error
    set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -static-libgcc -static-libstdc++")
else(MINGW)
    # set PIC option for non-MinGW targets
    set_target_properties(alp-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(alp-plugin-common PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif(MINGW)

# set IPO option, if supported
if(ENABLE_IPO AND (CMAKE_BUILD_TYPE STREQUAL "Release"))
    set_target_properties(alp-core PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
endif()

include_directories(${CMAKE_SOURCE_DIR})

# Project64 GFX Plugin (Windows only)
if(WIN32)
    set(NAME_PLUGIN_ZILMAR ${CMAKE_PROJECT_NAME})
    set(PATH_PLUGIN_ZILMAR "plugin-zilmar")

    file(GLOB SOURCES_PLUGIN_ZILMAR "${PATH_PLUGIN_ZILMAR}/*.c")
    add_library(${NAME_PLUGIN_ZILMAR} SHARED ${SOURCES_PLUGIN_ZILMAR})

    set_target_properties(${NAME_PLUGIN_ZILMAR} PROPERTIES PREFIX "")

    target_link_libraries(${NAME_PLUGIN_ZILMAR} alp-core alp-plugin-common shlwapi ${OPENGL_LIBRARIES})
endif(WIN32)

# Mupen64Plus GFX plugin
set(NAME_PLUGIN_M64P "mupen64plus-video-${CMAKE_PROJECT_NAME}")
set(PATH_PLUGIN_M64P "plugin-mupen64plus")

file(GLOB SOURCES_PLUGIN_M64P "${PATH_PLUGIN_M64P}/*.c")
add_library(${NAME_PLUGIN_M64P} SHARED ${SOURCES_PLUGIN_M64P})

set_target_properties(${NAME_PLUGIN_M64P} PROPERTIES PREFIX "")

target_link_libraries(${NAME_PLUGIN_M64P} alp-core alp-plugin-common ${OPENGL_LIBRARIES})

if(RETRACER)
    # RDP retracer
    set(NAME_RETRACER "${CMAKE_PROJECT_NAME}-retrace")
    set(PATH_RETRACER "retrace")
    set(LIB_RETRACER alp-core pthread)

    # optionally use SDL2 for visual output
    find_package(SDL2)
    if(SDL2_FOUND)
        set(LIB_RETRACER ${LIB_RETRACER} SDL2)
        add_definitions(-DRETRACE_SDL)
    endif(SDL2_FOUND)

    file(GLOB SOURCES_RETRACE "${PATH_RETRACER}/*.c")
    add_executable(${NAME_RETRACER} ${SOURCES_RETRACE})

    target_link_libraries(${NAME_RETRACER} ${LIB_RETRACER})
endif(RETRACER)
