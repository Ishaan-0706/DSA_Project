cmake_minimum_required(VERSION 3.20)

project(BlockDedup VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ZLIB REQUIRED)
find_package(OpenMP REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EXIV2 REQUIRED exiv2)

add_library(blockdedup
    src/rolling_hash.cpp
    src/metadata.cpp
    src/image_tree.cpp
    src/image_similarity.cpp
    src/delta_compressor.cpp
)

target_include_directories(
    blockdedup
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
        ${EXIV2_INCLUDE_DIRS}
)

target_link_libraries(
    blockdedup
    PUBLIC
        ZLIB::ZLIB
        OpenMP::OpenMP_CXX
        ${EXIV2_LIBRARIES}
)

# Main Image Deduplication & Tree CLI
add_executable(image_dedup_cli src/main.cpp)
target_link_libraries(image_dedup_cli PRIVATE blockdedup OpenMP::OpenMP_CXX)

# Standalone metadata extractor
add_executable(mtdt_proc src/mtdt_processing.cpp)
target_include_directories(mtdt_proc PRIVATE ${EXIV2_INCLUDE_DIRS})
target_link_libraries(mtdt_proc PRIVATE ${EXIV2_LIBRARIES})

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
