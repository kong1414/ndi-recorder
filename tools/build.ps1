$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
cmake -S $projectRoot -B "$projectRoot/build" -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE -ne 0){throw 'CMake configuration failed'}
cmake --build "$projectRoot/build" --config Release --parallel 6
if($LASTEXITCODE -ne 0){throw 'Build failed'}
ctest --test-dir "$projectRoot/build" -C Release --output-on-failure
if($LASTEXITCODE -ne 0){throw 'Tests failed'}

