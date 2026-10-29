#!/usr/bin/env python3
"""Run real Qt -> SSH -> HTTP -> Worker -> MySQL -> simulator integration."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time

root=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-build',required=True,type=Path)
    parser.add_argument('--sdk',default='E:/batt-v3-tools',type=Path)
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args(); args.output.mkdir(parents=True,exist_ok=False)
    hidden={'creationflags':subprocess.CREATE_NO_WINDOW} if os.name=='nt' else {}
    common=['ssh','-T','-p','2224','-i',str(root/'.local/v3-cloud-host/id_ed25519'),
            '-o','UserKnownHostsFile='+str(root/'.local/v3-cloud-host/known_hosts'),
            '-o','StrictHostKeyChecking=yes','-o','BatchMode=yes','-o','ExitOnForwardFailure=yes','book@127.0.0.1']
    command='cd /home/book/batt-access-v3 && BATT_MYSQL_TEST_CONFIG=/home/book/batt-access-v3/.local/mysql-test-admin.json .local/venv/bin/python scripts/qt-live-fixture.py'
    with (args.output/'fixture-errors.txt').open('w',encoding='utf-8') as errors:
        fixture=subprocess.Popen(common+[command],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errors,text=True,**hidden)
        tunnel=None
        try:
            ready=json.loads(fixture.stdout.readline())
            if not ready.get('ready'): raise RuntimeError('live fixture not ready')
            with socket.socket() as reserve:
                reserve.bind(('127.0.0.1',0)); port=reserve.getsockname()[1]
            forward=f'127.0.0.1:{port}:127.0.0.1:{ready["http_port"]}'
            # Options before destination; this process owns only its new forwarding port.
            tunnel=subprocess.Popen(common[:-1]+['-N','-L',forward,common[-1]],stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL,stderr=errors,**hidden)
            for attempt in range(100):
                try:
                    with socket.create_connection(('127.0.0.1',port),timeout=.2): break
                except OSError:
                    if tunnel.poll() is not None: raise RuntimeError('SSH tunnel exited')
                    time.sleep(.1)
            else: raise RuntimeError('SSH tunnel did not become ready')
            environment=os.environ.copy()
            environment['PATH']=str(args.sdk/'6.8.3/mingw_64/bin')+';'+str(args.sdk/'Tools/mingw1310_64/bin')+';'+environment['PATH']
            environment['QT_QPA_PLATFORM']='offscreen'; environment['BATT_QT_REAL_URL']=f'http://127.0.0.1:{port}'
            result=subprocess.run([str(args.qt_build/'ops_test.exe'),'realServiceThroughSsh','-o',str(args.output/'live.xml')+',xml'],
                                  cwd=args.qt_build,env=environment,capture_output=True,text=True,timeout=30,**hidden)
            (args.output/'process.txt').write_text(result.stdout+result.stderr,encoding='utf-8')
            if result.returncode: raise RuntimeError('Qt live integration failed; see XML')
            (args.output/'summary.json').write_text(json.dumps({'result':'QT_LIVE_PASS','chain':'Qt -> SSH -> HTTP -> Worker -> MySQL / C++ simulator',
                'qt_build':str(args.qt_build),'local_port':port,'remote_port':ready['http_port']},indent=2),encoding='utf-8')
            print('QT_LIVE_PASS')
        finally:
            if tunnel is not None:
                tunnel.terminate(); tunnel.wait(timeout=10)
            if fixture.poll() is None:
                try: fixture.communicate('\n',timeout=15)
                except subprocess.TimeoutExpired:
                    fixture.terminate(); fixture.wait(timeout=5)
            if fixture.returncode: raise RuntimeError('remote fixture cleanup failed')


if __name__=='__main__':
    main()
