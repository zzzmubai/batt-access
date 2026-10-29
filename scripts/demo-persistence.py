#!/usr/bin/env python3
"""Self-contained restart demonstration, with an isolated port and temporary DB."""
import argparse
import json
from pathlib import Path
import sys
import tempfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', default='build')
args = parser.parse_args()
build = root / args.build_dir
sys.path.insert(0, str(root / 'tests'))
sys.argv = [sys.argv[0], str(build / 'batt-access'), str(build / 'batt-sim')]
from network_test import Server, Peer
from query_network_test import fresh, begin, answer
from persistence_test import alarm


def emit(step, **values):
    print(json.dumps(dict(step=step, **values), ensure_ascii=False), flush=True)


with tempfile.TemporaryDirectory(prefix='batt-demo-') as folder:
    database = str(Path(folder) / 'demo.db')
    first = Server('--database-fixture', database)
    peer = Peer(first.port)
    try:
        fresh(peer); peer.send(alarm('03')); peer.receive()
        done, request = begin(first, peer); peer.send(answer(request))
        first.event('query_state', lambda e: e['state'] == 'success')
        pending, _ = begin(first, peer)
        emit('崩溃前：一个成功任务、一个pending、一条open告警', success_id=done['txnNo'], pending_id=pending['txnNo'])
        first.process.kill()
        assert first.process.wait(timeout=4) == -9
    finally:
        peer.close(); first.stop()
    with Server('--database-fixture', database) as second:
        second.command('status BT000'); status = second.event('status')
        second.command('task %d' % done['txnNo']); success = second.event('task')
        second.command('task %d' % pending['txnNo']); recovered = second.event('task')
        assert status['state'] == 'offline' and status['open_alarms'] == 1
        assert status['voltage_v'] is None and success['state'] == 'success'
        assert (recovered['state'], recovered['reason']) == ('link_lost', 'server_restart')
        emit('重启后：离线、告警保留、旧成功结果保留、pending结束', state=status['state'], open_alarms=status['open_alarms'], recovered_task=recovered)
        peer = Peer(second.port)
        try:
            fresh(peer); peer.send(alarm('03')); peer.receive()
            again, request = begin(second, peer); peer.send(answer(request))
            second.event('query_state', lambda e: e['state'] == 'success')
            second.command('status BT000'); status = second.event('status')
            assert again['txnNo'] > pending['txnNo'] and status['alarm_history'] == 1
            emit('重新登录上报：新编号继续增长，重复集合没有新增告警', new_id=again['txnNo'], state=status['state'], alarm_history=status['alarm_history'])
        finally:
            peer.close()
emit('全部断言通过；只终止演示脚本自己启动的服务，临时库已清理')
