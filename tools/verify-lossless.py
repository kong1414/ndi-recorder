import argparse,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument("file",type=Path);p.add_argument("--output",type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[1];ff=root/"vendor"/"ffmpeg"/"bin"/"ffmpeg.exe"
expected=json.loads(Path(str(a.file)+".checksums.json").read_text(encoding="utf-8-sig"))
if expected["segments"]!=1:raise SystemExit("Verify each session with a concat input when multiple segments are present")
def digest(args):
    text=subprocess.check_output([str(ff),"-v","error","-i",str(a.file),*args,"-f","md5","pipe:1"],stderr=subprocess.DEVNULL).decode().strip()
    return text.split("=")[-1]
v=digest(["-map","0:v:0","-pix_fmt",expected["pixel_format"],"-fps_mode","passthrough"])
au=digest(["-map","0:a:0","-c:a","pcm_f32le"])
report={"file":str(a.file),"pixel_format":expected["pixel_format"],"frames":expected["frames"],"video_exact":v==expected["video_md5"],"audio_exact":au==expected["audio_md5"],"sample_rate":expected["sample_rate"]}
print(json.dumps(report,ensure_ascii=False,indent=2))
if a.output:a.output.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
raise SystemExit(0 if report["video_exact"] and report["audio_exact"] else 1)

