#!/usr/bin/env python3
"""Explicit read-only daemon benchmark. Run manually with a physical device."""
import concurrent.futures,json,pathlib,socket,subprocess,sys,time,statistics,threading
binary=sys.argv[1];device=sys.argv[2];output=pathlib.Path(sys.argv[3]);port=19607
p=subprocess.Popen([binary,'--device',device,'--port',str(port)],stderr=subprocess.PIPE)
def connect():return socket.create_connection(('127.0.0.1',port),timeout=5)
def call(cmd):
    with connect() as s:s.sendall(json.dumps(cmd).encode()+b'\n');return json.loads(s.makefile('rb').readline())
def stats(a):
    a=sorted(a);return {'p50':statistics.median(a),'p95':a[int((len(a)-1)*.95)],'p99':a[int((len(a)-1)*.99)],'max':max(a)}
def phase(clients,n=64):
    barrier=threading.Barrier(clients)
    def consume(_):
        with connect() as s:
            f=s.makefile('rb');barrier.wait();s.sendall(b'{"cmd":"subscribe"}\n');frames=[json.loads(f.readline()) for _ in range(n)]
            intervals=[(b['completed_ns']-a['completed_ns'])/1e6 for a,b in zip(frames,frames[1:])]
            return {'samples':n,'hz':(n-1)*1e9/(frames[-1]['completed_ns']-frames[0]['completed_ns']),'interval_ms':stats(intervals),'missed_sequences':sum(b['sequence']-a['sequence']-1 for a,b in zip(frames,frames[1:])),'sequence_first':frames[0]['sequence'],'sequence_last':frames[-1]['sequence']}
    before=call({'cmd':'snapshot'});begin=time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as ex:result=list(ex.map(consume,range(clients)))
    after=call({'cmd':'snapshot'});return {'clients':clients,'wall_s':time.monotonic()-begin,'wire_transactions':after['transactions']-before['transactions'],'sample_sequences':after['sequence']-before['sequence'],'new_failures':after['failures']-before['failures'],'consumers':result}
try:
    for _ in range(100):
        try:
            s=call({'cmd':'snapshot'})
            if s['sequence']>=3:break
        except OSError:pass
        time.sleep(.05)
    else:raise RuntimeError('daemon not ready')
    result={'device':device,'all_requests_read_only':True,'initial':call({'cmd':'snapshot'}),'single':phase(1),'multi':phase(8)}
    delays=[];before=call({'cmd':'snapshot'})
    with connect() as s:
        f=s.makefile('rb')
        for _ in range(1000):
            begin=time.perf_counter_ns();s.sendall(b'{"cmd":"read","start":10,"count":4,"max_age_ms":1000}\n');reply=json.loads(f.readline());delays.append((time.perf_counter_ns()-begin)/1e6);assert reply['ok']
    after=call({'cmd':'snapshot'});result['cache']={'calls':1000,'roundtrip_ms':stats(delays),'cache_hits_added':after['cache_hits']-before['cache_hits'],'wire_transactions':after['transactions']-before['transactions'],'sample_sequences':after['sequence']-before['sequence']}
    result['final']=after;output.write_text(json.dumps(result,indent=2));print(json.dumps({k:v for k,v in result.items() if k not in ['initial','final']}))
finally:
    p.terminate();p.wait(5);print(p.stderr.read().decode(),file=sys.stderr)
