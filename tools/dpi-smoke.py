"""Win10/11 UI launch/scale smoke. Uses isolated config; never changes system DPI."""
import argparse,ctypes,json,sqlite3,subprocess,time
from ctypes import wintypes as w
from pathlib import Path
r=Path(__file__).resolve().parents[1]; out=r/'build/dpi-acceptance';out.mkdir(exist_ok=True)
exe=r/'build/Release/ndi-recorder.exe';u=ctypes.WinDLL('user32',use_last_error=True)
u.GetDpiForWindow.argtypes=[w.HWND];u.GetDpiForWindow.restype=w.UINT
u.SetWindowPos.argtypes=[w.HWND,w.HWND,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,w.UINT]
cbtype=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
u.EnumWindows.argtypes=[cbtype,w.LPARAM]
base=json.loads(sqlite3.connect(r/'build/ui-acceptance/recorder.db').execute("SELECT v FROM config WHERE k='main'").fetchone()[0])
base['plans']=[]
for c in base['channels']:c['enabled']=False
reports=[]
parser=argparse.ArgumentParser();parser.add_argument('--all-scales',action='store_true');options=parser.parse_args()
cases=[(x,'',False) for x in [.8,.9,1.,1.1,1.25,1.5]]+[(1.5,'settings',False),(1.5,'channel',False),(1.5,'settings',True),(1.5,'',True),(1.,'dpi-cycle',False)] if options.all_scales else [(1.,'',False),(1.,'settings',False),(1.,'channel',False),(1.,'settings',True),(1.,'',True)]
for scale,panel,small in cases:
 label=str(int(scale*100))+'-'+(panel or 'main')+('-small' if small else '')
 data=out/label;data.mkdir(exist_ok=True);base['settings']['uiScale']=scale;base['settings']['storage']=str(data/'recordings')
 db=sqlite3.connect(data/'recorder.db');db.execute('CREATE TABLE IF NOT EXISTS config(k TEXT PRIMARY KEY,v TEXT NOT NULL)');db.execute("INSERT OR REPLACE INTO config VALUES('main',?)",(json.dumps(base),));db.commit();db.close()
 bmp=data/'window.bmp';args=[str(exe),'--data',str(data),'--capture',str(bmp),'--exit-after',('8' if panel=='dpi-cycle' else '6')]
 if panel:args+=['--capture-panel',panel]
 app=subprocess.Popen(args,creationflags=subprocess.CREATE_NO_WINDOW)
 try:
  found=[]
  @cbtype
  def enum(h,l):
   pid=w.DWORD();u.GetWindowThreadProcessId(h,ctypes.byref(pid))
   if pid.value==app.pid:found.append(h)
   return True
  for _ in range(30):
   u.EnumWindows(enum,0)
   if found:break
   time.sleep(.1)
  h=next((h for h in found if u.IsWindowVisible(h)),found[0] if found else None)
  dpi=u.GetDpiForWindow(h) if h else 0
  if small and h:u.SetWindowPos(h,None,60,60,640,480,0x14)
  code=app.wait(timeout=20)
  reports.append({'case':label,'exit':code,'dpi':dpi,'screenshot':str(bmp),'pass':code==0 and bmp.exists() and bmp.stat().st_size>10000})
  print(label,reports[-1]['pass'],flush=True)
 finally:
  if app.poll() is None:app.terminate();app.wait(timeout=10)
(out/'result.json').write_text(json.dumps(reports,indent=2),encoding='utf-8')
raise SystemExit(0 if all(x['pass'] for x in reports) else 1)
