#!/usr/bin/env python3
"""Initialize a named empty V3 schema; no CREATE DATABASE, DROP, or repair."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "database" / "mysql"))
import schema

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-config", required=True)
    args = parser.parse_args()
    try:
        with schema.read_config(args.db_config) as connection:
            schema.initialize(connection)
        print(json.dumps({"result": "SCHEMA_INITIALIZED", "version": 1}))
    except Exception as error:
        # Never print a driver error containing connection credentials or SQL values.
        print(json.dumps({"result": "FAIL", "category": type(error).__name__}), file=sys.stderr)
        raise SystemExit(1)
