#!/usr/bin/env python3
"""Embed the reviewed schema manifest; generated output belongs in the build tree."""
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "database" / "mysql"))
import schema

target = Path(sys.argv[1])
target.parent.mkdir(parents=True, exist_ok=True)
manifest = json.dumps({"tables": sorted(schema.TABLES), "columns": schema.COLUMNS,
                       "indexes": schema.INDEXES, "checks": schema.CHECK_CLAUSES}, ensure_ascii=True, sort_keys=True)
target.write_text('#pragma once\nnamespace batt { inline constexpr const char* mysql_schema_manifest_json = R"manifest(' +
                  manifest + ')manifest"; }\n', encoding="utf-8")
