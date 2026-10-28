#!/bin/sh
# Run on the NEW Ubuntu 24.04 host only. No apt install, schema deletion, or V2 writes.
set -eu
if [ "$#" -ne 2 ]; then
  echo 'usage: sh scripts/check-v3-m0.sh /absolute/private-config.ini /absolute/new-evidence-dir' >&2
  exit 2
fi
config=$1
evidence=$2
case "$config:$evidence" in /*:/*) ;; *) echo 'absolute paths required' >&2; exit 2 ;; esac
project=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
# An existing result directory is evidence, never an output location to overwrite.
umask 077
mkdir "$evidence"
python3 "$project/scripts/v3-environment.py" --output "$evidence/environment.json" > "$evidence/inventory.txt"
cmake -S "$project/tools/m0" -B "$project/build-m0" -DCMAKE_BUILD_TYPE=Debug > "$evidence/configure.txt" 2>&1
cmake --build "$project/build-m0" -j2 > "$evidence/build.txt" 2>&1
ctest --test-dir "$project/build-m0" --output-on-failure > "$evidence/ctest.txt" 2>&1
"$project/build-m0/mysql_smoke" "$config" > "$evidence/mysql.txt" 2>&1
python3 "$project/tools/m0/python_probe.py" "$config" > "$evidence/python.txt" 2>&1
printf '%s\n' 'Linux admission passed. Windows Qt smoke and V2 baseline replay are still required.'
