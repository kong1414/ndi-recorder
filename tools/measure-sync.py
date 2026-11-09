"""Measure the built-in NDI test source's flash / tone onset offset."""
import argparse, array, json, statistics, subprocess
from pathlib import Path

def run(args):
    return subprocess.check_output([str(x) for x in args], stderr=subprocess.DEVNULL)
def assess_sync(flashes, tones, duration):
    # Exclude startup and the final partial pulse, not inconvenient large offsets.
    end = duration - 0.25
    video_events = [t for t in flashes if 1 <= t < end]
    audio_events = [t for t in tones if 1 <= t < end]
    unused = set(range(len(audio_events)))
    offsets = []
    for t in video_events:
        if not unused:
            continue
        index = min(unused, key=lambda i: abs(audio_events[i] - t))
        if abs(audio_events[index] - t) < 0.25:
            unused.remove(index)
            offsets.append((audio_events[index] - t) * 1000)
    unmatched_video = len(video_events) - len(offsets)
    unmatched_audio = len(unused)
    coverage = len(offsets) / max(len(video_events), len(audio_events), 1)
    return {
        "matched_pulses": len(offsets),
        "unmatched_video": unmatched_video,
        "unmatched_audio": unmatched_audio,
        "match_coverage": coverage,
        "offset_ms_audio_minus_video": offsets,
        "max_abs_offset_ms": max(map(abs, offsets), default=None),
        "median_offset_ms": statistics.median(offsets) if offsets else None,
        "pass_40ms": len(offsets) >= 3 and unmatched_video == 0 and unmatched_audio == 0
                     and max(map(abs, offsets)) <= 40,
        "requires_nonperiodic_source": True,
    }

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("file",type=Path)
    parser.add_argument("--ffmpeg-dir",type=Path,default=Path(__file__).resolve().parents[1]/"vendor"/"ffmpeg"/"bin")
    parser.add_argument("--output",type=Path)
    a=parser.parse_args()
    probe=a.ffmpeg_dir/"ffprobe.exe";ff=a.ffmpeg_dir/"ffmpeg.exe"
    meta=json.loads(run([probe,"-v","error","-show_streams","-show_format","-of","json",a.file]))
    video=next(x for x in meta["streams"] if x["codec_type"]=="video")
    audio=next((x for x in meta["streams"] if x["codec_type"]=="audio"),None)
    pts=json.loads(run([probe,"-v","error","-select_streams","v:0","-show_entries","frame=best_effort_timestamp_time","-of","json",a.file]))["frames"]
    raw=run([ff,"-v","error","-i",a.file,"-map","0:v:0","-vf","scale=16:16:flags=area,format=gray","-fps_mode","passthrough","-f","rawvideo","pipe:1"])
    flashes=[];was=False
    for i,p in enumerate(pts):
        block=raw[i*256:(i+1)*256]
        if not block:break
        lit=sum(block)/len(block)>170
        if lit and not was:flashes.append(float(p["best_effort_timestamp_time"]))
        was=lit
    tones=[]
    if audio:
        pcm=array.array("f",run([ff,"-v","error","-i",a.file,"-map","0:a:0","-ac","1","-ar","48000","-f","f32le","pipe:1"]))
        start=float(audio.get("start_time",0))
        for i in range(0,len(pcm),48):
            chunk=pcm[i:i+48]
            if max((abs(v) for v in chunk),default=0)>0.01:
                t=start+i/48000
                if not tones or t-tones[-1]>0.5:tones.append(t)
    sync = assess_sync(flashes, tones, float(meta["format"].get("duration", 0)))
    report={"file":str(a.file),"codec":video["codec_name"],"width":video["width"],"height":video["height"],
            "fps":video["avg_frame_rate"],"field_order":video.get("field_order"),"duration":meta["format"].get("duration"),
            "audio":None if audio is None else {"codec":audio["codec_name"],"sample_rate":audio["sample_rate"],"channels":audio["channels"]},
            "flash_count":len(flashes),"tone_count":len(tones),**sync,
            "note":"Use the nonperiodic built-in test source; periodic legacy pulses cannot rule out whole-second shifts. Short test only; inspect segment boundaries and long-run drift separately."}
    text=json.dumps(report,ensure_ascii=False,indent=2)
    print(text)
    if a.output:a.output.write_text(text,encoding="utf-8")
    return 0 if report["pass_40ms"] else 1
if __name__=="__main__":raise SystemExit(main())

