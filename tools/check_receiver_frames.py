# SPDX-License-Identifier: GPL-2.0-or-later
"""Check both frame owners with authored MMIO/time; no camera firmware required.

This is a device contract check. It does not qualify camera capture, physical
timing, pixel processing or pre-release files.
"""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

B = 0x15080000
PERIOD = (1200*502*10**9+72_000_000-1)//72_000_000


class Guest:
    def __init__(self, binary, folder, model, split, enabled):
        self.folder=Path(folder);self.process=None;self.streams=[];self.sockets=[]
        self.log=(self.folder/'stderr').open('wb')
        stub=self.folder/'authored-zero.bin';stub.write_bytes(bytes(4))
        raw=self.folder/'synthetic-black.raw'
        raw.write_bytes(struct.pack('<8s6I',b'NZ9RAW1\0',64,48,12,0,0,4095)+bytes(64*48*2))
        props=[p for p in json.loads((Path(__file__).parent/'model-properties.json').read_text())['properties']
            if not p.startswith(('service-core-event-model=','event1508-timing-period-ns='))]
        props+=['statistics-raw='+str(raw),'event1508-timing-period-ns=33333333','event1508-source-counter-hz=72000000',
            'event1508-global0-split-boundary-model=on','event1508-global1-paired-boundary=on',
            'event1508-global1-receiver-clock-model=on']
        if enabled:props+=['event1508-global1-frame-owner-model=on']
        if split:props+=['event1508-global1-split-boundary-model=on']
        self.process=subprocess.Popen([str(binary),'-M',model+','+','.join(props),
            '-accel','qtest','-kernel',str(stub),'-smp','4','-m','1024',
            '-display','none','-serial','none','-monitor','none',
            '-qmp','unix:'+str(self.folder/'qmp')+',server=on,wait=off',
            '-qtest','unix:'+str(self.folder/'qtest')+',server=on,wait=off'],
            stdout=subprocess.DEVNULL,stderr=self.log)
        try:
            self.qmp=self.connect(self.folder/'qmp');self.qt=self.connect(self.folder/'qtest')
            greeting=json.loads(self.qmp.readline())
            if 'QMP' not in greeting:raise RuntimeError('Missing QMP greeting')
            self.command('qmp_capabilities');self.now=0;self.qtest('clock_step 1')
        except Exception as error:
            self.close()
            detail=(self.folder/'stderr').read_text(errors='replace')[-4000:]
            raise RuntimeError('Device test startup failed: '+detail) from error

    def connect(self, path):
        deadline=time.monotonic()+15
        while True:
            s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);s.settimeout(15)
            try:s.connect(str(path));break
            except (FileNotFoundError,ConnectionRefusedError):
                s.close()
                if self.process.poll() is not None or time.monotonic()>=deadline:
                    raise RuntimeError('QEMU did not open its local test socket')
                time.sleep(.02)
        self.sockets.append(s);f=s.makefile('rwb',buffering=0);self.streams.append(f);return f

    def command(self, name, **kwargs):
        self.qmp.write((json.dumps(dict(execute=name,arguments=kwargs))+'\n').encode())
        while True:
            line=self.qmp.readline()
            if not line:raise RuntimeError('QEMU closed QMP')
            response=json.loads(line)
            if 'error' in response:raise RuntimeError(response['error'])
            if 'return' in response:return response['return']

    def qtest(self, line):
        self.qt.write((line+'\n').encode());answer=self.qt.readline().decode().strip()
        if not answer.startswith('OK'):raise RuntimeError(answer)
        return answer

    def write(self, offset, value):self.qtest(f'writel {B+offset:#x} {value:#x}')
    def get(self, name):return json.loads(self.command('qom-get',path='/machine',property=name))
    def owner(self):return self.get('event1508-global1-frame-owner-state')
    def pending(self):return self.get('event1508-global1-state')['pending']&0x90
    def advance(self, target):
        if target<self.now:raise ValueError('Time reversal')
        self.qtest('clock_step '+str(target-self.now));self.now=target

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait()
        for f in self.streams:f.close()
        for s in self.sockets:s.close()
        self.log.close()


