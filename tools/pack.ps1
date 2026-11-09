param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildRoot = Join-Path $projectRoot 'build'
$target = Join-Path $projectRoot 'dist/ndi-recorder-windows-x64'
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Build or tests failed' }
}
if (!(Test-Path (Join-Path $buildRoot 'Release/ndi-recorder.exe'))) { throw 'Build first' }
cmake --install $buildRoot --config Release --prefix $target
if ($LASTEXITCODE -ne 0) { throw 'Install staging failed' }
cpack --config (Join-Path $buildRoot 'CPackConfig.cmake') -C Release
if ($LASTEXITCODE -ne 0) { throw 'ZIP packaging failed' }
Get-FileHash (Join-Path $target 'ndi-recorder.exe') | Format-List
Write-Output "Ready: $target/ndi-recorder.exe"
Write-Output "Versioned ZIP: $projectRoot/dist/ndi-recorder-*-windows-x64.zip"
