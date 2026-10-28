#!/usr/bin/env python3
"""Run with sudo only on the dedicated V3 VM, once. Never prints credentials."""
import grp
import os
from pathlib import Path
import pwd
import secrets
import subprocess


def main():
    if os.geteuid() != 0 or Path("/var/lib/cloud/data/instance-id").read_text().strip() != "batt-v3-m0-20261028":
        raise SystemExit("dedicated V3 VM and sudo required")
    user = pwd.getpwnam("book")
    private = Path(user.pw_dir) / "batt-access-v3" / ".local"
    private.mkdir(mode=0o700, exist_ok=True)
    os.chown(private, user.pw_uid, user.pw_gid)
    config = private / "mysql-m0.ini"
    if config.exists():
        raise SystemExit("existing M0 configuration preserved")
    password = secrets.token_urlsafe(32)  # Only ASCII alphanumeric, '-' and '_'.
    sql = ("CREATE DATABASE batt_test_m0 CHARACTER SET utf8mb4;\n"
           "CREATE USER 'batt_m0'@'127.0.0.1' IDENTIFIED BY '" + password + "';\n"
           "GRANT CREATE TEMPORARY TABLES ON batt_test_m0.* TO 'batt_m0'@'127.0.0.1';\n")
    result = subprocess.run(["mysql", "--protocol=socket", "--user=root"], input=sql,
                            text=True, capture_output=True, check=False)
    if result.returncode:
        raise SystemExit("fixture creation failed; no automatic cleanup; inspect database state locally")
    runtime = Path(user.pw_dir) / "batt-v3-runtime"
    runtime.mkdir(mode=0o700, exist_ok=True)
    os.chown(runtime, user.pw_uid, user.pw_gid)
    descriptor = os.open(config, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        stream.write("[mysql]\nhost=127.0.0.1\nport=3306\ndatabase=batt_test_m0\nuser=batt_m0\npassword=" +
                     password + "\nruntime_dir=" + str(runtime) + "\nconnect_timeout=2\nread_timeout=1\nwrite_timeout=1\n")
    os.chown(config, user.pw_uid, user.pw_gid)
    print("M0_FIXTURE_READY; credentials remain in the VM private configuration")


if __name__ == "__main__":
    main()
