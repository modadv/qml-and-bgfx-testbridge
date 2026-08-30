<#
.SYNOPSIS
    Launch testbridge_lab from a plain shell, without Visual Studio.

.DESCRIPTION
    Two things Visual Studio does for you that a bare `.\testbridge_lab.exe`
    does not:

      1. Qt on PATH. conan_toolchain.cmake sets CMAKE_VS_DEBUGGER_ENVIRONMENT,
         which prepends the conan Qt bin dir to PATH *for the VS debugger only*.
         Without it the loader fails on Qt5Core.dll / Qt5Quick.dll. This script
         sets PATH and QT_PLUGIN_PATH itself (see scripts\qt_env.ps1).
      2. The working directory. main.cpp loads
         QDir::currentPath() + "/qml/Main.qml", and bgfx resolves
         shaders/<renderer>/*.bin relative to cwd, so the app must run with its
         own directory as cwd -- not from the repo root.

    If the executable directory already contains Qt5Core.dll (see
    scripts\deploy.ps1) the Qt discovery is skipped: the app is already
    self-contained.

.EXAMPLE
    scripts\run.ps1
.EXAMPLE
    scripts\run.ps1 -Tier nocompute -Port 17777
.EXAMPLE
    scripts\run.ps1 -WhatIfEnv          # print the resolved env, launch nothing
#>
[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",

    # Defaults to .build-release\build for Release, .build\build otherwise.
    [string]$BuildDir = "",

    # Overrides Qt discovery; falls back to $env:TESTBRIDGE_QT_BIN.
    [string]$QtBin = "",

    [ValidateSet("full", "nocompute")]
    [string]$Tier = "",

    [int]$Port = 0,

    [switch]$WhatIfEnv,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$AppArgs
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

$exe = Join-Path $BuildDir "bin\$Config\testbridge_lab.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Executable not found: $exe`nBuild it first: scripts\build.ps1 -Config $Config"
}
$exeDir = Split-Path -Parent $exe

$deployed = Test-Path -LiteralPath (Join-Path $exeDir "Qt5Core.dll")
$qtLabel = "<self-contained, deployed next to exe>"
if (-not $deployed) {
    $qtLabel = Enable-QtRuntimeEnv -BuildDir $BuildDir -QtBin $QtBin
}

if ($Tier) { $env:TESTBRIDGE_RENDER_TIER = $Tier }
if ($Port -gt 0) { $env:TESTBRIDGE_PORT = "$Port" }

Write-Host "exe  : $exe"
Write-Host "cwd  : $exeDir"
Write-Host "qt   : $qtLabel"
if ($env:TESTBRIDGE_RENDER_TIER) { Write-Host "tier : $env:TESTBRIDGE_RENDER_TIER" }
if ($env:TESTBRIDGE_PORT) { Write-Host "port : $env:TESTBRIDGE_PORT" }

if ($WhatIfEnv) { return }

Push-Location -LiteralPath $exeDir
try {
    & $exe @AppArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
