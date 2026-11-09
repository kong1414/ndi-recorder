import argparse,json,sqlite3,subprocess,time
from pathlib import Path
r=Path(__file__).resolve().parents[1];out=r/'build/interaction-acceptance';out.mkdir(exist_ok=True)
exe=r/'build/Release/ndi-recorder.exe'
base=json.loads(sqlite3.connect(r/'build/dpi-acceptance/100-main/recorder.db').execute("SELECT v FROM config WHERE k='main'").fetchone()[0]);base['plans']=[]
reports=[]
parser=argparse.ArgumentParser();parser.add_argument('--test-recovery',action='store_true');options=parser.parse_args()
cases=['drag','wheel','navigation','volume','follow']
if options.test_recovery:cases+=['reset-running','reset-startup']
for case in cases:
 data=out/case;data.mkdir(exist_ok=True);base['settings'].update({'uiScale':1.5 if case.startswith('reset') else 1.,'monitorVolume':1.,'storage':str(data/'recordings')})
 db=sqlite3.connect(data/'recorder.db');db.execute('CREATE TABLE IF NOT EXISTS config(k TEXT PRIMARY KEY,v TEXT NOT NULL)');db.execute("INSERT OR REPLACE INTO config VALUES('main',?)",(json.dumps(base),));db.commit();db.close()
 bmp=data/'window.bmp';initial=Path(str(bmp)+'.initial.json');final=Path(str(bmp)+'.json')
 for path in [initial,final]:path.unlink(missing_ok=True)
 args=[str(exe),'--data',str(data),'--capture',str(bmp),'--exit-after','6']
 if case.startswith('reset'):args+=['--capture-panel','settings']
 else:args+=['--capture-panel','test-'+case]
 if case=='reset-startup':args+=['--reset-ui-scale']
 app=subprocess.Popen(args,creationflags=subprocess.CREATE_NO_WINDOW)
 try:
  deadline=time.monotonic()+12
  while not initial.exists() and app.poll() is None and time.monotonic()<deadline:time.sleep(.05)
  a=json.loads(initial.read_text(encoding="utf-8"))
  if case=='reset-running':subprocess.run([str(exe),'--reset-ui-scale'],creationflags=subprocess.CREATE_NO_WINDOW,check=True,timeout=8)
  code=app.wait(timeout=15);b=json.loads(final.read_text());good=code==0 and b['plans']==a['plans']
  if case=='drag':good=good and b['view']<a['view']-1e5 and b['span']==a['span']
  elif case=='wheel':good=good and abs(b['span']/a['span']-1.25)<.001
  elif case=='navigation':good=good and b['view']>a['view']+3*86400000
  elif case=='volume':good=good and 2.5<b['monitorVolume']<3.5
  elif case=='follow':good=good and b['followNow'] and 2500<b['view']-a['view']<7000
  if case in ['drag','wheel','navigation']:good=good and not b['followNow']
  elif case.startswith('reset'):good=good and b['uiScale']==1.
  stored=json.loads(sqlite3.connect(data/'recorder.db').execute("SELECT v FROM config WHERE k='main'").fetchone()[0])
  if case=='volume':good=good and stored['settings']['monitorVolume']==b['monitorVolume']
  if case.startswith('reset'):good=good and stored['settings']['uiScale']==1.
  reports.append({'case':case,'pass':good,'initial':a,'final':b});print(case,'PASS' if good else 'FAIL',flush=True)
 finally:
  if app.poll() is None:app.terminate();app.wait(timeout=10)
(out/'result.json').write_text(json.dumps(reports,indent=2),encoding='utf-8')
raise SystemExit(0 if all(x['pass'] for x in reports) else 1)
