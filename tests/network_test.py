#!/usr/bin/env python3
"""Independent Python socket client checks the C++ programs on real Linux sockets."""
import json
import os
import pty
import queue
import socket
import subprocess
import sys
import threading
import tempfile
import time
import unittest
from mysql_probe import fixture

ACCESS, SIM = sys.argv[1:3]
sys.argv = sys.argv[:1]


def login(txn=1):
    return dict(msgType=110, devId='BT000', imsi='SIM000', devType=1, txnNo=txn)


def attributes(txn=2):
    return dict(msgType=310, devId='BT000', txnNo=txn, attrList=[
        dict(id='01111001', value=5280), dict(id='01112001', value=-125),
        dict(id='01113001', value=80), dict(id='01119001', value=28),
        dict(id='01102001', value=120.12)], metadata=dict(ok=True, other=None))


def wire(value):
    return json.dumps(value, separators=(',', ':')).encode('utf-8')


class Peer:
    def __init__(self, port):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=3)
        self.pending = ''

    def send(self, value):
        self.socket.sendall(wire(value))

    def receive(self, count=1, timeout=3):
        deadline = time.monotonic() + timeout
        result = []
        decoder = json.JSONDecoder()
        while len(result) < count:
            self.pending = self.pending.lstrip()
            try:
                value, end = decoder.raw_decode(self.pending)
            except ValueError:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AssertionError('response timeout; partial=' + repr(self.pending))
                self.socket.settimeout(remaining)
                chunk = self.socket.recv(4096)
                if not chunk:
                    raise AssertionError('server closed before complete response')
                self.pending += chunk.decode('utf-8')
            else:
                result.append(value)
                self.pending = self.pending[end:]
        return result

    def close(self):
        self.socket.close()


