"""Bounded monitoring A/B acceptance using existing receiver and deterministic NDI sender.
Windows process counters only; no dependency install, disk benchmark or per-frame probing.
"""
import ctypes,json,os,subprocess,time
from ctypes import wintypes as w
from pathlib import Path
r=Path(__file__).resolve().parents[1];exe=r/'build/Release/ndi-test-runner.exe'
out=r/'build/monitor-acceptance';out.mkdir(exist_ok=True)
k=ctypes.WinDLL('kernel32',use_last_error=True);ps=ctypes.WinDLL('psapi',use_last_error=True)
k.GetProcessTimes.argtypes=[w.HANDLE]+[ctypes.POINTER(ctypes.c_ulonglong)]*4
class Memory(ctypes.Structure):
 _fields_=[('cb',w.DWORD),('faults',w.DWORD)]+[(x,ctypes.c_size_t) for x in ['peakWS','working','peakPaged','paged','peakNonPaged','nonPaged','pagefile','peakPagefile','private']]
ps.GetProcessMemoryInfo.argtypes=[w.HANDLE,ctypes.POINTER(Memory),w.DWORD]
def cpu(p):
 a,b,c,d=(ctypes.c_ulonglong() for _ in range(4))
 if not k.GetProcessTimes(w.HANDLE(int(p._handle)),ctypes.byref(a),ctypes.byref(b),ctypes.byref(c),ctypes.byref(d)):raise ctypes.WinError()
 return (c.value+d.value)/1e7
reports=[];flags=subprocess.CREATE_NO_WINDOW
for enabled in [False,True,True,False]:
 label=('on' if enabled else 'off')+'-'+str(len(reports)+1)
 sender=subprocess.Popen([str(exe),'--test-source','MonitorAB','60','40'],creationflags=flags)
 rec=None
 try:
  time.sleep(1)
  env=dict(os.environ);env.pop('NDI_RECORDER_DISABLE_STATS',None)
  if not enabled:env['NDI_RECORDER_DISABLE_STATS']='1'
  rec=subprocess.Popen([str(exe),'--record-test',os.environ['COMPUTERNAME']+' (MonitorAB)','18','h264','auto'],env=env,creationflags=flags)
  begin=time.monotonic();base_cpu=None;base_time=None;memory=[];private=[]
  while rec.poll() is None:
   t=time.monotonic()
   if t-begin>5:
    if base_cpu is None:base_cpu=cpu(rec);base_time=t
    m=Memory();m.cb=ctypes.sizeof(m)
    if ps.GetProcessMemoryInfo(w.HANDLE(int(rec._handle)),ctypes.byref(m),m.cb):memory.append(m.working/1048576);private.append(m.private/1048576)
   if t-begin>45:raise TimeoutError(label)
   time.sleep(.25)
  duration=time.monotonic()-base_time
  # Process handle remains valid after exit.
  processor=(cpu(rec)-base_cpu)/duration*100
  detail=json.loads((exe.parent/'test-data/last-test.json').read_text(encoding='utf-8'))
  file=Path(detail['file']);size=file.stat().st_size
  sync=subprocess.run(['python',str(r/'tools/measure-sync.py'),str(file),'--output',str(out/(label+'-sync.json'))],capture_output=True,text=True)
  good=rec.returncode==0 and detail['frames']==1080 and sync.returncode==0
  stats=detail['stats']
  if enabled:good=good and stats['complete'] and stats['bytes']==size and stats['frames']==1080 and stats['black']==0 and stats['filterVideo']==0
  else:good=good and not stats['sampled']
  reports.append({'case':label,'monitoring':enabled,'cpu_one_core_percent':processor,'working_MB_mean':sum(memory)/len(memory),'private_MB_mean':sum(private)/len(private),'peak_working_MB':max(memory),'file_bytes':size,'stats':stats,'sync_exit':sync.returncode,'pass':good})
  (out/'ab.json').write_text(json.dumps(reports,indent=2),encoding='utf-8')
  print(label,'PASS' if good else 'FAIL','CPU',round(processor,2),'private MB',round(sum(private)/len(private),2),flush=True)
 finally:
  for p in [rec,sender]:
   if p and p.poll() is None:p.terminate();p.wait(timeout=10)
raise SystemExit(0 if all(x['pass'] for x in reports) else 1)
