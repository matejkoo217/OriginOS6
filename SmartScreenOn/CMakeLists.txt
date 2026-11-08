# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.18)
project(smartscreenon LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)

add_library(lsplant SHARED IMPORTED)
set_target_properties(lsplant PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libs/arm64-v8a/liblsplant.so)

add_library(dobby SHARED IMPORTED)
set_target_properties(dobby PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libs/arm64-v8a/libdobby.so)

add_library(smartscreenon SHARED hook/libsmartscreenon.cpp)
target_include_directories(smartscreenon PRIVATE hook)
target_link_libraries(smartscreenon lsplant dobby log)
set_target_properties(smartscreenon PROPERTIES
    BUILD_RPATH "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN")

add_executable(smartscreenond injector/smartscreenond.c)
target_link_libraries(smartscreenond log)
