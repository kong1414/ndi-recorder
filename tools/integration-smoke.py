"""Bounded one-channel integration suite; all subprocesses are owned by this test."""
import argparse,json,os,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument("--exe",type=Path,default=ROOT/"build"/"Release"/"ndi-test-runner.exe");p.add_argument("--long",action="store_true");p.add_argument("--only",default="");a=p.parse_args()
exe=a.exe.resolve();ff=ROOT/"vendor"/"ffmpeg"/"bin";out=ROOT/"build"/"acceptance";out.mkdir(exist_ok=True)
flags=subprocess.CREATE_NO_WINDOW
reports=[]
cases=[("p60",60,12,"h264",None),("i50",50,12,"h264",None),("lossless_i50",50,6,"ffv1",None),("lapse",60,12,"h264",6),("lapse_sparse",60,12,"h264",120)]
if a.long:cases.append(("segments_outage",60,70,"h264",1))
if a.only:cases=[case for case in cases if case[0]==a.only]
if not cases:raise SystemExit("No matching test case")
for label,mode,duration,codec,lapse in cases:
    source_name="RecorderAcceptance_"+label
    ndi_name=os.environ["COMPUTERNAME"]+" ("+source_name+")"
    source_args=[str(exe),"--test-source",source_name,str(mode),str(duration+25)]
    if label=="segments_outage":source_args+=["30","3"]
    sender=subprocess.Popen(source_args,creationflags=flags)
    receiver=None
    try:
        time.sleep(1)
        args=[str(exe),"--record-test",ndi_name,str(duration),codec,"auto"]
        if lapse is not None:args.append(str(lapse))
        if label=="segments_outage":args.append("1")
        receiver=subprocess.Popen(args,creationflags=flags)
        code=receiver.wait(timeout=duration+30)
        detail=json.loads((exe.parent/"test-data"/"last-test.json").read_text(encoding="utf-8-sig"))
        report={"case":label,"exit":code,"worker":detail}
        if code==0:
            file=Path(detail["file"])
            if codec=="ffv1":
                result=subprocess.run(["python",str(ROOT/"tools"/"verify-lossless.py"),str(file),"--output",str(out/(label+".json"))],capture_output=True,text=True)
                report["check_exit"]=result.returncode;report["check"]=result.stdout
            elif lapse is None:
                result=subprocess.run(["python",str(ROOT/"tools"/"measure-sync.py"),str(file),"--output",str(out/(label+".json"))],capture_output=True,text=True)
                report["check_exit"]=result.returncode;report["check"]=result.stdout
            elif label.startswith("lapse"):
                info=json.loads(subprocess.check_output([str(ff/"ffprobe.exe"),"-v","error","-show_streams","-show_format","-of","json",str(file)]))
                duration_out=float(info["format"]["duration"])
                report["check_exit"]=0 if len(info["streams"])==1 and abs(duration_out-duration/lapse)<0.1 else 1
                report["output_duration"]=duration_out
                pixels=subprocess.check_output([str(ff/"ffmpeg.exe"),"-v","error","-i",str(file),"-map","0:v:0","-vf","scale=1:1,format=gray","-f","rawvideo","pipe:1"])
                report["nonblack"]=max(pixels,default=0)>10
                if not report["nonblack"]:report["check_exit"]=1
            else:
                files=sorted(file.parent.glob("*_"+file.stem.rsplit("_",2)[-2]+"_*.mp4"))
                report["segments"]=[str(f) for f in files];report["check_exit"]=0 if len(files)==2 else 1
                report["sync_checks"]=[]
                for index,f in enumerate(files):
                    res=subprocess.run(["python",str(ROOT/"tools"/"measure-sync.py"),str(f),"--output",str(out/(label+"_"+str(index)+".json"))],capture_output=True,text=True)
                    report["sync_checks"].append({"exit":res.returncode,"result":res.stdout})
                    if res.returncode:report["check_exit"]=1
        else:report["check_exit"]=1
        if code==0:
            stats=detail["stats"]
            monitor_ok=stats["complete"] and stats["sampled"] and stats["frames"]==detail["frames"] and stats["bytes"]>0
            if label=="segments_outage":monitor_ok=monitor_ok and stats["black"]>0 and stats["silentSamples"]>0 and stats["bytes"]==sum(Path(f).stat().st_size for f in report["segments"])
            if label.startswith("lapse"):monitor_ok=monitor_ok and stats["converted"]>0 and stats["filterVideo"]==0 and stats["black"]==0
            report["monitor_pass"]=monitor_ok
            if not monitor_ok:report["check_exit"]=1
        reports.append(report)
        (out/("suite-"+a.only+".json" if a.only else "suite.json")).write_text(json.dumps(reports,ensure_ascii=False,indent=2),encoding="utf-8")
        print(label,"PASS" if report.get("check_exit")==0 else "FAIL",flush=True)
    finally:
        if receiver and receiver.poll() is None:receiver.terminate();receiver.wait(timeout=10)
        if sender.poll() is None:sender.terminate();sender.wait(timeout=10)
raise SystemExit(0 if all(x["exit"]==0 and x.get("check_exit")==0 for x in reports) else 1)

