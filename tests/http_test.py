#!/usr/bin/env python3
"""Independent HTTP/socket/SQL clients. No application business implementation is imported."""
import concurrent.futures
import http.client
import json
import os
import select
import socket
import pymysql
from mysql_probe import fixture, sql as mysql_sql
import subprocess
import tempfile
import threading
import time
import unittest
from network_test import ACCESS, Server, Peer, login, attributes
from query_network_test import fresh, answer
from persistence_test import alarm


class HttpServer(Server):
    def __init__(self, *args):
        super().__init__('--http-port', '0', *args)
        self.http_port = next(e['http_port'] for e in self.messages if e.get('event') == 'ready')


def request(server, method, path, body=None, timeout=4, media='application/json'):
    connection = http.client.HTTPConnection('127.0.0.1', server.http_port, timeout=timeout)
    try:
        headers = {'Content-Type': media} if body is not None else {}
        payload = json.dumps(body).encode('utf-8') if isinstance(body, dict) else body
        connection.request(method, path, payload, headers)
        response = connection.getresponse()
        data = response.read()
        return response.status, json.loads(data.decode('utf-8')), dict(response.getheaders())
    finally: connection.close()


def post(server, key, dev='BT000', **kwargs):
    return request(server, 'POST', '/v1/queries', dict(devId=dev, clientRequestId=key), **kwargs)


def sql(path, query, params=()):
    return mysql_sql(path, query, params)


