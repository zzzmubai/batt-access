#!/usr/bin/env python3
"""SSH test fixture: stdin newline/EOF stops its owned service and terminal."""
import json
from pathlib import Path
import sys
root=Path(__file__).resolve().parent.parent
sys.argv=[sys.argv[0],str(root/'build/batt-access'),str(root/'build/batt-sim')]
from http_test import HttpServer
from network_test import Peer
from query_network_test import fresh
with HttpServer() as server:
    peer=Peer(server.port)
    try:
        fresh(peer)
        print(json.dumps(dict(http_port=server.http_port)),flush=True)
        sys.stdin.readline()
    finally: peer.close()
print(json.dumps(dict(result='STOPPED')),flush=True)
