"""MySQL initializer and independent manifest checks; no service Store dependency."""
import configparser
import os
from pathlib import Path
import re
import stat

TABLES = {"schema_meta", "device", "alarm", "query_task"}
CHECKS = {"meta_singleton", "meta_version", "meta_initialized", "device_id_length", "device_imsi_length",
          "alarm_event_positive", "alarm_status", "alarm_lifecycle", "query_status", "query_lifecycle",
          "query_response_json", "query_request_length"}
INDEXES = {
    "schema_meta": {"PRIMARY": (0, ["singleton"])},
    "device": {"PRIMARY": (0, ["dev_id"])},
    "alarm": {"PRIMARY": (0, ["id"]), "alarm_event": (0, ["dev_id", "event_no"]),
              "one_open_alarm": (0, ["dev_id", "alarm_id", "alarm_desc", "open_guard"]),
              "alarm_device_id": (1, ["dev_id", "id"]), "alarm_device_status_id": (1, ["dev_id", "status", "id"])},
    "query_task": {"PRIMARY": (0, ["txn_no"]), "one_pending_query": (0, ["dev_id", "pending_guard"]),
                   "unique_client_request": (0, ["client_request_id"])},
}
# Explicit column types/nullability. No default collation is used for identity.
COLUMNS = {
    "schema_meta": {"singleton": "tinyint", "application_id": "varbinary(32)", "schema_version": "int", "initialized": "tinyint"},
    "device": {"dev_id": "varbinary(64)", "imsi": "varbinary(64)", "first_seen_ms": "bigint", "last_login_ms": "bigint"},
    "alarm": {"id": "bigint", "dev_id": "varbinary(64)", "event_no": "bigint", "alarm_id": "varbinary(64)",
              "alarm_desc": "varbinary(64)", "status": "varbinary(16)", "first_ms": "bigint", "last_ms": "bigint",
              "clear_ms": "bigint?", "open_guard": "tinyint?"},
    "query_task": {"txn_no": "bigint", "dev_id": "varbinary(64)", "connection_id": "varbinary(128)?",
                   "status": "varbinary(16)", "reason": "varbinary(128)?", "created_ms": "bigint", "finished_ms": "bigint?",
                   "response_json": "longtext?", "client_request_id": "varbinary(64)?", "pending_guard": "tinyint?"},
}


def normalize_expression(value):
    # INFORMATION_SCHEMA may escape literal quotes in its SQL representation.
    return re.sub(r"[\s`()]+", "", value.lower()).replace("_binary", "").replace("\\'", "'").replace("octet_length", "length")


CHECK_CLAUSES = {name: normalize_expression(clause) for name, clause in re.findall(
    r"CONSTRAINT (\w+) CHECK \((.*)\)", Path(__file__).with_name("001_init.sql").read_text(encoding="utf-8"))}


def identifier(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,63}", value):
        raise ValueError("database must be an ASCII identifier, at most 64 bytes")
    return value


def read_config(filename):
    path = Path(filename)
    if not path.is_absolute():
        raise ValueError("absolute configuration path required")
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ValueError("private configuration owned by current user required")
    parser = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=None)
    parser.read(path, encoding="utf-8")
    section = parser["mysql"]
    if section["host"] != "127.0.0.1":
        raise ValueError("loopback MySQL required")
    import pymysql
    return pymysql.connect(host="127.0.0.1", port=section.getint("port"), database=identifier(section["database"]),
                           user=section["user"], password=section["password"], charset="utf8mb4",
                           connect_timeout=2, read_timeout=2, write_timeout=2, autocommit=True)


