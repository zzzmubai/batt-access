#!/usr/bin/env python3
"""Give a C++ test isolated initialized schemas and private runtime configs."""
import argparse
from contextlib import ExitStack
import os
import subprocess

from mysql_probe import TestDatabase

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary")
parser.add_argument("--databases", type=int, default=1, choices=(1, 2))
args = parser.parse_args()
with ExitStack() as stack:
    environment = os.environ.copy()
    for index in range(args.databases):
        fixture = stack.enter_context(TestDatabase())
        environment["BATT_TEST_DB_CONFIG_" + str(index)] = fixture.config
        if index == 0:
            environment["BATT_TEST_DB_CONFIG"] = fixture.config
    raise SystemExit(subprocess.call([args.binary], env=environment))
