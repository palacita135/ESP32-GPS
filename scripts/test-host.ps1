$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) {
    throw 'Install Visual Studio C++ Build Tools, or compile tests/host/test_speed.cpp with your host C++ compiler.'
}
$vsRoot = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) { throw 'Visual Studio C++ tools were not found.' }
$devShell = Join-Path $vsRoot 'Common7\Tools\Launch-VsDevShell.ps1'
& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force '.pio\host-tests' | Out-Null
    $gpsSource = '.pio\libdeps\cyd\TinyGPSPlus\src'
    if (!(Test-Path -LiteralPath $gpsSource)) { throw 'Run pio run first to install TinyGPSPlus.' }
    & cl.exe /nologo /EHsc /std:c++17 /W4 /DARDUINO=10819 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Itests\host "/I$gpsSource" tests\host\test_speed.cpp "$gpsSource\TinyGPS++.cpp" /Fe:.pio\host-tests\test_speed.exe /Fo:.pio\host-tests\
    if ($LASTEXITCODE -ne 0) { throw 'Host test compilation failed.' }
    & '.\.pio\host-tests\test_speed.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Host tests failed.' }
    & cl.exe /nologo /EHsc /std:c++17 /W4 /DARDUINO=10819 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Itests\track "/I$gpsSource" tests\track\test_track.cpp "$gpsSource\TinyGPS++.cpp" /Fe:.pio/host-tests/test_track.exe /Fo:.pio/host-tests/
    if ($LASTEXITCODE -ne 0) { throw 'Track test compilation failed.' }
    & '.\.pio\host-tests\test_track.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Track tests failed.' }
} finally {
    Pop-Location
}
