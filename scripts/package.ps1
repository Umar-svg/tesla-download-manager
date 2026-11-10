# Builds the Windows installer: Release build -> install tree -> dependency
# check -> Inno Setup. Output: build\msvc-release\installer\TeslaDM-<ver>-setup.exe
param([switch]$SkipBuild)
$ErrorActionPreference = "Continue"   # native stderr is not an error (see build.ps1)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $SkipBuild) {
    & "$PSScriptRoot\build.ps1" -Preset msvc-release -NoTest
    if ($LASTEXITCODE) { throw "build failed" }
}
if (-not (Get-Command dumpbin.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    cmd.exe /d /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] }
    }
}
$env:PATH = "C:\Program Files\CMake\bin;$env:PATH"

$stage = (Join-Path $root "build\msvc-release\stage") -replace '\\', '/'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
cmake --install build/msvc-release --prefix $stage
if ($LASTEXITCODE) { throw "cmake --install failed" }

python -I installer\check_deps.py $stage
if ($LASTEXITCODE) { throw "dependency check failed: the install would not run on a clean PC" }

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 not found (winget install --id JRSoftware.InnoSetup -e)" }
& $iscc /Q build\msvc-release\tesla-dm.iss
if ($LASTEXITCODE) { throw "ISCC failed" }
Get-ChildItem build\msvc-release\installer\*.exe | ForEach-Object { "{0}  {1:N1} MB" -f $_.FullName, ($_.Length / 1MB) }
