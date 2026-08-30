<#
.SYNOPSIS
    Produce a self-contained testbridge_lab folder that runs anywhere -- no
    Visual Studio, no conan cache, no environment variables.

.DESCRIPTION
    Copies the executable and its runtime data (qml\, shaders\, assets\) into
    $DestDir, then runs Qt's windeployqt there to pull in the Qt DLLs, the
    qwindows platform plugin, and the QtQuick QML modules that qml\Main.qml
    imports. With -CompilerRuntime the MSVC redistributable DLLs are copied in
    too, so the folder also works on a machine with no VC++ runtime installed.

    Only Qt is deployed because it is the only shared dependency: spdlog and
    fmt are static in this build (their conan packages ship no bin\ dir), and
    bgfx is linked in.

    scripts\run.ps1 detects the result -- if Qt5Core.dll sits next to the exe it
    skips all environment setup.

.EXAMPLE
    scripts\deploy.ps1
.EXAMPLE
    scripts\deploy.ps1 -DestDir D:\share\testbridge_lab -Clean -CompilerRuntime
#>
[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",

    # Defaults to .build-release\build for Release, .build\build otherwise.
    [string]$BuildDir = "",

    # Defaults to <repo>\dist\testbridge_lab.
    [string]$DestDir = "",

    # Overrides Qt discovery; falls back to $env:TESTBRIDGE_QT_BIN.
    [string]$QtBin = "",

    # Also copy the MSVC redistributable DLLs (for machines without VC++).
    [switch]$CompilerRuntime,

    # Wipe DestDir first.
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "qt_env.ps1")

$root = Split-Path -Parent $PSScriptRoot

if (-not $BuildDir) {
    $BuildDir = if ($Config -eq "Release") { "$root\.build-release\build" } else { "$root\.build\build" }
}
elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $root $BuildDir
}
if (-not $DestDir) { $DestDir = Join-Path $root "dist\testbridge_lab" }
elseif (-not [System.IO.Path]::IsPathRooted($DestDir)) { $DestDir = Join-Path $root $DestDir }

$exe = Join-Path $BuildDir "bin\$Config\testbridge_lab.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Executable not found: $exe`nBuild it first: scripts\build.ps1 -Config $Config"
}
$exeDir = Split-Path -Parent $exe

# windeployqt is a Qt app itself, so it needs Qt on PATH before it can run.
$QtBin = Enable-QtRuntimeEnv -BuildDir $BuildDir -QtBin $QtBin
$windeployqt = Join-Path $QtBin "windeployqt.exe"
if (-not (Test-Path -LiteralPath $windeployqt)) {
    throw "windeployqt.exe not found in '$QtBin'."
}

if ($Clean -and (Test-Path -LiteralPath $DestDir)) {
    Write-Host "cleaning $DestDir"
    Remove-Item -LiteralPath $DestDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $DestDir | Out-Null

Write-Host "deploying $Config -> $DestDir"
Copy-Item -LiteralPath $exe -Destination $DestDir -Force

# qml\ and shaders\ are resolved relative to cwd at runtime; assets\ is
# referenced by qml\Main.qml. All three are already staged next to the exe by
# the build's post-build copy step.
foreach ($dir in @("qml", "shaders", "assets")) {
    $src = Join-Path $exeDir $dir
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination $DestDir -Recurse -Force
    }
    else {
        Write-Warning "missing runtime dir, skipped: $src"
    }
}

$deployArgs = @(
    $(if ($Config -eq "Debug") { "--debug" } else { "--release" })
    "--qmldir"; (Join-Path $DestDir "qml")
    "--no-translations"
    $(if ($CompilerRuntime) { "--compiler-runtime" } else { "--no-compiler-runtime" })
    (Join-Path $DestDir "testbridge_lab.exe")
)

& $windeployqt @deployArgs
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }

foreach ($required in @("Qt5Core.dll", "Qt5Quick.dll", "platforms\qwindows.dll")) {
    if (-not (Test-Path -LiteralPath (Join-Path $DestDir $required))) {
        throw "deployment incomplete: $required is missing from $DestDir"
    }
}

$size = [math]::Round(((Get-ChildItem -LiteralPath $DestDir -Recurse -File |
    Measure-Object -Property Length -Sum).Sum / 1MB), 1)
Write-Host ""
Write-Host "deployed to $DestDir ($size MB)"
Write-Host "run it with:  cd '$DestDir'; .\testbridge_lab.exe"
