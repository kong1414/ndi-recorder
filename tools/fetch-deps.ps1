# Download and lay out vendor/ dependencies with SHA256 checks; existing deps are skipped.
# Shared by local builds and CI (GitHub Actions windows runner). See tools/dependencies.json.
param([string]$Root = (Split-Path $PSScriptRoot -Parent))
$ErrorActionPreference = 'Stop'
$deps = Get-Content (Join-Path $Root 'tools/dependencies.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$vendor = Join-Path $Root 'vendor'
$dl = Join-Path $vendor '_download'
New-Item -ItemType Directory -Force $dl | Out-Null

function Get-Sha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '').ToLower() }
    finally { $fs.Dispose() }
}
function Assert-Sha256([string]$Path, [string]$Expect, [string]$What) {
    $actual = Get-Sha256 $Path
    if ($actual -ne $Expect.ToLower()) { throw "$What SHA256 mismatch: got $actual, expected $Expect. Archive contents changed; update the dependency explicitly per docs/BUILDING.md." }
}
function Get-Url([string]$Url, [string]$Dest) {
    Write-Host "download $Url"
    & curl.exe -fL --retry 3 -o $Dest $Url
    if ($LASTEXITCODE -ne 0) { throw "download failed: $Url" }
}

# FFmpeg: exact archive hosted in this repo's deps-vendor Release (upstream "latest" link rolls forward).
if (Test-Path (Join-Path $vendor 'ffmpeg/bin/avcodec-62.dll')) { Write-Host 'FFmpeg present, skip' } else {
    $zip = Join-Path $dl 'ffmpeg.zip'
    Get-Url $deps.ffmpeg.downloadUrl $zip
    Assert-Sha256 $zip $deps.ffmpeg.sha256 'FFmpeg'
    Expand-Archive -Force $zip $dl

    $inner = Get-ChildItem $dl -Directory | Where-Object Name -like 'ffmpeg-*' | Select-Object -First 1
    New-Item -ItemType Directory -Force (Join-Path $vendor 'ffmpeg') | Out-Null
    Copy-Item -Recurse -Force (Join-Path $inner.FullName 'bin'), (Join-Path $inner.FullName 'include'), (Join-Path $inner.FullName 'lib'), (Join-Path $inner.FullName 'LICENSE.txt') (Join-Path $vendor 'ffmpeg')
}

# SQLite amalgamation
if (Test-Path (Join-Path $vendor 'sqlite/sqlite3.c')) { Write-Host 'SQLite present, skip' } else {
    $zip = Join-Path $dl 'sqlite.zip'
    Get-Url $deps.sqlite.url $zip
    Assert-Sha256 $zip $deps.sqlite.sha256 'SQLite'
    Expand-Archive -Force $zip $dl
    $inner = Get-ChildItem $dl -Directory | Where-Object Name -like 'sqlite-amalgamation-*' | Select-Object -First 1
    New-Item -ItemType Directory -Force (Join-Path $vendor 'sqlite') | Out-Null
    Copy-Item -Force (Join-Path $inner.FullName 'sqlite3.c'), (Join-Path $inner.FullName 'sqlite3.h'), (Join-Path $inner.FullName 'sqlite3ext.h'), (Join-Path $inner.FullName 'shell.c') (Join-Path $vendor 'sqlite')
}

# Dear ImGui source tree
if (Test-Path (Join-Path $vendor 'imgui/imgui.cpp')) { Write-Host 'ImGui present, skip' } else {
    $zip = Join-Path $dl 'imgui.zip'
    Get-Url $deps.imgui.archiveUrl $zip
    Expand-Archive -Force $zip $dl

    $inner = Get-ChildItem $dl -Directory | Where-Object Name -like 'imgui-*' | Select-Object -First 1
    Copy-Item -Recurse -Force $inner.FullName (Join-Path $vendor 'imgui')
}

# nlohmann JSON single header and license
if (Test-Path (Join-Path $vendor 'json.hpp')) { Write-Host 'JSON present, skip' } else {
    $h = Join-Path $vendor 'json.hpp'
    Get-Url $deps.json.headerUrl $h
    Assert-Sha256 $h $deps.json.sha256 'json.hpp'
    $lic = Join-Path $vendor 'JSON-LICENSE.txt'
    Get-Url $deps.json.licenseUrl $lic
    Assert-Sha256 $lic $deps.json.licenseSha256 'JSON LICENSE'
}

# NDI SDK: unpack the official installer without installing (innounp is GPL, fetched from SourceForge, not committed).
if (Test-Path (Join-Path $vendor 'ndi/bin/Processing.NDI.Lib.x64.dll')) { Write-Host 'NDI present, skip' } else {
    $rar = Join-Path $dl 'innounp.rar'
    Get-Url $deps.innounp.url $rar
    Assert-Sha256 $rar $deps.innounp.sha256 'innounp'
    $sevenZip = (Get-Command 7z.exe -ErrorAction SilentlyContinue).Source
    if (-not $sevenZip) { $sevenZip = Join-Path $env:ProgramFiles '7-Zip/7z.exe' }
    & $sevenZip x -y "-o$dl\innounp" $rar | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'innounp extract failed (7-Zip 23+ required)' }
    $exe = Join-Path $dl 'ndi-sdk.exe'
    Get-Url $deps.ndi.installerUrl $exe
    Assert-Sha256 $exe $deps.ndi.installerSha256 'NDI SDK installer'
    & (Join-Path $dl 'innounp/innounp.exe') -x -y "-d$dl\ndi" $exe '{app}\Include\*' '{app}\Bin\x64\Processing.NDI.Lib.x64.dll' '{app}\NDI SDK License Agreement.pdf' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'NDI SDK unpack failed' }
    $app = Join-Path $dl 'ndi/{app}'
    New-Item -ItemType Directory -Force (Join-Path $vendor 'ndi/include'), (Join-Path $vendor 'ndi/bin') | Out-Null
    Copy-Item -Force (Join-Path $app 'Include/*.h') (Join-Path $vendor 'ndi/include')
    Copy-Item -Force (Join-Path $app 'Bin/x64/Processing.NDI.Lib.x64.dll') (Join-Path $vendor 'ndi/bin')
    Copy-Item -Force (Join-Path $app 'NDI SDK License Agreement.pdf') (Join-Path $vendor 'NDI-SDK-License.pdf')
    $dll = Join-Path $vendor 'ndi/bin/Processing.NDI.Lib.x64.dll'
    Assert-Sha256 $dll $deps.ndi.runtimeSha256 'NDI runtime'
    "NDI SDK $($deps.ndi.version) installer extracted by fetch-deps.ps1 at $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" | Out-File -Encoding utf8 (Join-Path $vendor 'ndi/bin/NDI-RUNTIME-VERSION.txt')
}

Remove-Item -Recurse -Force $dl -ErrorAction SilentlyContinue
Write-Host 'vendor dependencies ready'
