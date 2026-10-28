#!/usr/bin/env python3
"""Read-only inventory. This does not certify M0 or print database configuration."""
import argparse
import datetime
import importlib.metadata
import json
import os
import platform
import shutil
import subprocess
from pathlib import Path


def command(program, *arguments):
    executable = shutil.which(program)
    if executable is None:
        return {"available": False}
    try:
        result = subprocess.run([executable, *arguments], capture_output=True, text=True,
                                errors="replace", timeout=10, check=False)
        return {"available": True, "path": executable, "exit_code": result.returncode,
                "version_output": (result.stdout + result.stderr).strip()[:2000]}
    except (OSError, subprocess.TimeoutExpired):
        return {"available": True, "path": executable, "error": "version check failed"}


def collect(qmake="qmake", cmake="cmake", compiler="g++"):
    result = {"recorded_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "platform": platform.platform(), "python": platform.python_version(),
              "scope": "inventory only; M0 requires actual MySQL and Qt smoke results",
              "cmake": command(cmake, "--version"), "compiler": command(compiler, "--version"),
              "qmake": command(qmake, "-v"), "mysql_client": command("mysql_config", "--version"),
              "git": command("git", "rev-parse", "HEAD"), "blockers": []}
    try:
        result["pymysql"] = importlib.metadata.version("PyMySQL")
    except importlib.metadata.PackageNotFoundError:
        result["pymysql"] = None
    if os.name == "posix":
        os_release = Path("/etc/os-release")
        result["os_release"] = os_release.read_text() if os_release.exists() else "unavailable"
        if 'ID=ubuntu\n' not in result["os_release"] or 'VERSION_ID="24.04"' not in result["os_release"]:
            result["blockers"].append("server requires a separate Ubuntu 24.04 host")
        if not result["mysql_client"].get("version_output", "").startswith("8.4."):
            result["blockers"].append("Oracle MySQL 8.4 C API unavailable")
        if not result["pymysql"]:
            result["blockers"].append("independent Python MySQL driver unavailable")
    else:
        if "Qt version 6." not in result["qmake"].get("version_output", ""):
            result["blockers"].append("Windows Qt 6 development kit unavailable")
    for name in ("cmake", "compiler"):
        if not result[name].get("available") or result[name].get("exit_code") != 0:
            result["blockers"].append(name + " unavailable")
    result["inventory_ready"] = not result["blockers"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qmake", default="qmake")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = collect(args.qmake, args.cmake, args.compiler)
    encoded = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(encoded + "\n")
    print(encoded)
    return 0 if result["inventory_ready"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
