#!/usr/bin/env python3
"""HTTP request-loss, deduplication and restart demo, using owned services/databases."""
import argparse
import http.client
import json
from pathlib import Path
import subprocess
import socket
import sys
import tempfile

root=Path(__file__).resolve().parent.parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir',default='build')
parser.add_argument('--output-dir',help='New evidence directory; must not exist')
args=parser.parse_args()
build=(root/args.build_dir).resolve()
sys.path.insert(0,str(root/'tests'))
sys.argv=[sys.argv[0],str(build/'batt-access'),str(build/'batt-sim')]
from http_test import HttpServer, request, post, sql
from network_test import Peer
from query_network_test import fresh, answer
from persistence_test import alarm

output=Path(args.output_dir).resolve() if args.output_dir else None
if output: output.mkdir(parents=True,exist_ok=False)


def check(value,message):
    if not value: raise RuntimeError(message)


def emit(step,**details):
    row=dict(step=step,**details)
    print(json.dumps(row,ensure_ascii=False),flush=True)
    if output:
        with (output/'steps.jsonl').open('a',encoding='utf-8') as stream:
            stream.write(json.dumps(row,ensure_ascii=False)+'\n')


def log(server,index):
    if output:
        (output/('service-%d.jsonl' % index)).write_text(
            ''.join(json.dumps(row,ensure_ascii=False)+'\n' for row in server.messages),encoding='utf-8')


with tempfile.TemporaryDirectory(prefix='batt-http-demo-') as folder:
    database=str(Path(folder)/'demo.db')
    server=HttpServer('--database-fixture',database,'--test-api-reply-delay-ms','400')
    peer=Peer(server.port)
    try:
        fresh(peer); peer.send(alarm('03')); peer.receive()
        status,snapshot,_=request(server,'GET','/v1/devices/BT000')
        check(status==200 and snapshot['state']=='online_fresh','Device state')
        emit('1/5 HTTP读取设备与告警',state=snapshot['state'],voltage_v=snapshot['voltage_v'],
             alarms=request(server,'GET','/v1/alarms?devId=BT000&limit=1')[1])
        connection=http.client.HTTPConnection('127.0.0.1',server.http_port,timeout=0.1)
        try:
            connection.request('POST','/v1/queries',json.dumps(dict(devId='BT000',clientRequestId='lost-response')),
                               {'Content-Type':'application/json'})
            frame=peer.receive()[0]
            try: connection.getresponse(); raise RuntimeError('Test response should be delayed')
            except socket.timeout: pass
        finally: connection.close()
        code,replayed,_=post(server,'lost-response')
        check(code==200 and replayed['txnNo']==frame['txnNo'] and replayed['replayed'],'Request retry duplicated task')
        emit('2/5 HTTP响应丢失，同号重试返回原任务',status=code,txnNo=frame['txnNo'],replayed=True)
        response=answer(frame); peer.send(response); server.event('query_state',lambda e:e['state']=='success')
        done=request(server,'GET','/v1/queries/%d' % frame['txnNo'])[1]
        check(done['response']==response,'Complete result lost')
        emit('3/5 原任务成功，重复请求不重发910',task=done)
        _,pending,_=post(server,'restart-pending'); peer.receive()
        server.process.kill(); check(server.process.wait(timeout=4)==-9,'Owned process kill')
    finally:
        peer.close(); server.stop(); log(server,1)
    with HttpServer('--database-fixture',database) as server:
        code,recovered,_=post(server,'restart-pending')
        check((code,recovered['txnNo'],recovered['state'],recovered['reason'])==
              (200,pending['txnNo'],'link_lost','server_restart'),'Restart replay')
        check(post(server,'lost-response')[1]['state']=='success','Completed state changed')
        check(request(server,'GET','/v1/devices/BT000')[1]['state']=='offline','Invented online device')
        emit('4/5 重启后仍按原请求号找回任务',task=recovered)
        code,conflict,_=post(server,'lost-response','BT001')
        check(code==409 and conflict['error']['code']=='request_id_conflict','Cross-device key conflict')
        check(sql(database,'SELECT COUNT(*) FROM query_task')==[(2,)],'Unexpected tasks')
        emit('5/5 跨设备请求号冲突被拒绝，记录数保持2',http_status=code,error=conflict['error'])
    log(server,2)
    if output:
        rows = {table: sql(database, 'SELECT * FROM ' + table) for table in ('device','alarm','query_task')}
        (output/'simulated-rows.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
emit('HTTP演示全部通过',result='PASS',evidence_dir=str(output) if output else '临时库已清理')
