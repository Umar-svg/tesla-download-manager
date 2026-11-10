# Configure, build and test Tesla Download Manager with MSVC + Ninja.
# Loads the MSVC developer environment into this PowerShell session first.
param(
    [string]$Preset = "msvc-release",
    [switch]$NoTest
)
# "Continue": in Windows PowerShell 5.1, stderr from native tools (cmake, vcvars)
# becomes an error record, and "Stop" would abort on harmless warnings.
# Failures are detected through $LASTEXITCODE below.
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "MSVC C++ tools not found. Install VS 2022 Build Tools (VCTools workload)." }
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
    cmd.exe /d /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] }
    }
}
foreach ($p in "C:\Program Files\CMake\bin", "$env:LOCALAPPDATA\Microsoft\WinGet\Links") {
    if ((Test-Path $p) -and ($env:PATH -notlike "*$p*")) { $env:PATH = "$p;$env:PATH" }
}

Push-Location $root
try {
    cmake --preset $Preset
    if ($LASTEXITCODE) { throw "configure failed" }
    cmake --build --preset $Preset
    if ($LASTEXITCODE) { throw "build failed" }
    if (-not $NoTest) {
        ctest --preset $Preset
        if ($LASTEXITCODE) { throw "tests failed" }
    }
} finally {
    Pop-Location
}