class Server:
    def __init__(self, *args, terminal=False):
        self.temp = tempfile.TemporaryDirectory(prefix='batt-network-')
        self.db_path = os.path.join(self.temp.name, 'test.db')
        if '--database-fixture' in args:
            self.db_path = args[args.index('--database-fixture') + 1]
        arguments = list(args)
        if '--database-fixture' in arguments:
            at = arguments.index('--database-fixture'); del arguments[at:at+2]
        self.database = fixture(self.db_path)
        master, slave = pty.openpty() if terminal else (None, None)
        self.process = subprocess.Popen([ACCESS, '--port', '0', '--db-config', self.database.config] + arguments,
                                        stdin=slave if terminal else subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        universal_newlines=True, bufsize=1)
        if terminal:
            os.close(slave)
            self.command_stream = os.fdopen(master, 'w', buffering=1)
        else:
            self.command_stream = self.process.stdin
        self.messages = []
        self.events = queue.Queue()
        self.reader = threading.Thread(target=self._read)
        self.reader.daemon = True
        self.reader.start()
        try:
            self.port = self.event('ready')['port']
        except BaseException:
            self.stop()
            raise

    def _read(self):
        for line in self.process.stdout:
            try:
                event = json.loads(line)
            except ValueError:
                event = dict(event='diagnostic', text=line.rstrip())
            self.messages.append(event)
            self.events.put(event)

    def event(self, kind, match=None, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                item = self.events.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if item.get('event') == kind and (match is None or match(item)):
                return item
        raise AssertionError('missing event %s; log=%r' % (kind, self.messages))

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            code = self.process.wait(timeout=7)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError('server did not terminate')
        self.reader.join(timeout=2)
        self.process.stdout.close()
        self.command_stream.close()
        self.temp.cleanup()
        return code

    def command(self, line):
        self.command_stream.write(line + '\n')
        self.command_stream.flush()

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        code = self.stop()
        if kind is None and code != 0:
            raise AssertionError('server failed: %r' % self.messages)


class NetworkTests(unittest.TestCase):
    def test_login_fragmented_then_glued_messages(self):
        with Server('--test-send-chunk', '1') as server:
            peer = Peer(server.port)
            try:
                for byte in wire(login()):
                    peer.socket.sendall(bytes([byte]))
                self.assertEqual(peer.receive()[0], dict(msgType=111, devId='BT000', txnNo=1, result=1))
                alarm = dict(msgType=410, devId='BT000', txnNo=3, alarmList=[
                    dict(id='01003001', alarmDesc='03', alarmFlag=1)])
                peer.socket.sendall(wire(attributes()) + wire(alarm))
                replies = peer.receive(2)
                self.assertEqual([x['msgType'] for x in replies], [311, 411])
                actual = server.event('attributes')
                self.assertEqual((actual['voltage_v'], actual['current_a'], actual['soc'], actual['temperature_c']),
                                 (52.8, -1.25, 80, 28))
                self.assertEqual(server.event('alarms')['open'], 1)
            finally:
                peer.close()

    def test_balanced_bad_json_does_not_eat_next_login(self):
        with Server() as server:
            peer = Peer(server.port)
            try:
                peer.socket.sendall(b'{"msgType":110,}' + wire(login()))
                self.assertEqual(peer.receive()[0]['msgType'], 111)
                server.event('bad_json')
                server.event('login')  # stdout reader is independent of socket ACK arrival.
                self.assertEqual(sum(x.get('event') == 'login' for x in server.messages), 1)
            finally:
                peer.close()

    def test_invalid_frame_is_atomic_and_does_not_ack(self):
        with Server() as server:
            peer = Peer(server.port)
            try:
                peer.send(login())
                peer.receive()
                peer.send(attributes())
                peer.receive()
                invalid = attributes(3)
                invalid['attrList'][0]['value'] = 6000
                invalid['attrList'][2]['value'] = 101
                # A legal unknown-only frame follows, so exactly one response is expected.
                marker = dict(msgType=310, devId='BT000', txnNo=4,
                              attrList=[dict(id='ignored', value=1.25)])
                peer.socket.sendall(wire(invalid) + wire(marker))
                self.assertEqual(peer.receive()[0]['txnNo'], 4)
                server.event('rejected', lambda e: e['reason'] == 'invalid_attributes')
            finally:
                peer.close()
            closed = server.event('closed')
            self.assertEqual(closed['snapshot']['voltage_v'], 52.8)
            self.assertEqual(closed['snapshot']['soc'], 80)

    def test_unauthed_message_does_not_change_state(self):
        with Server() as server:
            peer = Peer(server.port)
            try:
                peer.socket.sendall(wire(attributes()) + wire(login()))
                self.assertEqual(peer.receive()[0]['msgType'], 111)
                server.event('rejected', lambda e: e['reason'] == 'unbound_device')
            finally:
                peer.close()
            self.assertEqual(server.event('closed')['snapshot']['state'], 'connected_no_data')

    def test_simulator_alarm_lifecycle(self):
        with Server('--test-send-chunk', '2') as server:
            run = subprocess.run([SIM, '--port', str(server.port), '--slice', '1', '--devices', '1', '--hold-seconds', '0'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 universal_newlines=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stderr)
            events = [json.loads(line) for line in run.stdout.splitlines()]
            self.assertEqual(sum(e['event'] == 'ack' for e in events), 8)
            self.assertEqual(events[-1]['event'], 'demo_complete')
            closed = server.event('closed')
            self.assertEqual((closed['alarm_history'], closed['open_alarms']), (3, 1))

    def test_unclosed_frame_times_out_without_claiming_recovery(self):
        with Server() as server:
            peer = Peer(server.port)
            try:
                peer.send(login())
                peer.receive()
                peer.socket.sendall(b'{"unfinished":"text')
                closed = server.event('closed', timeout=7)
                self.assertEqual(closed['reason'], 'frame_timeout')
                peer.socket.settimeout(1)
                self.assertEqual(peer.socket.recv(1), b'')
            finally:
                peer.close()

    def test_closed_oversized_object_is_rejected(self):
        with Server() as server:
            peer = Peer(server.port)
            try:
                peer.send(dict(oversized='x' * (64 * 1024)))
                closed = server.event('closed')
                self.assertEqual(closed['reason'], 'object_too_large')
            finally:
                peer.close()

    def test_signal_exit_with_live_client(self):
        with Server() as server:
            peer = Peer(server.port)
            try:
                peer.send(login())
                peer.receive()
                self.assertEqual(server.stop(), 0)
                self.assertEqual(server.messages[-1]['event'], 'stopped')
            finally:
                peer.close()

    def test_32_concurrent_independent_devices(self):
        with Server() as server:
            barrier = threading.Barrier(32)
            outcomes = queue.Queue()
            def terminal(index):
                peer = None
                try:
                    peer = Peer(server.port)
                    hello = login(); hello['devId'] = 'BT%03d' % index
                    peer.send(hello); peer.receive()
                    barrier.wait(timeout=8)
                    frame = attributes(); frame['devId'] = hello['devId']
                    frame['attrList'][0]['value'] = 5000 + index
                    frame['attrList'][2]['value'] = index
                    peer.send(frame)
                    assert peer.receive()[0]['devId'] == hello['devId']
                    peer.send(dict(msgType=410, devId=hello['devId'], txnNo=3, alarmList=[
                        dict(id='01003001', alarmDesc='03', alarmFlag=1)]))
                    assert peer.receive()[0]['msgType'] == 411
                    outcomes.put(None)
                except Exception as error:
                    outcomes.put(repr(error))
                finally:
                    if peer is not None: peer.close()
            threads = [threading.Thread(target=terminal, args=(i,)) for i in range(32)]
            for thread in threads: thread.daemon = True; thread.start()
            for thread in threads: thread.join(timeout=10)
            self.assertTrue(all(not t.is_alive() for t in threads), 'terminal thread stuck')
            self.assertEqual([outcomes.get(timeout=1) for _ in threads], [None] * 32)
            self.assertEqual(server.stop(), 0)
            stats = server.messages[-1]
            self.assertGreaterEqual(stats['peak_clients'], 32)
            self.assertEqual(stats['registered_devices'], 32)
            updates = {e['devId']: e for e in server.messages if e.get('event') == 'attributes'}
            self.assertEqual(len(updates), 32)
            for i in range(32):
                self.assertEqual(updates['BT%03d' % i]['voltage_v'], (5000 + i) / 100.0)
                self.assertEqual(updates['BT%03d' % i]['soc'], i)

    def test_replacement_closes_only_old_connection(self):
        with Server() as server:
            old, other, new = Peer(server.port), Peer(server.port), None
            try:
                old.send(login()); old.receive()
                old.send(attributes()); old.receive()
                hello = login(); hello['devId'] = 'BT001'
                other.send(hello); other.receive()
                frame = attributes(); frame['devId'] = 'BT001'; frame['attrList'][0]['value'] = 6100
                other.send(frame); other.receive()
                new = Peer(server.port)
                new.send(login(10)); new.receive()
                replacement = server.event('session_replaced')
                self.assertNotEqual(replacement['old_connection_id'], replacement['connection_id'])
                self.assertEqual(server.event('login', lambda e: e['connection_id'] == replacement['connection_id'])['state'],
                                 'connected_no_data')
                forged = attributes(11); forged['attrList'][0]['value'] = 9999
                try: old.send(forged)
                except OSError: pass
                old.socket.settimeout(1)
                try: self.assertEqual(old.socket.recv(4096), b'')
                except ConnectionResetError: pass
                correct = attributes(12); correct['attrList'][0]['value'] = 6500
                new.send(correct); self.assertEqual(new.receive()[0]['txnNo'], 12)
                other.send(frame); self.assertEqual(other.receive()[0]['devId'], 'BT001')
                self.assertEqual(server.stop(), 0)
                updates = [e for e in server.messages if e.get('event') == 'attributes']
                self.assertFalse(any(e['voltage_v'] == 99.99 for e in updates))
                self.assertTrue(any(e['devId'] == 'BT000' and e['voltage_v'] == 65.0 for e in updates))
                self.assertTrue(any(e['devId'] == 'BT001' and e['voltage_v'] == 61.0 for e in updates))
                replaced = [e for e in server.messages if e.get('event') == 'closed' and e['reason'] == 'replaced']
                self.assertEqual(len(replaced), 1)
                self.assertFalse(replaced[0]['was_current'])
            finally:
                old.close(); other.close()
                if new: new.close()

    def test_invalid_new_login_does_not_kick_old_session(self):
        with Server() as server:
            old, invalid = Peer(server.port), Peer(server.port)
            try:
                old.send(login()); old.receive()
                bad = login(); bad['devType'] = 2
                invalid.send(bad)
                self.assertEqual(invalid.receive()[0]['result'], 0)
                old.send(attributes()); self.assertEqual(old.receive()[0]['result'], 1)
                self.assertEqual(server.stop(), 0)
                self.assertFalse(any(e.get('event') == 'session_replaced' for e in server.messages))
            finally:
                old.close(); invalid.close()

    def test_same_connection_login_is_idempotent(self):
        with Server() as server:
            peer = Peer(server.port)
            try:
                peer.send(login()); peer.receive()
                peer.send(attributes()); peer.receive()
                peer.send(login(3)); self.assertEqual(peer.receive()[0]['txnNo'], 3)
            finally:
                peer.close()
            closed = server.event('closed')
            self.assertEqual(closed['snapshot']['state'], 'online_fresh')
            self.assertEqual(closed['snapshot']['voltage_v'], 52.8)
            self.assertEqual(sum(e.get('event') == 'login' for e in server.messages), 1)

    def test_bound_connection_cannot_spoof_other_device(self):
        with Server() as server:
            a, b = Peer(server.port), Peer(server.port)
            try:
                a.send(login()); a.receive()
                hello = login(); hello['devId'] = 'BT001'; b.send(hello); b.receive()
                other = attributes(); other['devId'] = 'BT001'; other['attrList'][0]['value'] = 6100
                b.send(other); b.receive()
                spoof = attributes(3); spoof['devId'] = 'BT001'; spoof['attrList'][0]['value'] = 9999
                a.socket.sendall(wire(spoof) + wire(attributes(4)))
                self.assertEqual(a.receive()[0]['txnNo'], 4)
                self.assertEqual(server.stop(), 0)
                closed = [e for e in server.messages if e.get('event') == 'closed' and e['snapshot'].get('devId') == 'BT001']
                self.assertEqual(closed[-1]['snapshot']['voltage_v'], 61.0)
            finally:
                a.close(); b.close()

    def test_stale_state_retains_connection_and_alarm_does_not_refresh(self):
        with Server('--test-freshness-ms', '1000') as server:
            peer = Peer(server.port)
            try:
                peer.send(login()); peer.receive(); peer.send(attributes()); peer.receive()
                server.event('device_state', lambda e: e['state'] == 'online_fresh')
                time.sleep(0.1)
                partial = attributes(3); partial['attrList'] = [dict(id='01113001', value=81)]
                peer.send(partial); peer.receive()
                stale = server.event('device_state', lambda e: e['state'] == 'connected_stale', timeout=3)
                self.assertEqual(stale['soc'], 81)
                peer.send(dict(msgType=410, devId='BT000', txnNo=4, alarmList=[
                    dict(id='01003001', alarmDesc='000', alarmFlag=0)]))
                self.assertEqual(peer.receive()[0]['msgType'], 411)
            finally:
                peer.close()
            self.assertEqual(server.event('closed')['snapshot']['state'], 'connected_stale')

    def test_real_fd_reuse_has_distinct_connection_ids(self):
        with Server() as server:
            for i in range(4):
                peer = Peer(server.port)
                peer.send(login(i + 1)); peer.receive()
                peer.close(); server.event('closed')
            self.assertEqual(server.stop(), 0)
            accepts = [e for e in server.messages if e.get('event') == 'accepted']
            self.assertEqual(len(accepts), 4)
            self.assertLess(len(set(e['fd'] for e in accepts)), 4, 'test did not exercise actual fd reuse')
            self.assertEqual(len(set(e['connection_id'] for e in accepts)), 4)

    def test_connection_limit_does_not_stop_existing_clients(self):
        with Server('--max-clients', '2') as server:
            a, b, rejected = Peer(server.port), Peer(server.port), None
            try:
                a.send(login()); a.receive()
                hello = login(); hello['devId'] = 'BT001'; b.send(hello); b.receive()
                rejected = Peer(server.port)
                server.event('connection_rejected', lambda e: e['reason'] == 'connection_limit')
                a.send(attributes()); self.assertEqual(a.receive()[0]['result'], 1)
            finally:
                a.close(); b.close()
                if rejected: rejected.close()

    def test_cpp_simulator_32_simultaneous_connections(self):
        with Server() as server:
            run = subprocess.run([SIM, '--port', str(server.port), '--devices', '32', '--hold-seconds', '0'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 universal_newlines=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stderr)
            output = [json.loads(line) for line in run.stdout.splitlines()]
            self.assertEqual(output[-1], dict(event='demo_complete', devices=32, acks=256))
            self.assertEqual(sum(e['event'] == 'device_complete' for e in output), 32)
            self.assertEqual(server.stop(), 0)
            self.assertEqual(server.messages[-1]['peak_clients'], 32)
            self.assertEqual(server.messages[-1]['registered_devices'], 32)

    def test_bound_connection_cannot_rebind_using_login(self):
        with Server() as server:
            a, b = Peer(server.port), Peer(server.port)
            try:
                a.send(login()); a.receive(); a.send(attributes()); a.receive()
                hello = login(); hello['devId'] = 'BT001'; b.send(hello); b.receive()
                frame = attributes(); frame['devId'] = 'BT001'; frame['attrList'][0]['value'] = 6100
                b.send(frame); b.receive()
                a.send(hello)
                self.assertEqual(a.receive()[0]['result'], 0)
                b.send(frame); self.assertEqual(b.receive()[0]['devId'], 'BT001')
                self.assertEqual(server.stop(), 0)
                self.assertFalse(any(e.get('event') == 'session_replaced' for e in server.messages))
            finally:
                a.close(); b.close()

    def test_cpp_relogin_demo(self):
        with Server() as server:
            run = subprocess.run([SIM, '--port', str(server.port), '--demo', 'relogin'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 universal_newlines=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(json.loads(run.stdout.splitlines()[-1]),
                             dict(event='relogin_complete', old_connection_blocked=True, other_device_ok=True))


if __name__ == '__main__':
    unittest.main(verbosity=2)
