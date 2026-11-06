cmake_minimum_required(VERSION 3.20)

project(BlockDedup VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ZLIB REQUIRED)
find_package(OpenMP)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(EXIV2 exiv2)
endif()

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

target_link_libraries(blockdedup PUBLIC ZLIB::ZLIB)
# shortcut: without OpenMP the chunk loops run one thread, install libomp to parallelize.
if(OpenMP_CXX_FOUND)
    target_link_libraries(blockdedup PUBLIC OpenMP::OpenMP_CXX)
    target_compile_definitions(blockdedup PUBLIC BLOCKDEDUP_HAS_OPENMP)
endif()
# shortcut: without libexiv2, extract() keeps size and mtime only, install exiv2 for EXIF.
if(EXIV2_FOUND)
    target_include_directories(blockdedup PUBLIC ${EXIV2_INCLUDE_DIRS})
    target_link_libraries(blockdedup PUBLIC ${EXIV2_LIBRARIES})
    target_compile_definitions(blockdedup PUBLIC BLOCKDEDUP_HAS_EXIV2)
endif()

# Main Image Deduplication & Tree CLI
add_executable(image_dedup_cli src/main.cpp)
target_link_libraries(image_dedup_cli PRIVATE blockdedup)

if(EXIV2_FOUND)
    add_executable(mtdt_proc src/mtdt_processing.cpp)
    target_include_directories(mtdt_proc PRIVATE ${EXIV2_INCLUDE_DIRS})
    target_link_libraries(mtdt_proc PRIVATE ${EXIV2_LIBRARIES})
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
