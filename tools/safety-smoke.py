"""Safety behaviors without filling disks or killing user processes."""
import json,sqlite3,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parents[1];exe=root/"build/Release/ndi-recorder.exe";target=root/"build/safety-acceptance";target.mkdir(exist_ok=True)
now=int(time.time()*1000);day=((now+28800000)//86400000)*86400000-28800000
settings={"storage":str(target/"recordings"),"reserveGB":1000000}
channel={"key":"disk-safety","name":"磁盘余量验收","source":"NoSource (Test)","enabled":True,"profile":{}}
plan={"key":"safety-plan","channel":"disk-safety","name":"禁止空间不足启动","repeat":0,"date":day,"start":int((now-day)//1000),"end":int((now-day)//1000)+120}
config={"version":1,"settings":settings,"channels":[channel],"plans":[plan]}
db=sqlite3.connect(target/"recorder.db")
db.execute("CREATE TABLE IF NOT EXISTS config(k TEXT PRIMARY KEY,v TEXT NOT NULL)")
db.execute("CREATE TABLE IF NOT EXISTS history(id INTEGER PRIMARY KEY,channel TEXT,task TEXT,path TEXT,start INTEGER,end INTEGER,state TEXT,detail TEXT)")
db.execute("DELETE FROM history")
db.execute("INSERT OR REPLACE INTO config VALUES('main',?)",(json.dumps(config,ensure_ascii=False),))
# Simulate an interrupted known file, never alter/delete the recording itself.
suite=json.loads((root/"build/acceptance/suite.json").read_text(encoding="utf-8"))
existing=suite[0]["worker"]["file"]
db.execute("INSERT INTO history(channel,task,path,start,end,state,detail) VALUES('recovery','recover-test',?,?,0,'recording','')",(existing,now-12000))
db.commit();db.close()
app=subprocess.Popen([str(exe),"--data",str(target),"--exit-after","7"],creationflags=subprocess.CREATE_NO_WINDOW)
code=app.wait(timeout=20)
db=sqlite3.connect(target/"recorder.db")
recordings=db.execute("SELECT COUNT(*) FROM history WHERE channel='disk-safety' AND path<>''").fetchone()[0]
recovery=db.execute("SELECT state,detail FROM history WHERE task='recover-test'").fetchone()
report={"exit":code,"low_disk_started_recordings":recordings,"recovery_state":recovery[0],"original_preserved":Path(existing).is_file(),
        "pass":code==0 and recordings==0 and recovery[0]=="recovered" and Path(existing).is_file()}
(target/"result.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
print(json.dumps(report,ensure_ascii=False,indent=2))
raise SystemExit(0 if report["pass"] else 1)

