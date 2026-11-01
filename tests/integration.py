#!/usr/bin/env python3
"""Exercise the actual daemon, POSIX transport, framing, scheduler and IPC via PTY."""
import os,pty,socket,subprocess,sys,threading,time,struct,json,select,concurrent.futures

def crc(data):
    c=65535
    for b in data:
        c^=b
        for _ in range(8): c=(c>>1)^0xa001 if c&1 else c>>1
    return c

def frame(data): return data+struct.pack('<H',crc(data))

class Device:
    def __init__(self):
        self.master,self.slave=pty.openpty();self.path=os.ttyname(self.slave)
        self.reg=[0]*121;self.reg[0]=60067;self.reg[3]=115;self.reg[8:14]=[420,6100,421,700,0,295];self.reg[18]=1
        self.requests=[];self.bad=False;self.exception=False;self.running=True
        self.thread=threading.Thread(target=self.loop,daemon=True);self.thread.start()
    def loop(self):
        buf=b''
        while self.running:
            if not select.select([self.master],[],[],.05)[0]:continue
            try:buf+=os.read(self.master,4096)
            except OSError:continue
            while len(buf)>=8:
                fn=buf[1];n=9+buf[6] if fn==16 and len(buf)>=7 else 8
                if len(buf)<n:break
                req,buf=buf[:n],buf[n:]
                assert crc(req[:-2])==int.from_bytes(req[-2:],'little'),req.hex()
                addr,fn,start,value=struct.unpack('>BBHH',req[:6]);self.requests.append((fn,start,value))
                time.sleep(.015)
                if fn==3:resp=bytes([addr,3,value*2])+struct.pack('>'+str(value)+'H',*self.reg[start:start+value])
                elif fn==6:self.reg[start]=value;resp=req[:6]
                elif fn==16:
                    if self.exception:self.exception=False;resp=bytes([addr,0x90,1])
                    else:self.reg[start:start+value]=struct.unpack('>'+str(value)+'H',req[7:-2]);resp=req[:6]
                else:raise AssertionError(fn)
                resp=frame(resp)
                if self.bad:self.bad=False;resp=resp[:-1]+bytes([resp[-1]^1])
                for chunk in [resp[:2],resp[2:3],resp[3:]]:
                    try:os.write(self.master,chunk)
                    except OSError:return
                    time.sleep(.0002)
    def close(self):self.running=False;self.thread.join(1);os.close(self.master);os.close(self.slave)

def connect(port):return socket.create_connection(('127.0.0.1',port),timeout=5)
def call(port,**request):
    with connect(port) as s:
        s.sendall(json.dumps(request).encode()+b'\n');return json.loads(s.makefile('rb').readline())
def wait_for(f,timeout=5):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        try:
            v=f()
            if v:return v
        except (OSError,ValueError):pass
        time.sleep(.01)
    raise AssertionError('condition timed out')

