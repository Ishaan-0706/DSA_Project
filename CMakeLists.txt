cmake_minimum_required(VERSION 3.20)

project(BlockDedup VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(blockdedup src/rolling_hash.cpp)
target_include_directories(
    blockdedup
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()