def check_case(binary, model, split, enabled):
    checks=[]
    def require(label, condition):
        if not condition:raise AssertionError(f'{model}/{split}/{enabled}: {label}')
        checks.append(label)
    with tempfile.TemporaryDirectory(prefix='wl-frame-') as folder:
        g=Guest(binary,folder,model,split,enabled)
        try:
            require('new model remains default-off',g.owner()['enabled']==enabled)
            for i,v in enumerate((0x04af04af,501,0,0x04af0000,0,1,0,0)):
                g.write(0x3000+4*i,v)
            for reg,val in ((0x3020,1),(0x68,0x90),(0x50,0x90),(0x44,1),(0x28,1),(0x10,1)):
                g.write(reg,val)
            g.advance(PERIOD)
            require('stopped cold receiver cannot own BEGIN',not g.owner()['inflight'])
            require('input0 remains independent',g.get('event1508-global0-state')['pending']&0x90==0x90)
            require('cold END follows selected legacy/owner behavior',g.pending()==(0 if enabled else 0x80))
            g.write(0x6c,0x90);g.write(0x18,1)
            if split:
                g.advance(2*PERIOD-PERIOD//2)
                require('enabled BEGIN arrives',g.pending()==0x10)
                if enabled:require('accepted BEGIN owns a frame',g.owner()['inflight'])
                g.write(0x68,0)
                require('mask cannot consume BEGIN',g.pending()==0x10)
                g.write(0x1c,1)
                require('STOP keeps pending BEGIN',g.pending()==0x10)
                g.write(0x68,0x90);g.advance(2*PERIOD)
                require('one started frame drains after STOP',g.pending()==0x90)
            else:
                g.advance(2*PERIOD)
                require('paired source frame completes',g.pending()==0x90)
                g.write(0x1c,1)
            g.write(0x6c,0x90)
            require('W1C retires status',g.pending()==0)
            g.advance(4*PERIOD)
            require('stopped END follows selected legacy/owner behavior',g.pending()==(0 if enabled else 0x80))
            if enabled:
                require('one END per accepted BEGIN',g.owner()['begins']==g.owner()['ends']==1)
                require('unowned END is counted',g.owner()['unowned_ends_suppressed']>=2)
            require('clock status never substitutes for pixels',g.get('primary-d980-state')['frames']==0)
            g.write(0x6c,0x90);g.write(0x1c,1);g.advance(5*PERIOD)
            require('repeated STOP grants no BEGIN',g.pending()&0x10==0)
            g.write(0x6c,0x90)
            g.command('qom-set',path='/machine',property='event1508-global1-input',value='128')
            require('explicit END remains independent',g.pending()==0x80)
            g.write(0x6c,0x80);g.write(0x18,1)
            require('resume cannot replay old phases',g.pending()==0)
            g.advance(5*PERIOD+(PERIOD-PERIOD//2 if split else PERIOD))
            require('resume starts a fresh frame',bool(g.pending()&0x10))
            if split:g.advance(6*PERIOD)
            if enabled:require('resumed frame retires exactly once',g.owner()['begins']==g.owner()['ends']==2)
            require('no fake RAW DMA after resume',g.get('primary-d980-state')['frames']==0)
            final=g.owner()
        finally:g.close()
        require('unknown access guard stays clear',b'Z9_UNKNOWN' not in (Path(folder)/'stderr').read_bytes())
    return dict(model=model,split=split,enabled=enabled,checks=checks,final=final,passed=True)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--qemu',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();binary=a.qemu.resolve()
    if a.output.exists():p.error('Use a new output path')
    cases=[check_case(binary,m,s,e) for m in ('nikon-z9-lab','nikon-zr-lab')
        for s in (False,True) for e in (False,True)]
    report=dict(passed=True,binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
        cases=cases,checks=sum(len(c['checks']) for c in cases),
        proprietary_firmware=False,native_camera_qualified=False,physical_protocol_verified=False)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    with a.output.open('x',encoding='utf-8') as out:json.dump(report,out,indent=2);out.write('\n')
    print(json.dumps({k:v for k,v in report.items() if k!='cases'}))


if __name__=='__main__':main()
