#!/usr/bin/env python3
"""Independent MySQL/socket/crash tests; only process-owned schemas are cleaned."""
import json
import os
import pymysql
from mysql_probe import fixture, sql as mysql_sql
import subprocess
import tempfile
import time
import unittest
from network_test import ACCESS, Server, Peer, login, attributes, wire
from query_network_test import fresh, begin, answer


def alarm(*codes):
    values = [dict(id='01003001', alarmDesc=code, alarmFlag=1) for code in codes]
    return dict(msgType=410, devId='BT000', txnNo=3,
                alarmList=values or [dict(id='01003001', alarmDesc='000', alarmFlag=0)])


class PersistenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='batt-persistence-')
        self.addCleanup(self.temp.cleanup)
        self.db = os.path.join(self.temp.name, 'state.db')

    def server(self, *args, **kwargs):
        server = Server('--database-fixture', self.db, *args, **kwargs)
        self.addCleanup(server.stop)
        return server

    def peer(self, server):
        peer = Peer(server.port)
        self.addCleanup(peer.close)
        return peer

    def sql(self, statement, params=()):
        return mysql_sql(self.db, statement, params)

    def seed(self, codes=('03', '06')):
        server = self.server()
        peer = self.peer(server)
        fresh(peer)
        peer.send(alarm(*codes)); self.assertEqual(peer.receive()[0]['result'], 1)
        peer.close()
        self.assertEqual(server.stop(), 0)

    def task(self, server, task_id):
        server.command('task %d' % task_id)
        return server.event('task', lambda e: e['txnNo'] == task_id)

    def finished(self, server, state='success'):
        return server.event('query_state', lambda e: e['state'] == state)

    def crash(self, server, code=86):
        self.assertEqual(server.process.wait(timeout=8), code)
        server.stop()

    def test_alarm_ack_means_committed_and_restart_keeps_open_only(self):
        server = self.server(); peer = self.peer(server); fresh(peer)
        peer.send(alarm('03', '06')); peer.receive()
        first = self.sql('SELECT event_no,first_ms FROM alarm ORDER BY event_no')
        self.assertEqual(len(first), 2)  # Separate reader observes committed data before ACK.
        peer.send(alarm('06', '01')); peer.receive()
        self.assertEqual(self.sql("SELECT alarm_desc FROM alarm WHERE status='open' ORDER BY alarm_desc"), [('01',), ('06',)])
        peer.close(); self.assertEqual(server.stop(), 0)
        server = self.server('--workers', '1')  # Recovery also supports changing shard count.
        server.command('status BT000'); status = server.event('status')
        self.assertEqual((status['state'], status['open_alarms'], status['alarm_history']), ('offline', 2, 3))
        self.assertIsNone(status['voltage_v']); self.assertEqual(status['session_token'], 0)
        server.command('alarms BT000')
        rows = [server.event('alarm') for _ in range(3)]
        self.assertEqual(sum(r['state'] == 'open' for r in rows), 2)
        self.assertEqual(self.sql('SELECT event_no,first_ms FROM alarm WHERE event_no<=2 ORDER BY event_no'), first)
        peer = self.peer(server); peer.send(login()); peer.receive()
        server.command('status BT000'); self.assertEqual(server.event('status')['state'], 'connected_no_data')
        peer.send(alarm('06', '01')); peer.receive()
        self.assertEqual(self.sql('SELECT COUNT(*) FROM alarm'), [(3,)])
        peer.send(alarm()); peer.receive()
        self.assertEqual(self.sql("SELECT COUNT(*) FROM alarm WHERE status='open'"), [(0,)])

    def test_complete_query_json_survives_restart_missing_and_empty_are_distinct(self):
        server = self.server(); peer = self.peer(server); fresh(peer)
        expected = []
        for mode in ('full', 'missing', 'empty'):
            pending, request = begin(server, peer)
            frame = answer(request, attrs=mode != 'missing')
            if mode == 'empty': frame['attrList'] = []
            if mode == 'full': frame['extra'] = dict(notes='测试', enabled=True, nullable=None)
            peer.send(frame); self.finished(server)
            expected.append((pending['txnNo'], frame, pending['connection_id']))
            stored = self.sql('SELECT status,response_json FROM query_task WHERE txn_no=?', (pending['txnNo'],))[0]
            self.assertEqual((stored[0], json.loads(stored[1])), ('success', frame))
        peer.close(); self.assertEqual(server.stop(), 0)
        server = self.server()
        for task_id, frame, identity in expected:
            row = self.task(server, task_id)
            self.assertEqual((row['state'], row['response'], row['connection_id']), ('success', frame, identity))

    def test_autoincrement_not_reused_after_deleting_highest_history(self):
        self.seed(); server = self.server()
        server.command('query BT000'); first = self.finished(server, 'link_lost')['txnNo']
        self.assertEqual(server.stop(), 0)
        self.sql('DELETE FROM query_task WHERE txn_no=?', (first,))
        server = self.server(); server.command('query BT000')
        second = self.finished(server, 'link_lost')['txnNo']
        self.assertGreater(second, first)
        self.assertIsNone(self.task(server, second)['connection_id'])
        server.command('query UNKNOWN'); self.assertEqual(server.event('query_rejected')['reason'], 'not_found')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM query_task'), [(1,)])

    def test_sigkill_pending_recovers_once_and_old_answer_is_orphan(self):
        server = self.server(); peer = self.peer(server); fresh(peer)
        pending, request = begin(server, peer)
        self.assertEqual(self.sql('SELECT status FROM query_task'), [('pending',)])
        server.process.kill(); self.crash(server, -9); peer.close()
        server = self.server(); row = self.task(server, pending['txnNo'])
        self.assertEqual((row['state'], row['reason']), ('link_lost', 'server_restart'))
        self.assertEqual(row['connection_id'], pending['connection_id'])
        peer = self.peer(server); fresh(peer); peer.send(answer(request)); server.event('orphan_response')
        self.assertEqual(self.task(server, pending['txnNo'])['state'], 'link_lost')
        self.assertEqual(server.stop(), 0)
        server = self.server(); self.assertEqual(self.task(server, pending['txnNo'])['finished_ms'], row['finished_ms'])

    def test_normal_stop_commits_pending_terminal_state(self):
        server = self.server(); peer = self.peer(server); fresh(peer); pending, _ = begin(server, peer)
        self.assertEqual(server.stop(), 0)
        self.assertEqual(self.sql('SELECT status,reason FROM query_task'), [('link_lost', 'server_shutdown')])
        server = self.server(); self.assertEqual(self.task(server, pending['txnNo'])['reason'], 'server_shutdown')

    def test_failed_login_does_not_ack_or_replace_committed_identity(self):
        self.seed(); before = self.sql('SELECT * FROM device')
        server = self.server('--test-storage-fail', 'login'); peer = self.peer(server)
        frame = login(); frame['imsi'] = 'SHOULD_ROLL_BACK'; peer.send(frame)
        self.crash(server, 1)
        self.assertEqual(peer.socket.recv(4096), b'')
        self.assertEqual(self.sql('SELECT * FROM device'), before)

    def test_failed_alarm_rolls_back_every_row_and_no_411_success(self):
        self.seed(); before = self.sql('SELECT * FROM alarm ORDER BY id')
        server = self.server('--test-storage-fail', 'alarm'); peer = self.peer(server); fresh(peer)
        peer.send(alarm('01')); self.crash(server, 1)
        self.assertEqual(peer.socket.recv(4096), b'')
        self.assertEqual(self.sql('SELECT * FROM alarm ORDER BY id'), before)
        self.assertTrue(any(e.get('event') == 'storage_error' for e in server.messages))

    def test_failed_query_begin_does_not_send_910_or_publish_pending(self):
        server = self.server('--test-storage-fail', 'query_begin'); peer = self.peer(server); fresh(peer)
        server.command('query BT000'); self.crash(server, 1)
        self.assertEqual(peer.socket.recv(4096), b'')
        self.assertEqual(self.sql('SELECT COUNT(*) FROM query_task'), [(0,)])
        self.assertFalse(any(e.get('event') == 'query_state' for e in server.messages))

    def test_failed_query_success_leaves_pending_for_recovery(self):
        server = self.server('--test-storage-fail', 'query_finish'); peer = self.peer(server); fresh(peer)
        pending, request = begin(server, peer); peer.send(answer(request)); self.crash(server, 1)
        self.assertEqual(self.sql('SELECT status,response_json FROM query_task'), [('pending', None)])
        self.assertFalse(any(e.get('state') == 'success' for e in server.messages))
        server = self.server(); self.assertEqual(self.task(server, pending['txnNo'])['reason'], 'server_restart')

    def test_alarm_crash_before_during_and_after_commit_is_atomic(self):
        for phase in ('before_commit', 'mid_alarm', 'after_commit'):
            with self.subTest(phase=phase):
                self.db = os.path.join(self.temp.name, phase + '.db'); self.seed()
                before = self.sql('SELECT * FROM alarm ORDER BY id')
                server = self.server('--test-storage-crash', 'alarm', '--test-storage-crash-phase', phase)
                peer = self.peer(server); fresh(peer); peer.send(alarm('01')); self.crash(server)
                self.assertEqual(peer.socket.recv(4096), b'')
                if phase == 'after_commit':
                    self.assertEqual(self.sql("SELECT alarm_desc FROM alarm WHERE status='open'"), [('01',)])
                    self.assertEqual(self.sql('SELECT COUNT(*) FROM alarm'), [(3,)])
                else:
                    self.assertEqual(self.sql('SELECT * FROM alarm ORDER BY id'), before)
                server = self.server(); peer = self.peer(server); fresh(peer); peer.send(alarm('01')); peer.receive()
                self.assertEqual(self.sql('SELECT COUNT(*) FROM alarm'), [(3,)])
                self.assertEqual(self.sql("SELECT alarm_desc FROM alarm WHERE status='open'"), [('01',)])
                with fixture(self.db).connect() as check_connection:
                    from mysql_probe import schema
                    schema.validate(check_connection)
                self.assertEqual(server.stop(), 0)

    def test_query_allocation_crash_committed_pending_is_recovered(self):
        for phase in ('before_commit', 'after_commit'):
            with self.subTest(phase=phase):
                self.db = os.path.join(self.temp.name, phase + '.db')
                server = self.server('--test-storage-crash', 'query_begin', '--test-storage-crash-phase', phase)
                peer = self.peer(server); fresh(peer); server.command('query BT000'); self.crash(server)
                self.assertEqual(peer.socket.recv(4096), b'')
                self.assertEqual(self.sql('SELECT status FROM query_task'), [('pending',)] if phase == 'after_commit' else [])
                server = self.server()
                self.assertEqual(self.sql('SELECT status,reason FROM query_task'), [('link_lost', 'server_restart')] if phase == 'after_commit' else [])
                self.assertEqual(server.stop(), 0)

    def test_query_success_crash_does_not_publish_uncommitted_result(self):
        for phase in ('before_commit', 'after_commit'):
            with self.subTest(phase=phase):
                self.db = os.path.join(self.temp.name, phase + '.db')
                server = self.server('--test-storage-crash', 'query_finish', '--test-storage-crash-phase', phase)
                peer = self.peer(server); fresh(peer); pending, request = begin(server, peer)
                response = answer(request); peer.send(response); self.crash(server)
                server = self.server(); row = self.task(server, pending['txnNo'])
                self.assertEqual(row['state'], 'success' if phase == 'after_commit' else 'link_lost')
                self.assertEqual(row['response'], response if phase == 'after_commit' else None)
                self.assertEqual(server.stop(), 0)

    def test_replacement_login_and_old_pending_are_one_transaction(self):
        for phase in ('before_commit', 'after_commit', 'failure'):
            with self.subTest(phase=phase):
                self.db = os.path.join(self.temp.name, phase + '.db')
                args = ('--test-storage-fail', 'login_replace') if phase == 'failure' else (
                    '--test-storage-crash', 'login_replace', '--test-storage-crash-phase', phase)
                server = self.server(*args); old = self.peer(server); fresh(old); pending, _ = begin(server, old)
                new = self.peer(server); frame = login(); frame['imsi'] = 'NEW_SIM'; new.send(frame)
                self.crash(server, 1 if phase == 'failure' else 86)
                committed = phase == 'after_commit'
                self.assertEqual(self.sql('SELECT imsi FROM device'), [('NEW_SIM' if committed else 'SIM000',)])
                self.assertEqual(self.sql('SELECT status,reason FROM query_task'),
                                 [('link_lost', 'replaced')] if committed else [('pending', '')])
                server = self.server(); row = self.task(server, pending['txnNo'])
                self.assertEqual(row['reason'], 'replaced' if committed else 'server_restart')
                self.assertEqual(server.stop(), 0)

    def test_external_writer_lock_has_bounded_failure(self):
        self.seed(); before = self.sql('SELECT * FROM alarm ORDER BY id')
        server = self.server(); peer = self.peer(server); fresh(peer)
        writer = fixture(self.db).connect()
        try:
            writer.begin()
            with writer.cursor() as lock:
                lock.execute("SELECT id FROM alarm WHERE dev_id='BT000' FOR UPDATE")
            started = time.monotonic(); peer.send(alarm('01')); self.crash(server, 1)
            self.assertLess(time.monotonic() - started, 2.5)
            self.assertEqual(peer.socket.recv(4096), b'')
        finally:
            writer.rollback(); writer.close()
        self.assertEqual(self.sql('SELECT * FROM alarm ORDER BY id'), before)

    def test_recovery_failure_never_listens_or_partially_changes_pending(self):
        server = self.server(); peer = self.peer(server); fresh(peer); begin(server, peer)
        server.process.kill(); self.crash(server, -9)
        output = subprocess.run([ACCESS, '--port', '0', '--db-config', fixture(self.db).config, '--test-storage-fail', 'recovery'],
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                universal_newlines=True, timeout=4)
        self.assertEqual(output.returncode, 1); self.assertNotIn('"event":"ready"', output.stdout)
        self.assertEqual(self.sql('SELECT status FROM query_task'), [('pending',)])
        server = self.server(); self.assertEqual(self.sql('SELECT status,reason FROM query_task'), [('link_lost', 'server_restart')])

    def test_timeout_and_disconnect_are_persisted_and_late_cannot_change_them(self):
        server = self.server('--test-query-timeout-ms', '300'); peer = self.peer(server); fresh(peer)
        pending, request = begin(server, peer); self.finished(server, 'timeout')
        peer.send(answer(request)); server.event('late_response')
        self.assertEqual(self.sql('SELECT status,response_json FROM query_task'), [('timeout', None)])
        pending2, _ = begin(server, peer); peer.close(); self.finished(server, 'link_lost')
        self.assertEqual(server.stop(), 0)
        server = self.server()
        self.assertEqual(self.task(server, pending['txnNo'])['state'], 'timeout')
        self.assertEqual(self.task(server, pending2['txnNo'])['reason'], 'peer_eof')

    def test_shutdown_deadline_exits_nonzero_with_unfinished_statistics(self):
        server = self.server('--test-slow-device', 'BT000', '--test-slow-ms', '1000')
        peer = self.peer(server); peer.send(login()); peer.receive()
        peer.socket.sendall(b''.join(wire(attributes(n + 10)) for n in range(16)))
        server.event('test_delay_started')
        server.process.terminate(); self.crash(server, 1)
        row = [e for e in server.messages if e.get('event') == 'unfinished'][-1]
        self.assertEqual(row['reason'], 'shutdown_timeout')
        self.assertGreater(sum(q['queued'] for q in row['queues']), 0)
        self.assertIn('unsent_bytes', row); self.assertIn('pending', row['queues'][0])

    def test_schema_constraints_and_invalid_database_startup(self):
        self.seed()
        db = fixture(self.db).connect()
        try:
            with db.cursor() as cursor:
                statements = [
                    "INSERT INTO device VALUES('BT000','x',1,1)",
                    "INSERT INTO alarm(dev_id,event_no,alarm_id,alarm_desc,status,first_ms,last_ms) VALUES('BT000',99,'01003001','03','open',1,1)",
                    "INSERT INTO alarm(dev_id,event_no,alarm_id,alarm_desc,status,first_ms,last_ms) VALUES('UNKNOWN',99,'01003001','01','open',1,1)",
                    "INSERT INTO query_task(dev_id,status,created_ms) VALUES('BT000','success',1)"]
                for statement, expected in zip(statements, (1062,1062,1452,3819)):
                    with self.assertRaises(pymysql.MySQLError) as caught: cursor.execute(statement)
                    self.assertEqual(caught.exception.args[0], expected)
                cursor.execute("INSERT INTO query_task(dev_id,status,created_ms) VALUES('BT000','pending',1)")
                with self.assertRaises(pymysql.IntegrityError):
                    cursor.execute("INSERT INTO query_task(dev_id,status,created_ms) VALUES('BT000','pending',1)")
        finally: db.close()
        for path in (':memory:', os.path.join(self.temp.name, 'missing', 'test.db'), self.temp.name):
            output = subprocess.run([ACCESS, '--port', '0', '--db-config', path], stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, timeout=5)
            self.assertNotEqual(output.returncode, 0); self.assertNotIn('"event":"ready"', output.stdout)

    def test_same_database_cannot_be_owned_by_two_servers(self):
        server = self.server(); peer = self.peer(server); fresh(peer); begin(server, peer)
        output = subprocess.run([ACCESS, '--port', '0', '--db-config', fixture(self.db).config], stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, timeout=4)
        self.assertEqual(output.returncode, 1); self.assertIn('database already in use', output.stdout)
        self.assertEqual(self.sql('SELECT status FROM query_task'), [('pending',)])

    def test_same_database_different_cwd_rejected_before_recovery(self):
        server = self.server(); peer = self.peer(server); fresh(peer); begin(server, peer)
        output = subprocess.run([os.path.abspath(ACCESS), '--port', '0', '--db-config', fixture(self.db).config],
            cwd=self.temp.name, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=5)
        self.assertEqual(output.returncode, 1)
        self.assertIn('database already in use', output.stdout)
        self.assertEqual(self.sql('SELECT status FROM query_task'), [('pending',)])

    def test_runtime_connection_kill_is_fatal_without_login_success(self):
        server = self.server(); peer = self.peer(server); fresh(peer)
        database = fixture(self.db)
        with database.connect(runtime=True) as killer, killer.cursor() as cursor:
            cursor.execute('SELECT ID FROM information_schema.PROCESSLIST WHERE DB=%s AND ID<>CONNECTION_ID()', (database.name,))
            victims = [row[0] for row in cursor.fetchall()]
            self.assertGreaterEqual(len(victims), 2)
            for identifier in victims: cursor.execute('KILL CONNECTION %s', (identifier,))
        peer.send(login(30))
        self.crash(server, 1)
        self.assertEqual(peer.socket.recv(4096), b'')
        self.assertTrue(any(row.get('event') == 'storage_error' for row in server.messages))

    def test_old_db_option_is_explicitly_rejected(self):
        output = subprocess.run([ACCESS, '--db', '/tmp/unused-v2.db'], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=5)
        self.assertEqual(output.returncode, 1)
        self.assertIn('replace --db with --db-config', output.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