class HttpTests(unittest.TestCase):
    def server(self, *args):
        server = HttpServer(*args); self.addCleanup(server.stop); return server

    def peer(self, server, dev='BT000'):
        peer = Peer(server.port); self.addCleanup(peer.close); fresh(peer, dev); return peer

    def test_disabled_by_default_and_fixed_loopback(self):
        with Server() as server:
            ready = next(e for e in server.messages if e.get('event') == 'ready')
            self.assertIsNone(ready['http_port'])
        server = self.server()
        self.assertGreater(server.http_port, 0)
        sockets = subprocess.check_output(['ss', '-ltn'], universal_newlines=True)
        matching = [line for line in sockets.splitlines() if ':%d ' % server.http_port in line]
        self.assertTrue(matching); self.assertTrue(all('127.0.0.1:' in line for line in matching))

    def test_occupied_http_port_fails_startup_without_ready(self):
        server=self.server()
        with tempfile.TemporaryDirectory(prefix='batt-http-bind-') as folder:
            result=subprocess.run([ACCESS,'--port','0','--http-port',str(server.http_port),'--db-config',fixture(os.path.join(folder,'bind')).config],
                stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,universal_newlines=True,timeout=5)
        self.assertEqual(result.returncode,1); self.assertIn('HTTP bind failed',result.stdout)
        self.assertNotIn('"event":"ready"',result.stdout)

    def test_state_and_cli_share_snapshot(self):
        server = self.server(); self.peer(server)
        code, row, _ = request(server, 'GET', '/v1/devices/BT000')
        self.assertEqual((code, row['state'], row['voltage_v']), (200, 'online_fresh', 52.8))
        server.command('status BT000'); cli = server.event('status')
        self.assertEqual(row['session_token'], cli['session_token'])
        self.assertEqual(request(server, 'GET', '/v1/devices/UNKNOWN')[0], 404)
        self.assertEqual(request(server, 'GET', '/v1/queries/999999')[0], 404)

    def test_post_commit_location_and_complete_result(self):
        server = self.server(); peer = self.peer(server)
        code, row, headers = post(server, 'create')
        self.assertEqual((code, row['state'], row['replayed']), (201, 'pending', False))
        self.assertEqual(headers['Location'], '/v1/queries/%d' % row['txnNo'])
        stored = sql(server.db_path, 'SELECT status,client_request_id FROM query_task')
        self.assertEqual(stored, [('pending', 'create')])
        frame = peer.receive()[0]; self.assertEqual(frame['txnNo'], row['txnNo'])
        response = answer(frame); response['extra'] = {'text': '原样保存', 'none': None}
        peer.send(response); server.event('query_state', lambda e: e['state'] == 'success')
        actual = request(server, 'GET', headers['Location'])[1]
        self.assertEqual(actual['response'], response)
        server.command('task %d' % row['txnNo']); self.assertEqual(server.event('task')['response'], response)

    def test_replay_before_busy_and_after_terminal_does_not_resend(self):
        server = self.server(); peer = self.peer(server)
        _, original, _ = post(server, 'once'); frame = peer.receive()[0]
        code, replay, _ = post(server, 'once')
        self.assertEqual((code, replay['txnNo'], replay['replayed']), (200, original['txnNo'], True))
        self.assertEqual(post(server, 'different')[0], 409)
        peer.send(answer(frame)); server.event('query_state', lambda e: e['state'] == 'success')
        self.assertEqual(post(server, 'once')[1]['state'], 'success')
        peer.socket.settimeout(0.15)
        with self.assertRaises(socket.timeout): peer.socket.recv(4096)
        self.assertEqual(sql(server.db_path, 'SELECT COUNT(*) FROM query_task'), [(1,)])

    def test_simultaneous_same_key_only_one_task(self):
        server = self.server(); peer = self.peer(server)
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            responses = list(pool.map(lambda _: post(server, 'parallel'), range(6)))
        self.assertEqual(sorted(code for code, _, _ in responses), [200]*5+[201])
        self.assertEqual(len({row['txnNo'] for _, row, _ in responses}), 1)
        self.assertEqual(sql(server.db_path, 'SELECT COUNT(*) FROM query_task'), [(1,)])
        frame = peer.receive()[0]; peer.send(answer(frame))

    def test_cross_shard_same_key_conflict_is_not_storage_failure(self):
        server = self.server(); self.peer(server); self.peer(server, 'BT001')
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(post, server, 'global', dev) for dev in ('BT000', 'BT001')]
            responses = [future.result() for future in futures]
        self.assertEqual(sorted(code for code, _, _ in responses), [201, 409])
        conflict = next(row for code, row, _ in responses if code == 409)
        self.assertEqual(conflict['error']['code'], 'request_id_conflict')
        self.assertIsNone(server.process.poll())
        self.assertEqual(sql(server.db_path, 'SELECT COUNT(*) FROM query_task'), [(1,)])

    def test_offline_is_durable_terminal_unknown_is_not_persisted(self):
        server = self.server(); peer = self.peer(server); peer.close()
        server.event('device_state', lambda e: e['state'] == 'offline')
        code, row, _ = post(server, 'offline')
        self.assertEqual((code, row['state'], row['reason']), (201, 'link_lost', 'offline'))
        self.assertEqual(post(server, 'unknown', 'UNKNOWN')[0], 404)
        self.assertEqual(sql(server.db_path, 'SELECT COUNT(*) FROM query_task'), [(1,)])

    def test_busy_rejection_does_not_reserve_request_key(self):
        server = self.server(); peer = self.peer(server)
        post(server, 'one'); first = peer.receive()[0]
        self.assertEqual(post(server, 'retry')[0], 409)
        peer.send(answer(first)); server.event('query_state', lambda e: e['state'] == 'success')
        self.assertEqual(post(server, 'retry')[0], 201)

    def test_cli_and_http_have_one_pending_guard(self):
        server = self.server(); peer = self.peer(server)
        server.command('query BT000'); original = peer.receive()[0]
        self.assertEqual(post(server, 'http-busy')[0], 409)
        peer.send(answer(original)); server.event('query_state', lambda e: e['state'] == 'success')
        post(server, 'http-busy'); peer.receive()
        server.command('query BT000'); self.assertEqual(server.event('query_rejected')['reason'], 'busy')

    def test_validation_method_and_media_errors_are_nonfatal(self):
        server = self.server(); self.peer(server)
        cases = [({}, 400), ({'devId':'BT000'},400), ({'devId':'BT000','clientRequestId':7},400),
                 ({'devId':'BT000','clientRequestId':''},400),
                 ({'devId':'BT000','clientRequestId':'bad key'},400),
                 ({'devId':'BT000','clientRequestId':'k','extra':1},400),
                 ('{"devId":"BT000","clientRequestId":"a","clientRequestId":"b"}',400),
                 ('{"devId":',400)]
        for body, expected in cases:
            with self.subTest(body=body): self.assertEqual(request(server,'POST','/v1/queries',body)[0],expected)
        self.assertEqual(post(server,'media',media='text/plain')[0],415)
        code,row,_=request(server,'POST','/v1/queries',{'devId':42,'clientRequestId':'echo-me'})
        self.assertEqual((code,row['clientRequestId']),(400,'echo-me'))
        for path in ('/v1/devices/BT000?unused=1','/v1/queries/0','/v1/queries/-1',
                     '/v1/queries/9223372036854775808','/v1/alarms?devId=BT000&limit=0',
                     '/v1/alarms?devId=BT000&limit=51','/v1/alarms?devId=BT000&status=bad',
                     '/v1/alarms?devId=BT000&limit=1&limit=2','/v1/alarms?devId=BT000&beforeId=0'):
            with self.subTest(path=path): self.assertEqual(request(server,'GET',path)[0],400)
        self.assertEqual(request(server,'GET','/missing')[0],404)
        code, _, headers = request(server,'PUT','/v1/devices/BT000')
        self.assertEqual((code,headers['Allow']),(405,'GET'))
        self.assertIsNone(server.process.poll())
        self.assertEqual(post(server,'valid',media='Application/JSON ; charset=utf-8')[0],201)

    def test_body_header_bounds_and_device_io_progress(self):
        server = self.server(); peer = self.peer(server)
        self.assertEqual(request(server,'POST','/v1/queries','x'*4097)[0],413)
        client = socket.create_connection(('127.0.0.1',server.http_port),timeout=3)
        try:
            client.sendall(b'GET /v1/devices/BT000 HTTP/1.1\r\nHost: localhost\r\nX-Large: '+b'x'*1100+b'\r\n\r\n')
            data=client.recv(4096); self.assertNotIn(b'200 OK',data)
        finally: client.close()
        peer.send(attributes(9)); self.assertEqual(peer.receive()[0]['txnNo'],9)

    def test_concurrent_results_have_unique_request_ids(self):
        server = self.server(); self.peer(server); self.peer(server,'BT001')
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            jobs=[pool.submit(request,server,'GET','/v1/devices/BT%03d' % (i%2)) for i in range(24)]
            responses=[future.result() for future in jobs]
        self.assertTrue(all(code==200 for code,_,_ in responses))
        self.assertEqual([row['devId'] for _,row,_ in responses],['BT%03d' % (i%2) for i in range(24)])
        self.assertEqual(len({row['requestId'] for _,row,_ in responses}),24)

    def test_alarm_cursor_filters_and_new_insert_no_duplicate_ids(self):
        server=self.server(); peer=self.peer(server)
        for codes in (('03',),(),('03',),(),('06',)):
            peer.send(alarm(*codes)); peer.receive()
        code,page,_=request(server,'GET','/v1/alarms?devId=BT000&limit=1')
        self.assertEqual(code,200); seen=[page['items'][0]['id']]
        peer.send(alarm('01')); peer.receive()
        cursor=page['nextBeforeId']
        while cursor is not None:
            _,page,_=request(server,'GET','/v1/alarms?devId=BT000&limit=1&beforeId=%d' % cursor)
            seen.extend(row['id'] for row in page['items']); cursor=page['nextBeforeId']
        self.assertEqual(seen,sorted(set(seen),reverse=True)); self.assertEqual(len(seen),3)
        _,page,_=request(server,'GET','/v1/alarms?devId=BT000&status=open')
        self.assertEqual([row['desc'] for row in page['items']],['01'])
        self.assertIsNone(page['nextBeforeId'])
        self.assertEqual(request(server,'GET','/v1/alarms?devId=UNKNOWN')[0],404)

    def test_timeout_late_and_polling_preserve_terminal_state(self):
        server=self.server('--test-query-timeout-ms','300'); peer=self.peer(server)
        _,row,_=post(server,'timeout'); frame=peer.receive()[0]
        server.event('query_state',lambda e:e['state']=='timeout')
        peer.send(answer(frame)); server.event('late_response')
        for _ in range(3): self.assertEqual(request(server,'GET','/v1/queries/%d' % row['txnNo'])[1]['state'],'timeout')
        self.assertEqual(post(server,'timeout')[1]['state'],'timeout')

    def test_application_timeout_before_execution_skips_task_and_late_reply(self):
        server=self.server('--test-api-timeout-ms','100','--test-slow-device','BT000','--test-slow-ms','400')
        peer=self.peer(server); peer.send(attributes(99)); server.event('test_delay_started',lambda e:e['txnNo']==99)
        code,row,_=post(server,'queued')
        self.assertEqual((code,row['outcome']),(503,'unknown'))
        peer.receive()
        self.assertEqual(request(server,'GET','/v1/devices/BT000')[0],200)  # FIFO barrier after expired work.
        self.assertEqual(sql(server.db_path,'SELECT COUNT(*) FROM query_task'),[(0,)])
        self.assertEqual(post(server,'queued')[0],201)

    def test_full_worker_queue_rejects_post_without_reserving_key(self):
        server=self.server('--test-queue-capacity','1','--test-slow-device','BT000','--test-slow-ms','1000')
        peer=self.peer(server); peer.send(attributes(99))
        server.event('test_delay_started',lambda e:e['txnNo']==99)
        # stats itself queues one command behind the slow BT000 work. Observe
        # queue occupancy through I/O stats rather than guessing with a sleep.
        for _ in range(3):
            server.command('stats'); counters=server.event('stats')
            if counters['queues'][1]['queued']==1: break
        self.assertEqual(counters['queues'][1]['queued'],1)
        code,row,_=post(server,'full-worker')
        self.assertEqual((code,row['error']['code'],row['outcome']),(503,'worker_busy','not_created'))
        self.assertEqual(sql(server.db_path,'SELECT COUNT(*) FROM query_task'),[(0,)])
        peer.receive(); server.event('worker_stats',lambda e:e['worker']==1)
        self.assertEqual(post(server,'full-worker')[0],201)

    def test_timeout_during_sql_commit_can_be_recovered_by_same_key(self):
        server=self.server('--test-api-timeout-ms','100'); self.peer(server)
        writer=fixture(server.db_path).connect(); writer.begin()
        with writer.cursor() as lock:
            lock.execute("SELECT dev_id FROM device WHERE dev_id='BT000' FOR UPDATE")
        try:
            code,row,_=post(server,'commit-wait'); self.assertEqual((code,row['outcome']),(503,'unknown'))
        finally: writer.rollback(); writer.close()
        code,row,_=post(server,'commit-wait')
        self.assertEqual((code,row['replayed']),(200,True))
        self.assertEqual(sql(server.db_path,'SELECT COUNT(*) FROM query_task'),[(1,)])

    def test_client_response_lost_keeps_task_and_deduplicates(self):
        server=self.server('--test-api-reply-delay-ms','400'); peer=self.peer(server)
        connection=http.client.HTTPConnection('127.0.0.1',server.http_port,timeout=0.1)
        try:
            connection.request('POST','/v1/queries',json.dumps(dict(devId='BT000',clientRequestId='lost')),{'Content-Type':'application/json'})
            frame=peer.receive()[0]
            with self.assertRaises(socket.timeout): connection.getresponse()
        finally: connection.close()
        code,row,_=post(server,'lost')
        self.assertEqual((code,row['txnNo']),(200,frame['txnNo']))
        peer.send(answer(frame)); server.event('query_state',lambda e:e['state']=='success')
        self.assertEqual(sql(server.db_path,'SELECT COUNT(*) FROM query_task'),[(1,)])

    def test_stopping_with_slow_drip_body_releases_http_threads(self):
        server=self.server(); peer=self.peer(server)
        sockets=[socket.create_connection(('127.0.0.1',server.http_port),timeout=2) for _ in range(4)]
        stop=threading.Event()
        for client in sockets:
            client.sendall(b'POST /v1/queries HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 4000\r\n\r\n')
        def drip():
            while not stop.wait(0.03):
                for client in sockets:
                    try: client.sendall(b' ')
                    except OSError: pass
        thread=threading.Thread(target=drip); thread.start()
        try:
            peer.send(attributes(20)); peer.receive()
            started=time.monotonic(); self.assertEqual(server.stop(),0); self.assertLess(time.monotonic()-started,4)
        finally:
            stop.set(); thread.join(timeout=2)
            for client in sockets: client.close()

    def test_connection_queue_is_bounded_without_blocking_devices(self):
        server=self.server(); peer=self.peer(server)
        clients=[]
        try:
            for _ in range(20):
                client=socket.create_connection(('127.0.0.1',server.http_port),timeout=1); clients.append(client)
                try: client.sendall(b'GET /v1/devices/BT000 HTTP/1.1\r\nHost: ')
                except OSError: pass
            ready,_,_=select.select(clients,[],[],0.5)
            rejected=False
            for client in ready:
                try: rejected=rejected or client.recv(1024)==b''
                except ConnectionResetError: rejected=True
            self.assertTrue(rejected)
            peer.send(attributes(44)); self.assertEqual(peer.receive()[0]['txnNo'],44)
        finally:
            for client in clients: client.close()


class HttpRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='batt-http-recovery-'); self.addCleanup(self.temp.cleanup)
        self.path=os.path.join(self.temp.name,'test.db')

    def server(self,*args):
        server=HttpServer('--database-fixture',self.path,*args); self.addCleanup(server.stop); return server

    def peer(self,server):
        peer=Peer(server.port); self.addCleanup(peer.close); fresh(peer); return peer

    def test_restart_returns_same_request_and_preserves_completed_task(self):
        server=self.server(); peer=self.peer(server)
        _,done,_=post(server,'done'); frame=peer.receive()[0]; peer.send(answer(frame)); server.event('query_state',lambda e:e['state']=='success')
        _,pending,_=post(server,'pending'); peer.receive()
        server.process.kill(); self.assertEqual(server.process.wait(timeout=4),-9); server.stop()
        server=self.server()
        self.assertEqual(post(server,'done')[1]['state'],'success')
        code,row,_=post(server,'pending')
        self.assertEqual((code,row['txnNo'],row['state'],row['reason']),(200,pending['txnNo'],'link_lost','server_restart'))
        self.assertEqual(request(server,'GET','/v1/devices/BT000')[1]['state'],'offline')
        self.assertEqual(sql(self.path,'SELECT COUNT(*) FROM query_task'),[(2,)])

    def test_before_and_after_commit_crash_never_split_key_and_task(self):
        for phase in ('before_commit','after_commit'):
            with self.subTest(phase=phase):
                self.path=os.path.join(self.temp.name,phase+'.db')
                server=self.server('--test-storage-crash','query_begin','--test-storage-crash-phase',phase); self.peer(server)
                try: post(server,'crash')
                except (OSError,http.client.HTTPException): pass
                self.assertEqual(server.process.wait(timeout=4),86); server.stop()
                rows=sql(self.path,'SELECT client_request_id,status FROM query_task')
                self.assertEqual(rows,[('crash','pending')] if phase=='after_commit' else [])
                server=self.server(); code,row,_=post(server,'crash')
                self.assertEqual(code,200 if phase=='after_commit' else 201)
                if phase=='after_commit': self.assertEqual(row['reason'],'server_restart')
                self.assertEqual(server.stop(),0)

    def test_sql_failure_rolls_back_task_key_and_exits_nonzero(self):
        server=self.server('--test-storage-fail','query_begin'); self.peer(server)
        try: post(server,'rollback')
        except (OSError,http.client.HTTPException): pass
        self.assertEqual(server.process.wait(timeout=4),1); server.stop()
        self.assertEqual(sql(self.path,'SELECT COUNT(*) FROM query_task'),[(0,)])
        server=self.server(); self.assertEqual(post(server,'rollback')[0],201)

    def test_uninitialized_and_wrong_schema_never_listen(self):
        from mysql_probe import TestDatabase, schema
        for kind in ('empty', 'interrupted', 'wrong_version', 'missing_index'):
            with self.subTest(kind=kind), TestDatabase(initialize=False) as database:
                with database.connect() as current:
                    if kind == 'interrupted':
                        with self.assertRaises(RuntimeError): schema.initialize(current, stop_after=2)
                    elif kind != 'empty':
                        schema.initialize(current)
                        with current.cursor() as cursor:
                            cursor.execute("UPDATE schema_meta SET schema_version=2" if kind == 'wrong_version'
                                else "ALTER TABLE query_task DROP INDEX unique_client_request")
                result=subprocess.run([ACCESS,'--port','0','--http-port','0','--db-config',database.config],
                    stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=8)
                self.assertEqual(result.returncode,1)
                self.assertNotIn('"event":"ready"',result.stdout)

    def test_int64_task_and_first_page_cursor_are_exact(self):
        server=self.server(); self.peer(server)
        large=9223372036854775807
        sql(self.path,"INSERT INTO query_task(txn_no,dev_id,status,created_ms,finished_ms) VALUES(?,'BT000','success',1,2)",(large,))
        sql(self.path,"INSERT INTO alarm(id,dev_id,event_no,alarm_id,alarm_desc,status,first_ms,last_ms) VALUES(?,'BT000',1,'01003001','03','open',1,1)",(large,))
        code,row,_=request(server,'GET','/v1/queries/%d' % large)
        self.assertEqual((code,row['txnNo']),(200,large))
        code,page,_=request(server,'GET','/v1/alarms?devId=BT000')
        self.assertEqual((code,page['items'][0]['id']),(200,large))
        _,page,_=request(server,'GET','/v1/alarms?devId=BT000&beforeId=%d' % large)
        self.assertEqual(page['items'],[])


if __name__=='__main__': unittest.main(verbosity=2)
