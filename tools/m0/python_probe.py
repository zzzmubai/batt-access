#!/usr/bin/env python3
"""Independent driver admission. No persistent tables and no credential output."""
import configparser
import importlib.metadata
import json
import os
import re
import stat
import sys
from pathlib import Path


def probe(filename):
    path = Path(filename)
    if not path.is_absolute():
        raise ValueError("absolute private config required")
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) & 0o077:
        raise ValueError("config must be owned by this user with mode 600")
    config = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=None)
    config.read(path, encoding="utf-8")
    section = config["mysql"]
    database = section["database"]
    if section["host"] != "127.0.0.1" or not re.fullmatch(r"batt_test_[A-Za-z0-9_]+", database) or len(database) > 64:
        raise ValueError("loopback and batt_test_ schema required")
    if section["password"] == "REPLACE_ME":
        raise ValueError("replace example credentials locally")
    import pymysql
    with pymysql.connect(host="127.0.0.1", port=section.getint("port"), user=section["user"],
                         password=section["password"], database=database, charset="utf8mb4",
                         connect_timeout=section.getint("connect_timeout"),
                         read_timeout=section.getint("read_timeout"), write_timeout=section.getint("write_timeout"),
                         autocommit=False) as connection:
        with connection.cursor() as cursor:
            cursor.execute("SELECT VERSION(),@@version_comment")
            version, comment = cursor.fetchone()
            if not version.startswith("8.4.") or "MySQL" not in comment:
                raise ValueError("Oracle MySQL 8.4 required")
            cursor.execute("CREATE TEMPORARY TABLE batt_python_m0(id BIGINT PRIMARY KEY, data VARBINARY(64), optional LONGTEXT) ENGINE=InnoDB")
            cursor.execute("INSERT INTO batt_python_m0 VALUES(%s,%s,%s)", (9007199254740993, b"A\x00z ", None))
            cursor.execute("SELECT id,data,optional FROM batt_python_m0")
            if cursor.fetchone() != (9007199254740993, b"A\x00z ", None):
                raise ValueError("independent driver roundtrip failed")
            connection.rollback()
            cursor.execute("SELECT COUNT(*) FROM batt_python_m0")
            if cursor.fetchone()[0] != 0:
                raise ValueError("independent driver rollback failed")
    print(json.dumps({"result": "M0_PYTHON_PASS", "driver": importlib.metadata.version("PyMySQL"), "server": version}))


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2:
            raise ValueError("usage: python_probe.py /absolute/private-config.ini")
        probe(sys.argv[1])
    except Exception as error:
        # Driver exceptions can contain credentials, account names, and server messages.
        print(json.dumps({"result": "FAIL", "category": type(error).__name__}), file=sys.stderr)
        raise SystemExit(1)
