# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.18)
project(abouthook LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)

# Prebuilt LSPlant + Dobby (copied from OriginOS6/face/libs/arm64-v8a).
add_library(lsplant SHARED IMPORTED)
set_target_properties(lsplant PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libs/arm64-v8a/liblsplant.so)

add_library(dobby SHARED IMPORTED)
set_target_properties(dobby PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libs/arm64-v8a/libdobby.so)

add_library(abouthook SHARED hook/libabouthook.cpp)
target_include_directories(abouthook PRIVATE hook)
target_link_libraries(abouthook lsplant dobby log)
set_target_properties(abouthook PROPERTIES
    BUILD_RPATH "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN")

add_executable(abouthookd injector/abouthookd.c)
target_link_libraries(abouthookd log)
