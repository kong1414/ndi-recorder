"""UI rendering + real scheduler/worker + single-instance integration test."""
import json,os,sqlite3,subprocess,time
from pathlib import Path
r=Path(__file__).resolve().parents[1];exe=r/"build/Release/ndi-recorder.exe";senderexe=r/"build/Release/ndi-test-runner.exe";root=r/"build/ui-acceptance";root.mkdir(exist_ok=True)
flags=subprocess.CREATE_NO_WINDOW
sender=subprocess.Popen([str(senderexe),"--test-source","RecorderUISchedule","60","60"],creationflags=flags)
try:
    time.sleep(3)
    now=int(time.time()*1000);start=(now//1000+7)*1000;end=start+12_000
    day=((start+28_800_000)//86_400_000)*86_400_000-28_800_000
    profile={"codec":"h264","encoder":"auto","mbps":20,"segmentMinutes":60}
    channels=[{"key":"ui-main","name":"主画面 · 1080p60","source":os.environ["COMPUTERNAME"]+" (RecorderUISchedule)","enabled":True,"profile":profile},
              {"key":"ui-backup","name":"备份通道","source":"Unconnected (Backup)","enabled":True,"profile":profile},
              {"key":"ui-studio","name":"演播室 · 每周收录","source":"Unconnected (Studio)","enabled":True,"profile":profile}]
    def plan(key,ch,name,a,b,repeat=0):
        return {"key":key,"channel":ch,"name":name,"repeat":repeat,"date":day,"start":int((a-day)//1000)%86400,"end":int((b-day)//1000)%86400,"weekMask":127,"enabled":True,"skips":[]}
    plans=[plan("ui-live","ui-main","定时录制自动验收",start,end),
           plan("ui-later","ui-main","下午节目收录",start+1_800_000,start+6_300_000),
           plan("ui-b-plan","ui-backup","备份 · 每日计划",start+3_600_000,start+8_000_000,1),
           plan("ui-s-plan","ui-studio","演播室 · 周计划",start+6_000_000,start+9_000_000,2)]
    config={"version":1,"settings":{"storage":str(root/"recordings"),"reserveGB":1},"channels":channels,"plans":plans}
    db=sqlite3.connect(root/"recorder.db");db.execute("CREATE TABLE IF NOT EXISTS config(k TEXT PRIMARY KEY,v TEXT NOT NULL)");db.execute("INSERT OR REPLACE INTO config VALUES('main',?)",(json.dumps(config,ensure_ascii=False),));db.commit();db.close()
    app=subprocess.Popen([str(exe),"--data",str(root),"--capture",str(root/"window.bmp"),"--capture-panel","monitor","--exit-after","28"],creationflags=flags)
    time.sleep(3)
    duplicate=subprocess.Popen([str(exe),"--data",str(root)],creationflags=flags)
    duplicate_code=duplicate.wait(timeout=8)
    app_code=app.wait(timeout=40)
    db=sqlite3.connect(root/"recorder.db")
    recs=db.execute("SELECT path,start,end,state,detail FROM history WHERE channel='ui-main' AND path<>'' ORDER BY id DESC LIMIT 1").fetchall()
    stats=db.execute("SELECT data FROM task_stats WHERE task=(SELECT task FROM history WHERE channel='ui-main' AND path<>'' ORDER BY id DESC LIMIT 1)").fetchone()
    gaps=db.execute("SELECT state,detail FROM history WHERE state IN ('failed','interrupted')").fetchall()
    report={"app_exit":app_code,"duplicate_exit":duplicate_code,"planned_start":start,"planned_end":end,"recordings":recs,"failures":gaps,"stats":json.loads(stats[0]) if stats else None}
    if recs:
        info=json.loads(subprocess.check_output([str(r/"vendor/ffmpeg/bin/ffprobe.exe"),"-v","error","-show_format","-show_streams","-of","json",recs[0][0]]))
        report["duration"]=float(info["format"]["duration"])
        report["pass"]=app_code==0 and duplicate_code==0 and recs[0][3]=="completed" and not gaps and stats is not None and report["stats"]["complete"] and abs(report["duration"]-12)<0.06 and abs(recs[0][1]-start)<50
    else:report["pass"]=False
    (root/"result.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps(report,ensure_ascii=False,indent=2))
finally:
    if sender.poll() is None:sender.terminate();sender.wait(timeout=10)
raise SystemExit(0 if report["pass"] else 1)

