param([switch]$Stop,[string]$DataDir,[int]$DurationSeconds=0,[switch]$NoLaunch)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$exe=Join-Path $root 'ndi-recorder.exe'
$diagnostics=Join-Path $root 'diagnostics'
New-Item -ItemType Directory -Force -Path $diagnostics | Out-Null
$pointer=Join-Path $diagnostics 'active-session.txt'
if($Stop) {
    if(!(Test-Path -LiteralPath $pointer)){Write-Host '没有正在运行的诊断会话。';exit 0}
    $session=(Get-Content -LiteralPath $pointer -Raw).Trim()
    $resolved=[IO.Path]::GetFullPath($session)
    if(!$resolved.StartsWith([IO.Path]::GetFullPath($diagnostics)+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid diagnostic session path'}
    Set-Content -LiteralPath (Join-Path $resolved 'stop.flag') -Value 'stop'
    Write-Host '已请求停止采样并整理报告。录制程序会继续运行。';exit 0
}
$lock=$null
try {$lock=[IO.File]::Open((Join-Path $diagnostics 'monitor.lock'),'OpenOrCreate','ReadWrite','None')} catch {throw '已有诊断脚本运行，请勿重复启动。'}
$session=Join-Path $diagnostics ('run-'+(Get-Date -Format 'yyyyMMdd_HHmmss')+'-'+$PID)
New-Item -ItemType Directory -Path $session | Out-Null
Set-Content -LiteralPath $pointer -Value $session -Encoding UTF8
$start=Get-Date
if(!$DataDir){$DataDir=Join-Path $root 'data'}
$DataDir=[IO.Path]::GetFullPath($DataDir)
$previous=@{}
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class RecorderIo {
 [StructLayout(LayoutKind.Sequential)] public struct Counters {public ulong ReadOperations,WriteOperations,OtherOperations,ReadBytes,WriteBytes,OtherBytes;}
 [DllImport("kernel32.dll",SetLastError=true)] public static extern bool GetProcessIoCounters(IntPtr h,out Counters c);
}
"@
function FileHash($path) {
    $stream=[IO.File]::OpenRead($path);$hash=[Security.Cryptography.SHA256]::Create()
    try {return ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-','')} finally {$hash.Dispose();$stream.Dispose()}
}
function Note($text) { Add-Content -LiteralPath (Join-Path $session 'collector.log') -Encoding UTF8 -Value ((Get-Date -Format o)+' '+$text) }
function Snapshot {
    if(Test-Path -LiteralPath (Join-Path $DataDir 'recorder.db')) {
        $dest=Join-Path $session 'snapshot'
        $si=New-Object Diagnostics.ProcessStartInfo
        $si.FileName=$exe;$si.Arguments='--diagnostic-export "'+$DataDir+'" "'+$dest+'"';$si.UseShellExecute=$false;$si.CreateNoWindow=$true
        $proc=[Diagnostics.Process]::Start($si)
        if(!$proc.WaitForExit(20000)){$proc.Kill();Note 'Database snapshot timed out'}
        elseif($proc.ExitCode -ne 0){Note ('Database snapshot exit '+$proc.ExitCode)}
        $proc.Dispose()
    }
    $logs=Join-Path $DataDir 'logs'
    if(Test-Path -LiteralPath $logs) {
        $dest=Join-Path $session 'logs';New-Item -ItemType Directory -Force -Path $dest|Out-Null
        $budget=256MB
        $keep=@{}
        foreach($f in (Get-ChildItem -LiteralPath $logs -File | Where-Object {$_.LastWriteTime -ge $start.AddMinutes(-5)} | Sort-Object LastWriteTime -Descending)) {
            if($f.Length -gt $budget){continue};$budget-=$f.Length
            $keep[$f.Name]=$true
            Copy-Item -LiteralPath $f.FullName -Destination $dest -Force -ErrorAction SilentlyContinue
        }
        $safeRoot=[IO.Path]::GetFullPath($dest)+[IO.Path]::DirectorySeparatorChar
        foreach($old in Get-ChildItem -LiteralPath $dest -File) {
            if(!$keep.ContainsKey($old.Name) -and $old.FullName.StartsWith($safeRoot,[StringComparison]::OrdinalIgnoreCase)) {
                Remove-Item -LiteralPath $old.FullName -Force
            }
        }
    }
}
try {
    $other=@(Get-CimInstance Win32_Process -Filter "Name='ndi-recorder.exe'" | Where-Object {$_.ExecutablePath -ne $exe -and $_.CommandLine -notmatch '--worker|--test-|--record-test|--diagnostic-export'})
    if($other.Count -gt 0 -and !$NoLaunch){throw '另一个目录的录制程序正在运行。请先退出旧版本，再启动本目录诊断脚本。'}
    $running=@(Get-CimInstance Win32_Process -Filter "Name='ndi-recorder.exe'" | Where-Object {$_.ExecutablePath -eq $exe -and $_.CommandLine -notmatch '--worker|--test-|--record-test|--diagnostic-export'})
    if($running.Count -gt 0 -and $running[0].CommandLine -match '--data\s+(?:"([^"]+)"|(\S+))') {
        $DataDir=if($Matches[1]){$Matches[1]}else{$Matches[2]}
        $DataDir=[IO.Path]::GetFullPath($DataDir)
    }
    $os=Get-CimInstance Win32_OperatingSystem
    @{started=$start.ToString('o');version=(Get-Item -LiteralPath $exe).VersionInfo.FileVersion;sha256=(FileHash $exe);
      os=$os.Caption;build=$os.BuildNumber;logicalProcessors=[Environment]::ProcessorCount;data=$DataDir;intervalSeconds=30;
      memoryBytes=$os.TotalVisibleMemorySize*1024;timezone=[TimeZoneInfo]::Local.Id} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $session 'environment.json') -Encoding UTF8
    if(!$NoLaunch -and $running.Count -eq 0) {
        # Interactive recorder window for the user's configuration.
        Start-Process -FilePath $exe -ArgumentList @('--data',('"'+$DataDir+'"')) | Out-Null
    }
    Write-Host '诊断已启动。请在录制程序内配置来源、计划并开始录制。' -ForegroundColor Green
    Write-Host '每 30 秒采样；不会自动停止录制。运行满 24 小时后可继续运行。'
    Write-Host '结束：按 Enter，或双击 Stop-24h-Debug.cmd。建议先在程序中停止录制。'
    Write-Host ('报告目录：'+$session)
    $lastSample=[datetime]::MinValue;$lastSnapshot=[datetime]::MinValue
    do {
        $now=Get-Date
        if(($now-$lastSample).TotalSeconds -ge 30) {
            $lastSample=$now;$rows=@();$present=@{}
            foreach($proc in @(Get-Process -Name 'ndi-recorder' -ErrorAction SilentlyContinue)) {
                try {
                    if($proc.Path -ne $exe){continue}
                    $key=[string]$proc.Id+'-'+$proc.StartTime.Ticks;$present[$key]=$true
                    $io=New-Object RecorderIo+Counters
                    $ioValid=[RecorderIo]::GetProcessIoCounters($proc.Handle,[ref]$io)
                    $cpu=$proc.TotalProcessorTime.TotalSeconds;$cpuPercent=$null;$writeMBps=$null
                    if($previous.ContainsKey($key)) {
                        $old=$previous[$key];$seconds=($now-$old.at).TotalSeconds
                        if($seconds -gt 0){$cpuPercent=100*($cpu-$old.cpu)/$seconds/[Environment]::ProcessorCount;if($ioValid -and $old.valid){$writeMBps=([double]$io.WriteBytes-$old.bytes)/$seconds/1e6}}
                    }
                    $previous[$key]=@{at=$now;cpu=$cpu;bytes=$io.WriteBytes;valid=$ioValid}
                    $rows+=@{pid=$proc.Id;cpuPercent=$cpuPercent;privateBytes=$proc.PrivateMemorySize64;workingBytes=$proc.WorkingSet64;handles=$proc.HandleCount;threads=$proc.Threads.Count;processWriteMBps=$writeMBps;processWriteBytes=$io.WriteBytes;ioValid=$ioValid}
                } catch {Note ('Process sample: '+$_.Exception.Message)} finally {$proc.Dispose()}
            }
            foreach($key in @($previous.Keys)){if(!$present.ContainsKey($key)){$previous.Remove($key)}}
            $drives=@(Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=3' | Select-Object DeviceID,FreeSpace,Size)
            @{time=$now.ToString('o');elapsedSeconds=($now-$start).TotalSeconds;processes=$rows;drives=$drives} | ConvertTo-Json -Depth 6 -Compress | Add-Content -LiteralPath (Join-Path $session ('samples-'+$now.ToString('yyyyMMdd')+'.jsonl')) -Encoding UTF8
        }
        if(($now-$lastSnapshot).TotalMinutes -ge 10){try {Snapshot} catch {Note $_.Exception.Message};$lastSnapshot=$now}
        if(Test-Path -LiteralPath (Join-Path $session 'stop.flag')){break}
        if(($now-$start).TotalDays -ge 30){Note '30-day diagnostic session limit reached';break}
        if($DurationSeconds -gt 0 -and ($now-$start).TotalSeconds -ge $DurationSeconds){break}
        $pressed=$false
        try {if(![Console]::IsInputRedirected -and [Console]::KeyAvailable){$pressed=[Console]::ReadKey($true).Key -eq 'Enter'}} catch {}
        if($pressed){break}
        Start-Sleep -Milliseconds 500
    } while($true)
} catch {Note $_.Exception.ToString();throw} finally {
    try {Snapshot} catch {Note $_.Exception.Message}
    @{ended=(Get-Date).ToString('o');elapsedSeconds=((Get-Date)-$start).TotalSeconds;note='录像不会被复制。进程 I/O 包含网络等，并非物理磁盘落盘速度。'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $session 'finished.json') -Encoding UTF8
    if($lock){$lock.Dispose()}
    Write-Host ('采样结束。请复制整个目录供分析：'+$session) -ForegroundColor Green
}
