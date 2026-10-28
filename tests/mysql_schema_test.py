#!/usr/bin/env python3
"""M1: independent SQL constraints, initialization failures and EXPLAIN evidence."""
import json
import sys

import pymysql
from mysql_probe import TestDatabase, schema

checks = 0


def require(value, message):
    global checks
    checks += 1
    if not value:
        raise AssertionError(message)


def refused(function, expected):
    try:
        function()
    except expected:
        require(True, "expected refusal")
    else:
        require(False, "operation unexpectedly accepted")


def constraints():
    with TestDatabase() as database, database.connect() as current:
        schema.validate(current)
        with current.cursor() as cursor:
            def reject(sql, args, code):
                try:
                    cursor.execute(sql, args)
                except pymysql.MySQLError as error:
                    require(error.args[0] == code, "wrong SQL rejection classification")
                else:
                    require(False, "SQL constraint did not reject")
            device = "INSERT INTO device VALUES(%s,%s,1,2)"
            for name in (b"BT", b"bt", b"BT ", b"x" * 64, "电池".encode()):
                cursor.execute(device, (name, b"SIM"))
            cursor.execute("SELECT COUNT(*) FROM device")
            require(cursor.fetchone()[0] == 5, "case/trailing-space/UTF-8 identity lost")
            reject(device, (b"x" * 65, b"SIM"), 1406)
            reject(device, (b"", b"SIM"), 3819)
            reject(device, (b"BT", b"SIM"), 1062)
            alarm = "INSERT INTO alarm(dev_id,event_no,alarm_id,alarm_desc,status,first_ms,last_ms,clear_ms) VALUES(%s,%s,'01003001','03',%s,1,2,%s)"
            for event in (1, 2):
                cursor.execute(alarm, (b"BT", event, b"cleared", 2))
            cursor.execute(alarm, (b"BT", 3, b"open", None))
            reject(alarm, (b"BT", 4, b"open", None), 1062)
            reject(alarm, (b"BT", 2, b"cleared", 2), 1062)
            reject(alarm, (b"missing", 1, b"open", None), 1452)
            reject(alarm, (b"BT", 4, b"OPEN", None), 3819)
            reject(alarm, (b"BT", 4, b"open ", None), 3819)
            reject(alarm, (b"BT", 4, b"open", 2), 3819)
            reject(alarm, (b"BT", 0, b"cleared", 2), 3819)
            query = "INSERT INTO query_task(dev_id,status,created_ms,finished_ms,client_request_id,response_json) VALUES(%s,%s,1,%s,%s,%s)"
            cursor.execute(query, (b"BT", b"pending", None, b"request-A", None))
            reject(query, (b"BT", b"pending", None, b"new", None), 1062)
            reject(query, (b"bt", b"pending", None, b"request-A", None), 1062)
            for response in (None, "{}", "[]"):
                cursor.execute(query, (b"BT", b"success", 2, None, response))
            cursor.execute(query, (b"bt", b"pending", None, b"request-a", None))
            reject(query, (b"BT", b"unknown", 2, None, None), 3819)
            reject(query, (b"BT", b"timeout", None, None, None), 3819)
            reject(query, (b"BT", b"success", 2, None, "invalid JSON"), 3819)
            reject(query, (b"BT", b"success", 2, b"", None), 3819)
            reject(query, (b"BT", b"success", 2, b"a" * 65, None), 1406)
            cursor.execute("SELECT response_json FROM query_task WHERE client_request_id IS NULL ORDER BY txn_no")
            require(cursor.fetchall() == ((None,), ("{}",), ("[]",)), "NULL/empty object/empty array conflated")
            large = 9007199254740993
            cursor.execute("INSERT INTO query_task(txn_no,dev_id,status,created_ms,finished_ms) VALUES(%s,'BT','success',1,2)", (large,))
            cursor.execute("SELECT txn_no FROM query_task WHERE txn_no=%s", (large,))
            require(cursor.fetchone()[0] == large, "64-bit task precision lost")
        with database.connect(runtime=True) as runtime:
            schema.validate(runtime)
            with runtime.cursor() as cursor:
                try:
                    cursor.execute("CREATE TABLE forbidden(id INT)")
                except pymysql.MySQLError as error:
                    require(error.args[0] in (1044, 1142), "runtime DDL denial changed")
                else:
                    require(False, "runtime account has DDL privileges")
        with current.cursor() as cursor:
            cursor.executemany("INSERT INTO alarm(dev_id,event_no,alarm_id,alarm_desc,status,first_ms,last_ms,clear_ms) VALUES('BT',%s,'01003001','06','cleared',1,2,2)", [(number,) for number in range(100, 1100)])
            cursor.execute("ANALYZE TABLE alarm")
            cursor.fetchall()
            cursor.execute("EXPLAIN FORMAT=JSON SELECT id FROM alarm WHERE dev_id='BT' AND status='cleared' AND id<1000 ORDER BY id DESC LIMIT 21")
            plan = json.loads(cursor.fetchone()[0])
            print(json.dumps({"explain": plan}, ensure_ascii=False))


