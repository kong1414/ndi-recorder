"""Delayed and absent source regressions; owns only its test processes."""
import json, os, sqlite3, subprocess, time
from pathlib import Path
root = Path(__file__).resolve().parents[1]
exe = root / "build/Release/ndi-test-runner.exe"
flags = subprocess.CREATE_NO_WINDOW
out = root / "build/recovery-acceptance"
out.mkdir(exist_ok=True)
results = []

def run_case(label, late):
    name = "RecorderRecovery_" + label + "_" + str(os.getpid())
    receiver = subprocess.Popen([str(exe), "--record-test", os.environ["COMPUTERNAME"] + " (" + name + ")",
                                 "10" if late else "1", "h264", "auto"], creationflags=flags)
    sender = None
    try:
        if late:
            time.sleep(8)
            sender = subprocess.Popen([str(exe), "--test-source", name, "60", "15"], creationflags=flags)
        code = receiver.wait(timeout=35)
        detail = json.loads((exe.parent / "test-data/last-test.json").read_text(encoding="utf-8-sig"))
        result = {"case": label, "exit": code, "worker": detail}
        result["pass"] = code == 0 and (detail["frames"] > 0 and detail["fps"] == 60 if late
                                       else detail["frames"] == 0 and not detail["file"])
        if late and result["pass"]:
            with sqlite3.connect(exe.parent / "test-data/recorder.db") as db:
                task = db.execute("SELECT task FROM history WHERE path=?", (detail["file"],)).fetchone()[0]
                rows = db.execute("SELECT start,end,state,detail FROM history WHERE task=? ORDER BY id", (task,)).fetchall()
            result["history"] = rows
            result["pass"] = any(row[2] == "no_video" and row[1] > row[0] for row in rows) and not any(row[2] == "failed" for row in rows)
        results.append(result)
        print(label, "PASS" if result["pass"] else "FAIL", flush=True)
    finally:
        for process in (receiver, sender):
            if process and process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

run_case("late_p60", True)
run_case("absent_until_deadline", False)
(out / "result.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
raise SystemExit(0 if all(result["pass"] for result in results) else 1)
