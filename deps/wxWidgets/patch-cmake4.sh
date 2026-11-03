#!/bin/bash
# CMake 4.x rejects cmake_minimum_required(< 3.5) in wx cotire probe and root project.
set -euo pipefail
ROOT="${1:-.}"
cd "$ROOT"
for f in \
    CMakeLists.txt \
    build/cmake/modules/cotire.cmake \
    build/cmake/modules/cotire_test/CMakeLists.txt
do
    if [[ -f "$f" ]]; then
        sed -i '' 's/cmake_minimum_required(VERSION 2\.8\.12)/cmake_minimum_required(VERSION 3.5)/g' "$f"
    fi
done
