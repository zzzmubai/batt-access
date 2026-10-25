#!/usr/bin/env python3
"""Repeatable acceptance demo using owned processes, ports and databases only."""
import argparse
import copy
from contextlib import contextmanager
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', default='build')
parser.add_argument('--output-dir', help='New directory for evidence; must not exist')
args = parser.parse_args()
BUILD = (ROOT / args.build_dir).resolve()
sys.path.insert(0, str(ROOT / 'tests'))
sys.argv = [sys.argv[0], str(BUILD / 'batt-access'), str(BUILD / 'batt-sim')]
from network_test import Server, Peer

SAMPLES = json.loads((ROOT / 'docs/fixtures/protocol-samples.json').read_text(encoding='utf-8'))


def check(condition, description):
    if not condition:
        raise RuntimeError('验收失败：' + description)


class Demo:
    def __init__(self, output):
        self.output = Path(output).resolve() if output else None
        if self.output:
            self.output.mkdir(parents=True, exist_ok=False)
        self.events = []
        self.service_count = 0

    def emit(self, step, **details):
        row = dict(step=step, **details)
        self.events.append(row)
        print(json.dumps(row, ensure_ascii=False, sort_keys=True), flush=True)
        if self.output:
            with (self.output / 'steps.jsonl').open('a', encoding='utf-8') as stream:
                stream.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + '\n')

    @contextmanager
    def service(self, database, *options):
        self.service_count += 1
        index = self.service_count
        server = Server('--db', str(database), *options)
        server.expected_exit = 0
        try:
            yield server
        finally:
            code = server.stop()
            if self.output:
                with (self.output / ('service-%d.jsonl' % index)).open('w', encoding='utf-8') as stream:
                    for row in server.messages:
                        stream.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + '\n')
            check(code == server.expected_exit, '服务退出码 %s，预期 %s' % (code, server.expected_exit))

    def rows(self, database, sql):
        connection = sqlite3.connect(Path(database).resolve().as_uri() + '?mode=ro', uri=True)
        try:
            return connection.execute(sql).fetchall()
        finally:
            connection.close()

    def snapshot(self, server, dev):
        server.command('status ' + dev)
        return server.event('status', lambda e: e['devId'] == dev)

    def devices(self, server):
        server.command('devices')
        return [server.event('device') for _ in range(32)]

    def task(self, server, task_id):
        server.command('task %d' % task_id)
        return server.event('task', lambda e: e['txnNo'] == task_id)

    def query(self, server, terminal):
        terminal.attributes()
        server.command('query ' + terminal.dev)
        pending = server.event('query_state', lambda e: e['devId'] == terminal.dev and e['state'] == 'pending')
        request = terminal.peer.receive()[0]
        check(request == dict(msgType=910, devId=terminal.dev, txnNo=pending['txnNo']), '910关联')
        return pending, request

    def answer(self, terminal, request):
        response = copy.deepcopy(SAMPLES['query']['response'])
        response.update(devId=terminal.dev, txnNo=request['txnNo'])
        terminal.peer.send(response)
        return response

    def cpp_demo(self, database):
        with self.service(database) as server:
            simulator = subprocess.Popen([str(BUILD / 'batt-sim'), '--port', str(server.port),
                                          '--devices', '32', '--hold-seconds', '3'],
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            try:
                server.event('alarms', lambda e: e['devId'] == 'BT031' and e['history'] == 3, timeout=12)
                rows = self.devices(server)
                check(len({r['devId'] for r in rows}) == 32, 'C++模拟器设备数')
                check(all(r['state'] == 'online_fresh' and r['open_alarms'] == 1 and r['alarm_history'] == 3 for r in rows), 'C++模拟器32设备状态')
                output, error = simulator.communicate(timeout=7)
                check(simulator.returncode == 0, 'C++模拟器退出：' + error)
                markers = [json.loads(line) for line in output.splitlines()]
                check(any(e.get('event') == 'demo_complete' and e.get('acks') == 256 for e in markers), '256次关联ACK')
                check(self.rows(database, 'SELECT COUNT(*) FROM alarm') == [(96,)], 'C++模拟器96条告警历史')
                if self.output:
                    (self.output / 'cpp-simulator.jsonl').write_text(output, encoding='utf-8')
                self.emit('1/7 C++模拟器32终端闭环通过', devices=32, acknowledgements=256, alarm_history=96)
            finally:
                if simulator.poll() is None:
                    simulator.terminate()
                    simulator.communicate(timeout=4)

    def full_demo(self, database):
        terminals = []
        with self.service(database, '--test-freshness-ms', '2000') as server:
            try:
                for index in range(32):
                    terminal = Terminal(server.port, 'BT%03d' % index)
                    terminals.append(terminal)
                    terminal.login(); terminal.attributes()
                rows = self.devices(server)
                check(len({r['devId'] for r in rows}) == 32, '固定样例的32个设备')
                for row in rows:
                    for key, expected in SAMPLES['attributes']['expected_snapshot'].items():
                        check(row[key] == expected, '独立样例字段 ' + key)
                zero = terminals[0]
                zero.send_case('invalid_soc', expect_ack=False)
                server.event('rejected', lambda e: e['reason'] == SAMPLES['invalid_soc']['expected_reason'])
                status = self.snapshot(server, zero.dev)
                for key, expected in SAMPLES['invalid_soc']['expected_unchanged'].items():
                    check(status[key] == expected, '非法属性帧应整帧拒绝')
                zero.attributes()  # Also verifies no ACK from the rejected frame is queued.
                self.emit('2/7 独立固定样例与32条连接通过', devices=32, voltage_v=52.8, current_a=-1.25, invalid_frame='整帧拒绝')

                zero.alarm(True); zero.alarm(True)
                check(self.rows(database, "SELECT COUNT(*) FROM alarm WHERE dev_id='BT000'") == [(1,)], '持续告警重复建事件')
                old_id = self.rows(database, "SELECT id FROM alarm WHERE dev_id='BT000'")[0][0]
                zero.alarm(False)
                check(self.rows(database, "SELECT status FROM alarm WHERE id=%d" % old_id) == [('cleared',)], '000未恢复告警')
                zero.alarm(True)
                alarm_rows = self.rows(database, "SELECT id,status FROM alarm WHERE dev_id='BT000' ORDER BY id")
                check(len(alarm_rows) == 2 and alarm_rows[-1][0] > old_id and alarm_rows[-1][1] == 'open', '恢复后再触发未生成新事件')
                self.emit('3/7 告警持续、恢复、再次触发通过', first_id=old_id, new_id=alarm_rows[-1][0], open_alarms=1)

                until = time.monotonic() + 7
                stale = None
                while time.monotonic() < until:
                    terminals[1].alarm(True)  # Keep 410 flowing while withholding 310.
                    for terminal in terminals:
                        if terminal.dev != 'BT001': terminal.attributes()
                    stale = self.snapshot(server, 'BT001')
                    if stale['state'] == 'connected_stale': break
                    time.sleep(0.1)  # Reporting cadence; state observation is the barrier.
                check(stale['state'] == 'connected_stale' and stale['session_token'] != 0, '停止310应过期，410不刷新属性')
                check(self.snapshot(server, 'BT000')['state'] == 'online_fresh', '其他设备仍可采集')

                old = terminals[2]
                replaced, old_request = self.query(server, old)
                new = Terminal(server.port, old.dev)
                terminals[2] = new
                try:
                    new.login(); new.attributes(voltage=6500)
                    ended = server.event('query_state', lambda e: e['txnNo'] == replaced['txnNo'] and e['state'] == 'link_lost')
                    check(ended['reason'] == 'replaced', '替换应结束旧pending')
                    self.answer(new, old_request)
                    server.event('orphan_response', lambda e: e['txnNo'] == replaced['txnNo'])
                    try:
                        old.attributes(voltage=9999)
                        raise RuntimeError('旧连接仍收到成功ACK')
                    except (OSError, AssertionError):
                        pass
                    check(self.snapshot(server, new.dev)['voltage_v'] == 65.0, '旧连接污染新会话')
                    check(self.task(server, replaced['txnNo'])['state'] == 'link_lost', '旧应答改变终态')
                finally:
                    old.close()
                self.emit('4/7 数据过期与会话替换通过', stale_device='BT001', freshness_test_ms=2000, replaced_device='BT002', old_task_state='link_lost')

                success, request = self.query(server, zero)
                response = self.answer(zero, request)
                server.event('query_state', lambda e: e['txnNo'] == success['txnNo'] and e['state'] == 'success')
                check(self.task(server, success['txnNo'])['response'] == response, '成功结果完整保存')
                timeout, request = self.query(server, zero)
                began = time.monotonic()
                server.event('query_state', lambda e: e['txnNo'] == timeout['txnNo'] and e['state'] == 'timeout', timeout=6)
                while time.monotonic() - began < 5:
                    for terminal in terminals: terminal.attributes()
                    time.sleep(0.05)
                self.answer(zero, request)
                server.event('late_response', lambda e: e['txnNo'] == timeout['txnNo'])
                check(self.task(server, timeout['txnNo'])['state'] == 'timeout', '迟到不能翻案')
                self.emit('5/7 查询成功、3秒超时、5秒迟到通过', success_id=success['txnNo'], timeout_id=timeout['txnNo'], query_timeout_ms=3000, scheduled_response_ms=5000)

                pending, _ = self.query(server, terminals[3])
                alarm_before = self.rows(database, 'SELECT * FROM alarm ORDER BY id')
                check(self.rows(database, "SELECT COUNT(*) FROM alarm WHERE status='open'")[0][0] > 0, '崩溃前必须有open告警')
                server.expected_exit = -9
                server.process.kill()
                check(server.process.wait(timeout=4) == -9, 'SIGKILL未结束所创建的服务')
            finally:
                for terminal in terminals: terminal.close()

        with self.service(database) as server:
            rows = self.devices(server)
            check(all(r['state'] == 'offline' and r['voltage_v'] is None and r['session_token'] == 0 for r in rows), '历史记录不可恢复在线或新鲜')
            check(self.rows(database, 'SELECT * FROM alarm ORDER BY id') == alarm_before, '重启丢失告警或时间')
            recovered = self.task(server, pending['txnNo'])
            check((recovered['state'], recovered['reason']) == ('link_lost', 'server_restart'), 'pending恢复')
            check(self.task(server, success['txnNo'])['response'] == response, '重启丢失成功结果')
            check(self.task(server, timeout['txnNo'])['state'] == 'timeout', '重启翻案超时')
            check(self.task(server, replaced['txnNo'])['state'] == 'link_lost', '重启翻案旧会话')
            self.emit('6/7 崩溃后32设备离线，告警及终态保留', devices=32, pending_id=pending['txnNo'], recovered_state=recovered['state'], reason=recovered['reason'])
            terminals = []
            try:
                for index in range(32):
                    terminal = Terminal(server.port, 'BT%03d' % index)
                    terminals.append(terminal)
                    terminal.login(); terminal.attributes(); terminal.alarm(index in (0, 1))
                check(self.rows(database, 'SELECT COUNT(*) FROM alarm') == [(len(alarm_before),)], '重发集合重复建告警')
                check(all(r['state'] == 'online_fresh' for r in self.devices(server)), '重连后32设备采集')
                again, request = self.query(server, terminals[3]); self.answer(terminals[3], request)
                server.event('query_state', lambda e: e['txnNo'] == again['txnNo'] and e['state'] == 'success')
                check(again['txnNo'] > pending['txnNo'], '重启后编号回退')
                check(self.rows(database, 'PRAGMA integrity_check') == [('ok',)], '数据库完整性')
                self.emit('7/7 32设备重新采集，编号继续增长', previous_id=pending['txnNo'], new_id=again['txnNo'], devices=32, database_integrity='ok')
            finally:
                for terminal in terminals: terminal.close()


class Terminal:
    """Only builds fixed frames and checks ACKs; no server business state machine."""
    def __init__(self, port, dev):
        self.peer = Peer(port)
        self.dev = dev
        self.sequence = 0

    def send_case(self, name, expect_ack=True, voltage=None):
        case = SAMPLES[name]
        request = copy.deepcopy(case['request'])
        self.sequence += 1
        request.update(devId=self.dev, txnNo=self.sequence)
        if voltage is not None: request['attrList'][0]['value'] = voltage
        self.peer.send(request)
        if expect_ack:
            expected = copy.deepcopy(case['expected_ack'])
            expected.update(devId=self.dev, txnNo=self.sequence)
            check(self.peer.receive()[0] == expected, 'ACK类型/设备/编号/结果关联')

    def login(self): self.send_case('login')
    def attributes(self, voltage=None): self.send_case('attributes', voltage=voltage)
    def alarm(self, active): self.send_case('alarm_open' if active else 'alarm_clear')
    def close(self): self.peer.close()


def main():
    demo = Demo(args.output_dir)
    with tempfile.TemporaryDirectory(prefix='batt-final-') as folder:
        folder = Path(folder)
        cpp_database = folder / 'cpp.db'
        database = folder / 'final.db'
        demo.cpp_demo(cpp_database)
        demo.full_demo(database)
        if demo.output:
            # SQLite Backup API includes committed WAL pages; do not copy a lone db file.
            for path in (database, cpp_database):
                target = demo.output / path.name
                command = '.backup ' + json.dumps(str(target), ensure_ascii=False)
                result = subprocess.run([str(BUILD / 'batt-db'), '-readonly', str(path), command],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, timeout=10)
                check(result.returncode == 0, 'SQLite备份失败：' + result.stdout)
                check(demo.rows(target, 'PRAGMA integrity_check') == [('ok',)], '备份完整性')
                check(demo.rows(target, 'SELECT COUNT(*) FROM device') == [(32,)], '备份设备数')
            (demo.output / 'summary.json').write_text(json.dumps(dict(result='PASS', steps=demo.events,
                evidence='模拟数据；过期使用2000ms测试钩子，查询为默认3000ms；不是吞吐压测或现场验证'),
                ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    demo.emit('最终验收完成', result='PASS', evidence_dir=str(demo.output) if demo.output else '临时库已清理')


if __name__ == '__main__':
    main()
