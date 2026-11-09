"""Reproduce a persistent sub-2-second source clock regression using a real NDI sender."""
import json, os, subprocess, time
from pathlib import Path
r=Path(__file__).resolve().parents[1];exe=r/'build/Release/ndi-recorder.exe';out=r/'build/clock-recovery';out.mkdir(exist_ok=True)
flags=subprocess.CREATE_NO_WINDOW
name='RecorderClockRegression';source=os.environ['COMPUTERNAME']+' ('+name+')'
sender=subprocess.Popen([str(exe),'--test-source',name,'60','45','0','0','-600'],creationflags=flags)
try:
 time.sleep(1)
 run=subprocess.run([str(exe),'--record-test',source,'22','h264','auto'],creationflags=flags,timeout=55)
 result=json.loads((exe.parent/'test-data/last-test.json').read_text(encoding='utf-8-sig'))
 file=Path(result['file']);assert run.returncode==0 and file.exists(),result
 pixels=subprocess.check_output([str(r/'vendor/ffmpeg/bin/ffmpeg.exe'),'-v','error','-i',str(file),'-vf','scale=1:1,format=gray','-an','-f','rawvideo','pipe:1'])
 result['tailNonblackFrames']=sum(v>2 for v in pixels[17*60:]);result['tailFrames']=len(pixels[17*60:])
 result['pass']=result['stats']['black']<180 and result['tailNonblackFrames']>len(pixels[17*60:])*.95
 (out/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
 print(json.dumps(result,indent=2));assert result['pass']
finally:
 if sender.poll() is None:sender.terminate();sender.wait(timeout=10)
