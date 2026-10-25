#!/bin/sh
set -eu
project_dir=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build_dir=${BATT_BUILD_DIR:-build}
case "$build_dir" in
  /*) ;;
  *) build_dir="$project_dir/$build_dir" ;;
esac
mkdir -p "$build_dir"
cd "$build_dir"
# Compatible with the existing Ubuntu 18.04 CMake 3.10 (no -S/-B or --test-dir).
cmake "$project_dir" -DCMAKE_BUILD_TYPE=Debug -DBATT_SANITIZE="${BATT_SANITIZE:-OFF}" -DBATT_THREAD_SANITIZE="${BATT_THREAD_SANITIZE:-OFF}"
cmake --build . -- -j2
ctest --output-on-failure
