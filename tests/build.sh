#!/usr/bin/env bash
set -e
mkdir -p build/bin
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build
echo "Build successful! Executables created in build/:"
echo "  - ./build/mtdt_proc"
echo "  - ./build/image_dedup_cli"
