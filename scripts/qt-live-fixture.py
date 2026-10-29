#!/usr/bin/env python3
"""Owned V3 service + C++ simulator for the real Windows Qt/SSH integration test."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir',default='build-v3')
args=parser.parse_args(); build=(root/args.build_dir).resolve()
sys.path.insert(0,str(root/'tests')); sys.argv=[sys.argv[0],str(build/'batt-access'),str(build/'batt-sim')]
from http_test import HttpServer
from mysql_probe import cleanup_fixtures

try:
    with HttpServer() as server:
        simulator=subprocess.Popen([str(build/'batt-sim'),'--port',str(server.port),'--demo','serve','--devices','1'],
            stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
        try:
            server.event('alarms',lambda row: row.get('devId')=='BT000')
            print(json.dumps({'ready':True,'http_port':server.http_port,'device_port':server.port}),flush=True)
            sys.stdin.readline()
        finally:
            simulator.terminate(); simulator.communicate(timeout=5)
finally:
    cleanup_fixtures()