def validate(connection, require_complete=True):
    with connection.cursor() as cursor:
        cursor.execute("SELECT TABLE_NAME,ENGINE,TABLE_COLLATION FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE()")
        tables = cursor.fetchall()
        if {row[0] for row in tables} != TABLES or any(row[1:] != ("InnoDB", "utf8mb4_bin") for row in tables):
            raise ValueError("unexpected tables or engine/collation")
        for table, expected in COLUMNS.items():
            cursor.execute("SELECT COLUMN_NAME,COLUMN_TYPE,IS_NULLABLE,EXTRA,GENERATION_EXPRESSION FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=%s", (table,))
            columns = {row[0]: row[1:] for row in cursor.fetchall()}
            if set(columns) != set(expected):
                raise ValueError("column set mismatch")
            for name, spec in expected.items():
                kind, nullable, extra, expression = columns[name]
                if kind != spec.rstrip("?") or (nullable == "YES") != spec.endswith("?"):
                    raise ValueError("column type/null mismatch")
                if name in ("id", "txn_no") and "auto_increment" not in extra:
                    raise ValueError("auto increment missing")
                if name in ("open_guard", "pending_guard"):
                    terminal = "open" if name == "open_guard" else "pending"
                    normalized = normalize_expression(expression)
                    if extra != "STORED GENERATED" or normalized != "casewhenstatus='" + terminal + "'then1elsenullend":
                        raise ValueError("generated guard mismatch: " + name + "; extra=" + extra + "; expression=" + normalized)
            cursor.execute("SELECT INDEX_NAME,NON_UNIQUE,COLUMN_NAME FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME=%s ORDER BY INDEX_NAME,SEQ_IN_INDEX", (table,))
            indexes = {}
            for name, non_unique, column in cursor.fetchall():
                indexes.setdefault(name, (non_unique, []))[1].append(column)
            if indexes != INDEXES[table]:
                raise ValueError("index manifest mismatch")
        cursor.execute("SELECT CONSTRAINT_NAME,ENFORCED FROM information_schema.TABLE_CONSTRAINTS WHERE CONSTRAINT_SCHEMA=DATABASE() AND CONSTRAINT_TYPE='CHECK'")
        checks = cursor.fetchall()
        if {row[0] for row in checks} != CHECKS or any(row[1] != "YES" for row in checks):
            raise ValueError("check constraints missing or unenforced")
        cursor.execute("SELECT CONSTRAINT_NAME,CHECK_CLAUSE FROM information_schema.CHECK_CONSTRAINTS WHERE CONSTRAINT_SCHEMA=DATABASE()")
        if {name: normalize_expression(clause) for name, clause in cursor.fetchall()} != CHECK_CLAUSES:
            raise ValueError("check constraint expression mismatch")
        cursor.execute("SELECT CONSTRAINT_NAME,TABLE_NAME,COLUMN_NAME,REFERENCED_TABLE_NAME,REFERENCED_COLUMN_NAME FROM information_schema.KEY_COLUMN_USAGE WHERE CONSTRAINT_SCHEMA=DATABASE() AND REFERENCED_TABLE_NAME IS NOT NULL")
        if set(cursor.fetchall()) != {("alarm_device_fk", "alarm", "dev_id", "device", "dev_id"),
                                     ("query_device_fk", "query_task", "dev_id", "device", "dev_id")}:
            raise ValueError("foreign key manifest mismatch")
        cursor.execute("SELECT UPDATE_RULE,DELETE_RULE FROM information_schema.REFERENTIAL_CONSTRAINTS WHERE CONSTRAINT_SCHEMA=DATABASE()")
        if any(update not in ("RESTRICT", "NO ACTION") or delete not in ("RESTRICT", "NO ACTION") for update, delete in cursor.fetchall()):
            raise ValueError("foreign key action mismatch")
        cursor.execute("SELECT CHARACTER_SET_NAME,COLLATION_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='query_task' AND COLUMN_NAME='response_json'")
        if cursor.fetchone() != ("utf8mb4", "utf8mb4_bin"):
            raise ValueError("response character set mismatch")
        if require_complete:
            cursor.execute("SELECT singleton,application_id,schema_version,initialized FROM schema_meta")
            if cursor.fetchall() != ((1, b"batt-access", 1, 1),):
                raise ValueError("unknown or incomplete schema version")


def initialize(connection, stop_after=None):
    with connection.cursor() as cursor:
        cursor.execute("SELECT VERSION(),@@version_comment")
        version, vendor = cursor.fetchone()
        if not version.startswith("8.4.") or "MySQL" not in vendor:
            raise ValueError("Oracle MySQL 8.4 required")
        cursor.execute("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE()")
        if cursor.fetchone()[0]:
            raise ValueError("initializer requires an empty schema; existing objects preserved")
        cursor.execute("SET SESSION sql_mode='STRICT_ALL_TABLES,NO_ENGINE_SUBSTITUTION'")
        ddl = Path(__file__).with_name("001_init.sql").read_text(encoding="utf-8")
        ddl = "\n".join(line for line in ddl.splitlines() if not line.lstrip().startswith("--"))
        statements = [part.strip() for part in ddl.split(";") if part.strip()]
        for index, statement in enumerate(statements[:-1], 1):
            cursor.execute(statement)
            if stop_after == index:
                raise RuntimeError("controlled initialization interruption; schema retained incomplete")
        validate(connection, require_complete=False)
        cursor.execute(statements[-1])
    validate(connection)
