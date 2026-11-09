import json,sqlite3,subprocess,time
from pathlib import Path
r=Path(__file__).resolve().parents[1];exe=r/'build/Release/ndi-recorder.exe';out=r/'build/ui-refinement';out.mkdir(exist_ok=True)
source=sqlite3.connect(r/'build/ui-acceptance/recorder.db')
base=json.loads(source.execute("SELECT v FROM config WHERE k='main'").fetchone()[0]);base['settings']['uiScale']=1.
for c in base['channels']:c['enabled']=False
for p in base['plans']:p['enabled']=False
results=[]
for case in ['test-listen','test-cancel-listen','plan','export','import']:
 data=out/case;data.mkdir(exist_ok=True);db=sqlite3.connect(data/'recorder.db');source.backup(db)
 db.execute("UPDATE config SET v=? WHERE k='main'",(json.dumps(base),));db.execute('DELETE FROM history')
 now=int(time.time()*1000)
 for i in range(3):
  db.execute("INSERT INTO history(channel,task,path,start,end,state,detail) VALUES(?,?,?,?,?,?,?)",(base['channels'][0]['key'],'ui-fixture','fixture-part-'+str(i)+'.mp4',now-3300000+i*900000,now-2400000+i*900000,'completed','UI rendering fixture'))
 db.commit();db.close()
 bmp=data/'window.bmp'
 app=subprocess.run([str(exe),'--data',str(data),'--capture',str(bmp),'--capture-panel',case,'--exit-after','6'],creationflags=subprocess.CREATE_NO_WINDOW,timeout=20)
 a=json.loads(Path(str(bmp)+'.initial.json').read_text());b=json.loads(Path(str(bmp)+'.json').read_text())
 good=app.returncode==0 and b['uiScale']==1
 if case=='test-listen':good=good and not a['listening'] and len(b['listening'])==1
 if case=='test-cancel-listen':good=good and len(a['listening'])==1 and not b['listening']
 if case=='plan':good=good and b['planStart'].endswith(':00:00') and b['planEnd'].endswith(':00:00')
 results.append({'case':case,'pass':good,'state':b});print(case,'PASS' if good else 'FAIL',flush=True)
(out/'result.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf-8')
assert all(x['pass'] for x in results)
