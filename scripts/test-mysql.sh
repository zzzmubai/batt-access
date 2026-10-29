#!/bin/sh
set -eu
: "${BATT_MYSQL_TEST_CONFIG:?Set an absolute private test-admin JSON config; see V3 tutorial}"
project=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
build=${BATT_BUILD_DIR:-build-v3}
case "$build" in /*) ;; *) build="$project/$build" ;; esac
cmake -S "$project" -B "$build" -DCMAKE_BUILD_TYPE=Debug -DBATT_SANITIZE="${BATT_SANITIZE:-OFF}" -DBATT_THREAD_SANITIZE="${BATT_THREAD_SANITIZE:-OFF}"
cmake --build "$build" -j2
ctest --test-dir "$build" --output-on-failure
