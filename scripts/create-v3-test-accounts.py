#!/usr/bin/env python3
"""One-time scoped MySQL test identities on our dedicated VM. No password output."""
import json
import os
from pathlib import Path
import pwd
import secrets
import subprocess


def main():
    if os.geteuid() != 0 or Path("/var/lib/cloud/data/instance-id").read_text().strip() != "batt-v3-m0-20261028":
        raise SystemExit("dedicated V3 VM and sudo required")
    user = pwd.getpwnam("book")
    path = Path(user.pw_dir) / "batt-access-v3" / ".local" / "mysql-test-admin.json"
    if path.exists():
        raise SystemExit("existing test account configuration preserved")
    admin_password, runtime_password = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
    # Escaped underscores are literal; only the trailing '%' is a schema wildcard.
    sql = ("CREATE USER 'batt_test_admin'@'127.0.0.1' IDENTIFIED BY '" + admin_password + "';\n"
           "CREATE USER 'batt_test_runtime'@'127.0.0.1' IDENTIFIED BY '" + runtime_password + "';\n"
           "GRANT ALL PRIVILEGES ON `batt\\_test\\_%`.* TO 'batt_test_admin'@'127.0.0.1';\n"
           "GRANT SELECT,INSERT,UPDATE,DELETE ON `batt\\_test\\_%`.* TO 'batt_test_runtime'@'127.0.0.1';\n")
    result = subprocess.run(["mysql", "--protocol=socket", "--user=root"], input=sql, text=True,
                            capture_output=True, check=False)
    if result.returncode:
        raise SystemExit("account initialization failed; inspect locally; no automatic deletion")
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        json.dump({"host": "127.0.0.1", "port": 3306, "admin_user": "batt_test_admin",
                   "admin_password": admin_password, "runtime_user": "batt_test_runtime",
                   "runtime_password": runtime_password, "runtime_dir": "/home/book/batt-v3-runtime"}, stream)
    os.chown(path, user.pw_uid, user.pw_gid)
    print("V3_TEST_ACCOUNTS_READY; private configuration remains on the VM")


if __name__ == "__main__":
    main()
