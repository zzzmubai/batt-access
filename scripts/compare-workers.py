#!/usr/bin/env python3
"""Run only the repeatable one-vs-two-worker fault-injection comparison."""
import argparse
from pathlib import Path
import sys
import unittest

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', default='build')
args = parser.parse_args()
build = root / args.build_dir
sys.path.insert(0, str(root / 'tests'))
sys.argv = [sys.argv[0], str(build / 'batt-access'), str(build / 'batt-sim')]
suite = unittest.defaultTestLoader.loadTestsFromName(
    'query_network_test.QueryNetworkTests.test_worker_isolation_comparison')
result = unittest.TextTestRunner(verbosity=2).run(suite)
sys.exit(0 if result.wasSuccessful() else 1)
