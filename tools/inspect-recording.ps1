param([string[]]$Files)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$Files) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog=New-Object Windows.Forms.OpenFileDialog
    $dialog.Title='选择已停止录制的视频文件（可多选）';$dialog.Filter='录像文件|*.mp4;*.mkv;*.mov;*.avi|所有文件|*.*';$dialog.Multiselect=$true
    if($dialog.ShowDialog() -ne 'OK'){exit 0};$Files=$dialog.FileNames;$dialog.Dispose()
}
$out=Join-Path $root ('diagnostics\video-check-'+(Get-Date -Format 'yyyyMMdd_HHmmss')+'-'+$PID)
New-Item -ItemType Directory -Force -Path $out|Out-Null
function RunTool($tool,$arguments,$stdout,$stderr) {
    $si=New-Object Diagnostics.ProcessStartInfo
    $si.FileName=(Join-Path $root $tool)
    $si.Arguments=($arguments | ForEach-Object {'"'+([string]$_).Replace('"','\"')+'"'}) -join ' '
    $si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
    $proc=[Diagnostics.Process]::Start($si)
    $stream=[IO.File]::Create($stdout);$errors=[IO.File]::Create($stderr)
    try {
        $a=$proc.StandardOutput.BaseStream.CopyToAsync($stream);$b=$proc.StandardError.BaseStream.CopyToAsync($errors)
        $proc.WaitForExit();[void]$a.GetAwaiter().GetResult();[void]$b.GetAwaiter().GetResult();return $proc.ExitCode
    } finally {$stream.Dispose();$errors.Dispose();$proc.Dispose()}
}
$index=0
foreach($name in $Files) {
    $index++;$file=Get-Item -LiteralPath $name
    $dest=Join-Path $out ('file-'+$index);New-Item -ItemType Directory -Path $dest|Out-Null
    Write-Host ('扫描：'+$file.FullName+'（完整解码，建议停止录制后运行）')
    $before=$file.Length;$modified=$file.LastWriteTimeUtc
    $probe=RunTool 'ffprobe.exe' @('-v','error','-show_format','-show_streams','-of','json',$file.FullName) (Join-Path $dest 'probe.json') (Join-Path $dest 'probe-errors.log')
    $decode=$null;$black=@();$silence=@()
    if($probe -eq 0) {
        $meta=Get-Content -LiteralPath (Join-Path $dest 'probe.json') -Raw | ConvertFrom-Json
        $decodeArgs=@('-hide_banner','-nostdin','-nostats','-threads','2','-filter_threads','1','-i',$file.FullName)
        if(@($meta.streams|Where-Object codec_type -eq 'video').Count -gt 0){$decodeArgs+=@('-map','0:v:0','-vf','blackdetect=d=2:pix_th=0.10:pic_th=0.98')}
        if(@($meta.streams|Where-Object codec_type -eq 'audio').Count -gt 0){$decodeArgs+=@('-map','0:a:0','-af','silencedetect=n=-50dB:d=2')}
        $decodeArgs+=@('-f','null','NUL')
        $decode=RunTool 'ffmpeg.exe' $decodeArgs (Join-Path $dest 'decode-stdout.log') (Join-Path $dest 'decode.log')
        $black=@(Select-String -LiteralPath (Join-Path $dest 'decode.log') -Pattern 'black_start:'|ForEach-Object {$_.Line})
        $silence=@(Select-String -LiteralPath (Join-Path $dest 'decode.log') -Pattern 'silence_start:|silence_end:'|ForEach-Object {$_.Line})
    }
    $file.Refresh()
    @{file=$file.FullName;bytes=$before;changedDuringScan=($file.Length -ne $before -or $file.LastWriteTimeUtc -ne $modified);probeExit=$probe;decodeExit=$decode;blackIntervals=$black;silenceIntervals=$silence;
      thresholds='black >=2s, 98% pixels below 10%; silence below -50dB >=2s';note='区间为文件内相对秒数。黑色内容/静音也可能是正常节目，需与运行日志对照。未上传或修改录像。'} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $dest 'summary.json') -Encoding UTF8
}
Write-Host ('完成。请把整个报告目录一并提供：'+$out) -ForegroundColor Green
