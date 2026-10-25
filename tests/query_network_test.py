#!/usr/bin/env python3
import json
import socket
import struct
import subprocess
import threading
import time
import unittest
from network_test import Server, Peer, login, attributes, wire, SIM


def fresh(peer, dev='BT000'):
    hello = login(); hello['devId'] = dev; peer.send(hello); peer.receive()
    frame = attributes(); frame['devId'] = dev; peer.send(frame); peer.receive()


def answer(request, attrs=True):
    frame = dict(msgType=911, devId=request['devId'], txnNo=request['txnNo'])
    if attrs:
        frame['attrList'] = [dict(id='8001', protect=4150, recovery=4100, delay=1000)]
    return frame


def begin(server, peer, dev='BT000'):
    server.command('query ' + dev)
    pending = server.event('query_state', lambda e: e['devId'] == dev and e['state'] == 'pending')
    request = peer.receive()[0]
    if request['msgType'] != 910 or request['txnNo'] != pending['txnNo']:
        raise AssertionError('query envelope mismatch')
    return pending, request


class QueryNetworkTests(unittest.TestCase):
    def test_quiet_console_keeps_status_and_query_results_visible(self):
        with Server('--quiet', '--test-query-timeout-ms', '300') as server:
            peer = Peer(server.port)
            try:
                fresh(peer)
                server.command('status BT000')
                row = server.event('diagnostic', lambda e: e['text'].startswith('[STATUS]'))['text']
                self.assertIn('online_fresh', row)
                self.assertIn('52.80V', row)
                self.assertFalse(any(e.get('event') == 'attributes' for e in server.messages))
                server.command('query BT000')
                request = peer.receive()[0]
                self.assertEqual(request['msgType'], 910)
                server.event('diagnostic', lambda e: '[QUERY #' in e['text'] and 'pending' in e['text'])
                peer.send(answer(request))
                server.event('diagnostic', lambda e: '[QUERY #' in e['text'] and 'success' in e['text'])
                result = server.event('diagnostic', lambda e: e['text'].startswith('[RESULT #'))['text']
                self.assertIn('4150', result)
                server.command('quiet off')
                server.event('console_mode', lambda e: e['quiet'] is False)
                peer.send(attributes(5)); peer.receive(); server.event('attributes')
                server.command('quiet on')
                server.event('diagnostic', lambda e: e['text'].startswith('[CONSOLE]'))
                server.command('query BT000')
                delayed = peer.receive()[0]
                server.event('diagnostic', lambda e: '[QUERY #' in e['text'] and 'timeout' in e['text'])
                peer.send(answer(delayed))
                server.event('diagnostic', lambda e: e['text'].startswith('[LATE #'))
                server.command('task %d' % delayed['txnNo'])
                task = server.event('diagnostic', lambda e: e['text'].startswith('[TASK #'))['text']
                self.assertIn('timeout', task)
            finally: peer.close()

    def test_interactive_terminal_automatically_uses_quiet_mode(self):
        with Server(terminal=True) as server:
            ready = [e for e in server.messages if e.get('event') == 'ready'][0]
            self.assertTrue(ready['cli'])
            self.assertTrue(ready['quiet'])
            peer = Peer(server.port)
            try:
                fresh(peer)
                server.command('status BT000')
                row = server.event('diagnostic', lambda e: e['text'].startswith('[STATUS]'))['text']
                self.assertIn('BT000', row)
                self.assertIn('online_fresh', row)
                self.assertTrue(any(e.get('text') == '> status BT000' for e in server.messages))
            finally: peer.close()

    def test_success_task_lookup_and_duplicate(self):
        with Server() as server:
            peer = Peer(server.port)
            try:
                fresh(peer); pending, request = begin(server, peer)
                peer.send(answer(request))
                done = server.event('query_state', lambda e: e['txnNo'] == pending['txnNo'] and e['state'] == 'success')
                self.assertEqual(done['response']['attrList'][0]['protect'], 4150)
                peer.send(answer(request)); server.event('duplicate_response')
                server.command('task %d' % pending['txnNo'])
                self.assertEqual(server.event('task')['state'], 'success')
            finally: peer.close()

    def test_timeout_late_frames_cannot_reverse_terminal_state(self):
        with Server('--test-query-timeout-ms', '300') as server:
            peer = Peer(server.port)
            try:
                fresh(peer); pending, request = begin(server, peer)
                server.event('query_state', lambda e: e['state'] == 'timeout')
                peer.send(answer(request)); server.event('late_response')
                peer.send(answer(request)); server.event('late_response')
                server.command('task %d' % pending['txnNo'])
                self.assertEqual(server.event('task')['state'], 'timeout')
                server.command('stats')
                stats = server.event('worker_stats', lambda e: e['worker'] == pending['worker'])
                self.assertEqual((stats['late_frames'], stats['late_tasks']), (2, 1))
            finally: peer.close()

    def test_same_device_second_pending_is_busy(self):
        with Server() as server:
            peer = Peer(server.port)
            try:
                fresh(peer); pending, request = begin(server, peer)
                server.command('query BT000')
                self.assertEqual(server.event('query_rejected')['reason'], 'busy')
                peer.send(answer(request, attrs=False))
                done = server.event('query_state', lambda e: e['state'] == 'success')
                self.assertEqual(done['txnNo'], pending['txnNo'])
                self.assertNotIn('attrList', done['response'])
            finally: peer.close()

    def test_pending_disconnect_becomes_link_lost(self):
        with Server() as server:
            peer = Peer(server.port)
            try:
                fresh(peer); pending, request = begin(server, peer)
                peer.close()
                ended = server.event('query_state', lambda e: e['txnNo'] == pending['txnNo'] and e['state'] == 'link_lost')
                self.assertEqual(ended['reason'], 'peer_eof')
            finally: peer.close()

    def test_relogin_and_old_response_do_not_complete_new_session(self):
        with Server() as server:
            old = Peer(server.port); new = None
            try:
                fresh(old); pending, request = begin(server, old)
                new = Peer(server.port); fresh(new)
                ended = server.event('query_state', lambda e: e['txnNo'] == pending['txnNo'] and e['state'] == 'link_lost')
                self.assertEqual(ended['reason'], 'replaced')
                new.send(answer(request)); server.event('orphan_response')
                server.command('task %d' % pending['txnNo'])
                self.assertEqual(server.event('task')['state'], 'link_lost')
            finally:
                old.close()
                if new: new.close()

    def test_offline_and_unknown_device_are_explicit(self):
        with Server() as server:
            peer = Peer(server.port)
            try:
                fresh(peer); peer.close()
                server.event('device_state', lambda e: e['state'] == 'offline')
                server.command('query BT000')
                ended = server.event('query_state')
                self.assertEqual((ended['state'], ended['reason']), ('link_lost', 'offline'))
                server.command('query MISSING')
                self.assertEqual(server.event('query_rejected')['reason'], 'not_found')
            finally: peer.close()

    def test_invalid_response_does_not_complete_pending(self):
        with Server() as server:
            peer = Peer(server.port)
            try:
                fresh(peer); pending, request = begin(server, peer)
                invalid = answer(request); invalid['attrList'] = [1]
                peer.send(invalid); server.event('query_response_rejected')
                server.command('task %d' % pending['txnNo'])
                self.assertEqual(server.event('task')['state'], 'pending')
                peer.send(answer(request)); server.event('query_state', lambda e: e['state'] == 'success')
            finally: peer.close()

    def test_same_device_work_order_is_preserved(self):
        with Server('--test-slow-device', 'BT000', '--test-slow-ms', '60') as server:
            peer = Peer(server.port)
            try:
                fresh(peer)
                frames = []
                for i in range(3):
                    frame = attributes(10 + i); frame['attrList'][0]['value'] = 6000 + i
                    frames.append(frame)
                peer.socket.sendall(b''.join(wire(frame) for frame in frames))
                self.assertEqual([x['txnNo'] for x in peer.receive(3)], [10, 11, 12])
                self.assertEqual(server.stop(), 0)
                updates = [e for e in server.messages if e.get('event') == 'attributes'][-3:]
                self.assertEqual([e['voltage_v'] for e in updates], [60.0, 60.01, 60.02])
                self.assertEqual(len(set(e['worker'] for e in updates)), 1)
            finally: peer.close()

    def test_full_queue_closes_sender_without_blocking_other_shard(self):
        with Server('--test-queue-capacity', '1', '--test-slow-device', 'BT000', '--test-slow-ms', '200') as server:
            slow, fast = Peer(server.port), Peer(server.port)
            try:
                fresh(slow); fresh(fast, 'BT001')
                slow.socket.sendall(b''.join(wire(attributes(10 + i)) for i in range(10)))
                closed = server.event('closed', lambda e: e['reason'] == 'overloaded')
                self.assertEqual(closed['snapshot']['devId'], 'BT000')
                frame = attributes(); frame['devId'] = 'BT001'
                fast.send(frame); self.assertEqual(fast.receive()[0]['devId'], 'BT001')
                server.event('device_state', lambda e: e.get('devId') == 'BT000' and e['state'] == 'offline')
                server.command('stats'); stats = server.event('stats')
                self.assertEqual(stats['deferred_closes'], 0)
                self.assertTrue(all(row['peak'] <= 1 for row in stats['queues']))
            finally: slow.close(); fast.close()

    def test_stale_worker_send_result_cannot_reach_reused_socket(self):
        with Server('--test-slow-device', 'BT000', '--test-slow-ms', '200') as server:
            old, new = Peer(server.port), None
            try:
                fresh(old)
                old.send(attributes(99))
                server.event('test_delay_started', lambda e: e['txnNo'] == 99)
                old.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                old.close(); server.event('closed')
                new = Peer(server.port); new.send(login(100))
                self.assertEqual(new.receive()[0]['msgType'], 111)
                frame = attributes(101); frame['attrList'][0]['value'] = 6500
                new.send(frame); self.assertEqual(new.receive()[0]['txnNo'], 101)
                server.command('stats')
                self.assertGreaterEqual(server.event('stats')['stale_results'], 1)
            finally:
                old.close()
                if new: new.close()

    def test_queued_old_login_cannot_take_back_replaced_session(self):
        with Server('--test-slow-device', 'BT000', '--test-slow-ms', '200') as server:
            old, new = Peer(server.port), None
            try:
                fresh(old)
                old.send(attributes(99))
                server.event('test_delay_started', lambda e: e['txnNo'] == 99)
                new = Peer(server.port); new.send(login(100))
                server.event('login_queued')
                old.send(login(101))
                self.assertEqual(new.receive()[0]['msgType'], 111)
                update = attributes(102); update['attrList'][0]['value'] = 6500
                new.send(update); self.assertEqual(new.receive()[0]['txnNo'], 102)
                server.event('rejected', lambda e: e['reason'] == 'stale_login')
                self.assertEqual(server.stop(), 0)
                self.assertEqual(server.messages[-1]['replacements'], 1)
            finally:
                old.close()
                if new: new.close()

    def test_shutdown_drains_admitted_work_and_finishes_pending(self):
        with Server('--test-slow-device', 'BT000', '--test-slow-ms', '100') as server:
            peer = Peer(server.port)
            try:
                fresh(peer); pending, request = begin(server, peer)
                frames = []
                for i in range(3):
                    frame = attributes(20 + i); frame['attrList'][0]['value'] = 7000 + i; frames.append(frame)
                peer.socket.sendall(b''.join(wire(frame) for frame in frames))
                server.event('test_delay_started', lambda e: e['txnNo'] == 20)
                self.assertEqual(server.stop(), 0)
                updates = [e['voltage_v'] for e in server.messages if e.get('event') == 'attributes'][-3:]
                self.assertEqual(updates, [70.0, 70.01, 70.02])
                ended = [e for e in server.messages if e.get('event') == 'query_state' and e['txnNo'] == pending['txnNo']][-1]
                self.assertEqual((ended['state'], ended['reason']), ('link_lost', 'server_shutdown'))
            finally: peer.close()

    def test_half_close_keeps_admitted_ack_until_worker_finishes(self):
        with Server('--test-slow-device', 'BT000', '--test-slow-ms', '100') as server:
            peer = Peer(server.port)
            try:
                fresh(peer); peer.send(attributes(8)); peer.socket.shutdown(socket.SHUT_WR)
                self.assertEqual(peer.receive()[0]['txnNo'], 8)
                self.assertEqual(server.event('closed')['reason'], 'peer_eof')
            finally: peer.close()

    def test_cpp_persistent_simulator_answers_queries(self):
        with Server() as server:
            simulator = subprocess.Popen([SIM, '--port', str(server.port), '--demo', 'serve', '--devices', '1', '--duration-seconds', '2'],
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            try:
                server.event('attributes')
                server.command('query BT000')
                done = server.event('query_state', lambda e: e['state'] == 'success')
                self.assertEqual(done['response']['attrList'][0]['protect'], 4150)
                output, error = simulator.communicate(timeout=5)
                self.assertEqual(simulator.returncode, 0, error)
                self.assertIn('query_response', output)
            finally:
                if simulator.poll() is None: simulator.terminate(); simulator.communicate(timeout=3)

    def test_worker_isolation_comparison(self):
        measurements = []
        for worker_count in (1, 2):
            with Server('--workers', str(worker_count), '--test-slow-device', 'BT000', '--test-slow-ms', '300') as server:
                peers = [Peer(server.port) for _ in range(3)]
                try:
                    for i, peer in enumerate(peers): fresh(peer, 'BT%03d' % i)
                    peers[0].send(attributes(99))
                    marker = server.event('test_delay_started', lambda e: e['txnNo'] == 99)
                    timing = {}; failures = []
                    def measure(index):
                        try:
                            frame = attributes(100 + index); frame['devId'] = 'BT%03d' % index
                            started = time.monotonic(); peers[index].send(frame); peers[index].receive()
                            timing[index] = (time.monotonic() - started) * 1000
                        except Exception as error: failures.append(repr(error))
                    threads = [threading.Thread(target=measure, args=(i,)) for i in (1, 2)]
                    for thread in threads: thread.start()
                    peers[0].receive()
                    for thread in threads: thread.join(timeout=3)
                    self.assertFalse(failures)
                    self.assertEqual(set(timing), {1, 2})
                    measurements.append(dict(workers=worker_count, slow_worker=marker['worker'],
                                             BT001_ms=round(timing[1], 2), BT002_ms=round(timing[2], 2)))
                finally:
                    for peer in peers: peer.close()
        print('WORKER_COMPARISON ' + json.dumps(measurements, sort_keys=True))


if __name__ == '__main__':
    unittest.main(verbosity=2)
