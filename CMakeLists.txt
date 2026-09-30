cmake_minimum_required(VERSION 3.20)
project(letterlock CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Windows: vcpkg (vcpkg.json). Linux: the distro's SDL2 / SDL2_ttf dev packages
# (the Makefile is the simpler way to build there).
find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)

if(WIN32)
    add_executable(letterlock WIN32 letterlock.cpp letterlock.rc)   # .rc embeds icon.ico
else()
    add_executable(letterlock letterlock.cpp)
endif()
target_include_directories(letterlock PRIVATE ${CMAKE_SOURCE_DIR})
if(MSVC)
    target_compile_options(letterlock PRIVATE /utf-8 /bigobj)
    target_compile_definitions(letterlock PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_link_libraries(letterlock PRIVATE
    $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    $<IF:$<TARGET_EXISTS:SDL2_ttf::SDL2_ttf>,SDL2_ttf::SDL2_ttf,SDL2_ttf::SDL2_ttf-static>
)
