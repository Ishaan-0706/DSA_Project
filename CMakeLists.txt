# how to use it :
# first run : cmake -S . -B build 
# Reads your CMakeLists.txt and generates the build files (makefiles) into a build/ folder, without compiling anything yet.
# then run : cmake --build build
# Runs the actual compiler using the build files CMake generated, turning your .cpp files into the library/executable.
# whenever change in cpp/hpp files run this command again.

cmake_minimum_required(VERSION 3.20)

project(LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library()

target_include_directories()

add_executable()
target_link_libraries()