def run(enabled, model=60067, max_v=6100, max_i=6100, max_ovp=6200, max_ocp=6200, extra=()):
    d=Device()
    d.reg[0]=model
    with socket.socket() as reserve:reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
    args=[sys.argv[1],'--device',d.path,'--port',str(port),'--period-ms','30',*extra]
    if enabled:args+=['--allow-writes','--allow-multiwrite']
    proc=subprocess.Popen(args,stderr=subprocess.PIPE)
    try:
        wait_for(lambda:call(port,cmd='snapshot')['sequence']>2)
        caps=call(port,cmd='registers')
        assert caps['model']==model and len(caps['registers'])==121
        assert caps['registers'][8]['raw_max']==max_v
        assert caps['registers'][9]['raw_max']==max_i
        assert caps['registers'][82]['raw_max']==max_ovp
        assert caps['registers'][83]['raw_max']==max_ocp
        backlight_supported = model not in (8009, 60125)
        assert caps['registers'][72]['writable']==backlight_supported
        assert caps['registers'][72]['raw_max']==5
        # Two subscribers consume the same sequence, rather than driving two polls.
        streams=[]
        for _ in range(2):
            s=connect(port);s.sendall(b'{"cmd":"subscribe"}\n');streams.append((s,s.makefile('rb')))
        one=json.loads(streams[0][1].readline());two=json.loads(streams[1][1].readline())
        assert abs(one['sequence']-two['sequence'])<=1
        for _ in range(3):
            next_one=json.loads(streams[0][1].readline());assert next_one['sequence']>one['sequence'];one=next_one
        for s,f in streams:f.close();s.close()
        before=call(port,cmd='snapshot')['cache_hits']
        for _ in range(30):assert call(port,cmd='read',start=10,count=4,max_age_ms=1000)['values']==[421,700,0,295]
        assert call(port,cmd='snapshot')['cache_hits']>=before+30
        # Eight simultaneous identical cold requests must share queued/active work.
        barrier=threading.Barrier(8)
        def read_cold(_):barrier.wait();return call(port,cmd='read',start=80,count=4)
        before=sum(fn==3 and st==80 for fn,st,n in d.requests)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:responses=list(pool.map(read_cold,range(8)))
        assert all(r['ok'] for r in responses)
        assert sum(fn==3 and st==80 for fn,st,n in d.requests)-before<=2
        assert call(port,cmd='snapshot')['coalesced']>=6
        assert call(port,cmd='read',start=120,count=2)['error']==-6
        with connect(port) as s:
            s.sendall(b'{"cmd":"read","count":-1}\n');assert not json.loads(s.makefile('rb').readline())['ok']
        if not enabled:
            assert call(port,cmd='write',start=18,values=[0])['error']==-8
            assert all(fn==3 for fn,_,_ in d.requests)
        else:
            assert call(port,cmd='write',start=8,values=[max_v])['ok']
            assert call(port,cmd='write',start=8,values=[max_v+1])['error']==-6
            assert call(port,cmd='write',start=9,values=[max_i])['ok']
            assert call(port,cmd='write',start=82,values=[max_ovp])['ok']
            assert call(port,cmd='write',start=83,values=[max_ocp])['ok']
            assert call(port,cmd='write',start=15,values=[1])['ok']
            assert call(port,cmd='write',start=19,values=[0])['ok']
            assert call(port,cmd='write',start=116,values=[max_v,max_i,max_ovp,max_ocp])['ok']
            assert d.reg[116:120]==[max_v,max_i,max_ovp,max_ocp]
            assert call(port,cmd='write',start=116,values=[max_v+1,max_i,max_ovp,max_ocp])['error']==-6
            assert call(port,cmd='write',start=18,values=[0])['ok']
            assert call(port,cmd='read',start=18,count=1)['values']==[0]
            assert call(port,cmd='write',start=8,values=[100,200])['ok'];assert d.reg[8:10]==[100,200]
            d.exception=True
            before=sum(fn==16 for fn,_,_ in d.requests)
            assert call(port,cmd='write',start=8,values=[100,200])['error']==-9
            assert sum(fn==16 for fn,_,_ in d.requests)==before+1
            assert call(port,cmd='write',start=18,values=[2])['error']==-6
            readonly=call(port,cmd='write',start=16,values=[0])
            assert readonly['error']==-8,readonly
            if backlight_supported:
                assert call(port,cmd='write',start=72,values=[5])['ok']
                assert call(port,cmd='write',start=72,values=[6])['error']==-6
            else:
                assert call(port,cmd='write',start=72,values=[3])['error']==-8
        # A CRC failure must not publish a corrupt sample; worker must recover.
        old=call(port,cmd='snapshot');d.bad=True
        wait_for(lambda:call(port,cmd='snapshot')['failures']>old['failures'])
        wait_for(lambda:call(port,cmd='snapshot')['sequence']>old['sequence']+2)
        assert proc.poll() is None
    finally:
        proc.terminate()
        try:proc.wait(4)
        except subprocess.TimeoutExpired:proc.kill();proc.wait();raise
        log=proc.stderr.read().decode();d.close()
        assert proc.returncode==0,(proc.returncode,log)

run(False)
profiles=[(60067,6100,6100,6200,6200),(60124,6100,1210,6200,1220),
          (60065,61000,61000,62000,62000),(8009,8100,9100,8200,9200),
          (60309,6100,3010,6200,3020)]
for model,vmax,imax,ovp,ocp in profiles:run(True,model,vmax,imax,ovp,ocp)
run(True,model=6006)
# Unknown IDs become writable only with an explicit --model-profile.
run(True,12345,30000,30000,31000,31000,extra=('--model-profile','12345,1000,10000,30,3,31,3.1'))
for bad in ['12345,50,1000,30,3,31,3.1','12345,100,1000,700,3,31,3.1','60067,100,1000,61,6.1,62,6.2','12345,100,1000,30,3,31']:
    r=subprocess.run([sys.argv[1],'--device','/dev/null','--model-profile',bad],capture_output=True)
    assert r.returncode==2,(bad,r)
print('PTY: all supported model scales/ranges, custom model profiles, HR72 limits, descriptor API, 0x06/0x10, CRC recovery and shutdown passed')