def initialization():
    with TestDatabase(initialize=False) as database, database.connect() as current:
        refused(lambda: schema.initialize(current, stop_after=2), RuntimeError)
        refused(lambda: schema.validate(current), ValueError)
        refused(lambda: schema.initialize(current), ValueError)
        with current.cursor() as cursor:
            cursor.execute("SELECT COUNT(*) FROM schema_meta")
            require(cursor.fetchone()[0] == 0, "interrupted DDL published completion")
    with TestDatabase() as database, database.connect() as current:
        refused(lambda: schema.initialize(current), ValueError)
        with current.cursor() as cursor:
            cursor.execute("UPDATE schema_meta SET initialized=0")
        refused(lambda: schema.validate(current), ValueError)
        with current.cursor() as cursor:
            cursor.execute("UPDATE schema_meta SET initialized=1,application_id='unknown'")
        refused(lambda: schema.validate(current), ValueError)
        with current.cursor() as cursor:
            cursor.execute("UPDATE schema_meta SET application_id='batt-access'")
            cursor.execute("UPDATE schema_meta SET schema_version=2")
        refused(lambda: schema.validate(current), ValueError)
        with current.cursor() as cursor:
            cursor.execute("UPDATE schema_meta SET schema_version=1")
            cursor.execute("ALTER TABLE query_task DROP INDEX unique_client_request")
        refused(lambda: schema.validate(current), ValueError)
    with TestDatabase() as database, database.connect() as current:
        with current.cursor() as cursor:
            cursor.execute("ALTER TABLE query_task DROP CHECK query_response_json")
            cursor.execute("ALTER TABLE query_task ADD CONSTRAINT query_response_json CHECK (1=1)")
        refused(lambda: schema.validate(current), ValueError)
    with TestDatabase(initialize=False) as database, database.connect() as current:
        with current.cursor() as cursor:
            cursor.execute("CREATE TABLE unrelated(id INT)")
        refused(lambda: schema.initialize(current), ValueError)
        with current.cursor() as cursor:
            cursor.execute("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='unrelated'")
            require(cursor.fetchone()[0] == 1, "unknown table removed by initialization")


if __name__ == "__main__":
    try:
        constraints()
        initialization()
        print(json.dumps({"result": "M1_SCHEMA_PASS", "checks": checks}))
    except Exception as error:
        # Controlled assertion messages contain no SQL parameters or credentials.
        detail = str(error) if isinstance(error, (AssertionError, ValueError)) else ""
        code = error.args[0] if isinstance(error, pymysql.MySQLError) else None
        print(json.dumps({"result": "FAIL", "category": type(error).__name__, "checks": checks,
                          "detail": detail, "mysql_code": code}), file=sys.stderr)
        raise SystemExit(1)
