#!/bin/sh
set -eu
project=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
exec python3 "$project/scripts/init-mysql.py" "$@"
