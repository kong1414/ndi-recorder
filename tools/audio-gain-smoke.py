import array,json,math,os,subprocess,time
from pathlib import Path
r=Path(__file__).resolve().parents[1];exe=r/'build/Release/ndi-test-runner.exe';out=r/'build/audio-acceptance';out.mkdir(exist_ok=True)
flags=subprocess.CREATE_NO_WINDOW
sender=subprocess.Popen([str(exe),'--test-source','AudioGainTest','60','35'],creationflags=flags);rec=None
try:
 time.sleep(1)
 rec=subprocess.Popen([str(exe),'--record-test',os.environ['COMPUTERNAME']+' (AudioGainTest)','18','h264','auto','1','60','6'],creationflags=flags)
 code=rec.wait(timeout=45);j=json.loads((exe.parent/'test-data/last-test.json').read_text(encoding='utf-8'))
 raw=subprocess.check_output([str(r/'vendor/ffmpeg/bin/ffmpeg.exe'),'-v','error','-i',j['file'],'-map','0:a:0','-ac','1','-ar','48000','-f','f32le','pipe:1'])
 pcm=array.array('f',raw)
 first=max(abs(v) for v in pcm[2*48000:7*48000]);second=max(abs(v) for v in pcm[11*48000:16*48000]);delta=20*math.log10(second/first)
 sync=subprocess.run(['python',str(r/'tools/measure-sync.py'),j['file'],'--output',str(out/'sync.json')],capture_output=True,text=True)
 report={'exit':code,'measured_gain_db':delta,'first_peak':first,'second_peak':second,'frames':j['frames'],'sync_exit':sync.returncode,'stats':j['stats'],'pass':code==0 and j['frames']==1080 and abs(delta-6)<.6 and sync.returncode==0}
 (out/'result.json').write_text(json.dumps(report,indent=2),encoding='utf-8');print(json.dumps(report,indent=2));raise SystemExit(0 if report['pass'] else 1)
finally:
 for p in [rec,sender]:
  if p and p.poll() is None:p.terminate();p.wait(timeout=10)
