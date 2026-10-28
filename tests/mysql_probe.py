"""Independent MySQL fixtures. Only schemas CREATED by this process can be dropped."""
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import uuid

import pymysql

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "database" / "mysql"))
import schema

_created = set()


def options():
    path = Path(os.environ["BATT_MYSQL_TEST_CONFIG"])
    info = path.stat()
    if not path.is_absolute() or not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ValueError("test credentials require an absolute, owner-only file")
    value = json.loads(path.read_text(encoding="utf-8"))
    if value["host"] != "127.0.0.1" or not isinstance(value["port"], int) or not 1 <= value["port"] <= 65535:
        raise ValueError("test database must use loopback")
    runtime = Path(value["runtime_dir"])
    if not runtime.is_absolute() or not runtime.is_dir():
        raise ValueError("fixed runtime directory unavailable")
    return value


def connection(value, database=None, runtime=False):
    prefix = "runtime" if runtime else "admin"
    result = pymysql.connect(host=value["host"], port=value["port"], user=value[prefix + "_user"],
                             password=value[prefix + "_password"], database=database, charset="utf8mb4",
                             connect_timeout=2, read_timeout=2, write_timeout=2, autocommit=True)
    with result.cursor() as cursor:
        cursor.execute("SET SESSION sql_mode='STRICT_ALL_TABLES,NO_ENGINE_SUBSTITUTION'")
        cursor.execute("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED")
        cursor.execute("SET SESSION innodb_lock_wait_timeout=1")
    return result


class TestDatabase:
    def __init__(self, initialize=True):
        self.options = options()
        self.name = "batt_test_" + uuid.uuid4().hex
        schema.identifier(self.name)
        self.directory = tempfile.TemporaryDirectory(prefix="batt-mysql-")
        self.config = str(Path(self.directory.name) / "mysql.ini")
        with connection(self.options) as admin:
            with admin.cursor() as cursor:
                cursor.execute("CREATE DATABASE `" + self.name + "` CHARACTER SET utf8mb4 COLLATE utf8mb4_bin")
        _created.add(self.name)
        self.record("created")
        try:
            if initialize:
                with self.connect() as current:
                    schema.initialize(current)
            descriptor = os.open(self.config, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                stream.write("[mysql]\nhost=127.0.0.1\nport=" + str(self.options["port"]) + "\ndatabase=" + self.name +
                             "\nuser=" + self.options["runtime_user"] + "\npassword=" + self.options["runtime_password"] +
                             "\nruntime_dir=" + self.options["runtime_dir"] + "\nconnect_timeout=2\nread_timeout=1\nwrite_timeout=1\n")
        except BaseException:
            self.close()
            raise

    def record(self, action):
        path = Path(self.options["runtime_dir"]) / ("test-owned-" + str(os.getpid()) + ".jsonl")
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        with os.fdopen(descriptor, "a", encoding="utf-8") as stream:
            stream.write(json.dumps({"schema": self.name, "action": action, "pid": os.getpid()}) + "\n")

    def connect(self, runtime=False):
        return connection(self.options, self.name, runtime)

    def close(self):
        if self.name not in _created:
            return
        # All conditions are required: loopback, fixed prefix, valid identifier,
        # and this process's CREATE-success registry. Never clean arbitrary configs.
        if self.options["host"] != "127.0.0.1" or not self.name.startswith("batt_test_"):
            raise ValueError("refusing unsafe cleanup")
        schema.identifier(self.name)
        with connection(self.options) as admin:
            with admin.cursor() as cursor:
                cursor.execute("DROP DATABASE `" + self.name + "`")
        _created.remove(self.name)
        self.record("dropped")
        self.directory.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
