#!/usr/bin/env python3
"""Small reproducible VM workload baseline, not a throughput/capacity claim."""
import argparse
import collections
import concurrent.futures
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

root=Path(__file__).resolve().parent.parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir',default='build')
args=parser.parse_args()
build=(root/args.build_dir).resolve()
sys.path.insert(0,str(root/'tests'))
sys.argv=[sys.argv[0],str(build/'batt-access'),str(build/'batt-sim')]
from http_test import HttpServer, request, post


def usage(pid):
    fields=Path('/proc/%d/stat' % pid).read_text().split()
    seconds=(int(fields[13])+int(fields[14]))/os.sysconf('SC_CLK_TCK')
    peak=next(int(line.split()[1]) for line in Path('/proc/%d/status' % pid).read_text().splitlines() if line.startswith('VmHWM:'))
    return seconds,peak


def distribution(values):
    values=sorted(values)
    if not values: return None
    return {name:round(values[max(0,math.ceil(len(values)*fraction)-1)],3)
            for name,fraction in [('p50',.5),('p95',.95),('p99',.99)]}


results=[]
for label,devices,clients,iterations,pause in [('light',1,1,30,.1),('reporting',32,4,30,.1),('burst',32,16,8,0)]:
    with HttpServer() as server:
        simulator=subprocess.Popen([str(build/'batt-sim'),'--port',str(server.port),'--demo','serve','--devices',str(devices)],
                                   stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,universal_newlines=True)
        try:
            server.event('alarms',lambda e:e['devId']=='BT%03d' % (devices-1))
            cpu_before,_=usage(server.process.pid); began=time.monotonic()
            def traffic(client):
                samples=[]; previous=None
                for index in range(iterations):
                    started=time.monotonic(); outcome='transport_error'
                    try:
                        dev='BT%03d' % (client%devices)
                        if index%10==0:
                            previous='%s-%d-%d' % (label,client,index)
                            code,row,_=post(server,previous,dev)
                            outcome='created' if code==201 else ('busy' if code==409 else 'http_%d' % code)
                        elif index%10==1:
                            code,row,_=post(server,previous,dev)
                            outcome='replayed' if code==200 else ('created' if code==201 else 'http_%d' % code)
                        else:
                            code,row,_=request(server,'GET','/v1/devices/'+dev)
                            outcome='state_ok' if code==200 and row.get('state')=='online_fresh' else 'http_%d' % code
                    except Exception as error:
                        outcome='transport_'+type(error).__name__
                    samples.append((outcome,(time.monotonic()-started)*1000))
                    if pause: time.sleep(pause)
                return samples
            with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:
                samples=[sample for group in pool.map(traffic,range(clients)) for sample in group]
            elapsed=time.monotonic()-began; cpu_after,peak=usage(server.process.pid)
            if server.process.poll() is not None: raise RuntimeError('Service exited during workload')
            server.command('stats'); stats=server.event('stats')
            http_stats=server.event('http_stats')
            row=dict(profile=label,devices=devices,http_clients=clients,attempts=len(samples),seconds=round(elapsed,3),
                     report_interval_seconds=1,cpu_percent_one_core=round((cpu_after-cpu_before)/elapsed*100,2),peak_rss_kib=peak,
                     outcomes=dict(collections.Counter(outcome for outcome,_ in samples)),queues=stats['queues'],http_stats=http_stats)
            row['latency_ms_by_outcome']={outcome:distribution([ms for kind,ms in samples if kind==outcome]) for outcome in row['outcomes']}
            results.append(row)
        finally:
            if simulator.poll() is None: simulator.terminate()
            _,error=simulator.communicate(timeout=5)
            if simulator.returncode!=0: raise RuntimeError('Simulator failed: '+error)
print(json.dumps(dict(environment=dict(kernel=platform.release(),cpus=os.cpu_count(),build_dir=str(build)),
                      note='Debug虚拟机短时基线；混合读状态/创建/重放；按结果类别分别统计时延，不以错误吞吐作为收益',profiles=results),
                 ensure_ascii=False,indent=2))
