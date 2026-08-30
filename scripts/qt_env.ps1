<#
.SYNOPSIS
    Shared Qt runtime discovery for scripts\run.ps1 and scripts\deploy.ps1.
    Dot-source it; it defines functions and does nothing on its own.

.DESCRIPTION
    Nothing in the build copies Qt next to testbridge_lab.exe. Visual Studio
    gets away with it because conan_toolchain.cmake sets
    CMAKE_VS_DEBUGGER_ENVIRONMENT, which prepends the conan Qt bin dir to PATH
    for the VS debugger only. Outside VS that has to be done by hand, and the
    conan cache path contains a package hash that changes on re-install -- so
    resolve it from the generators rather than hardcoding it.
#>

function Resolve-QtBin {
    <#
    .SYNOPSIS
        Locate the Qt bin directory (the one holding Qt5Core.dll).
    .DESCRIPTION
        Resolution order: explicit -QtBin, $env:TESTBRIDGE_QT_BIN, then the
        conan generators under <BuildDir>\generators. Throws if nothing works.
    #>
    param(
        [string]$BuildDir,
        [string]$QtBin = ""
    )

    if (-not $QtBin) { $QtBin = $env:TESTBRIDGE_QT_BIN }

    if (-not $QtBin) {
        $generators = Join-Path $BuildDir "generators"
        $candidates = [System.Collections.Generic.List[string]]::new()

        if (Test-Path -LiteralPath $generators) {
            # conanrunenv-<config>-<arch>.bat: set "PATH=<dir>;<dir>;%PATH%"
            Get-ChildItem -LiteralPath $generators -Filter "conanrunenv-*.bat" -ErrorAction SilentlyContinue |
                ForEach-Object {
                    foreach ($line in Get-Content -LiteralPath $_.FullName) {
                        if ($line -match '^\s*set\s+"PATH=(.+?);%PATH%"\s*$') {
                            $matches[1] -split ';' | ForEach-Object { $candidates.Add($_) }
                        }
                    }
                }

            # conan_toolchain.cmake:
            #   set(CMAKE_VS_DEBUGGER_ENVIRONMENT "PATH=$<$<CONFIG:Release>:a;b>;%PATH%")
            $toolchain = Join-Path $generators "conan_toolchain.cmake"
            if (Test-Path -LiteralPath $toolchain) {
                foreach ($line in Get-Content -LiteralPath $toolchain) {
                    if ($line -match 'CMAKE_VS_DEBUGGER_ENVIRONMENT\s+"PATH=(.*)"') {
                        ($matches[1] -replace '\$<[^:>]+:', '' -replace '[<>$]', '') -split ';' |
                            ForEach-Object { $candidates.Add(($_ -replace '%PATH%', '').Trim()) }
                    }
                }
            }
        }

        foreach ($cand in $candidates) {
            if ($cand -and (Test-Path -LiteralPath (Join-Path $cand "Qt5Core.dll"))) { $QtBin = $cand; break }
        }
    }

    if (-not $QtBin) {
        throw @"
Could not locate the Qt bin directory.
Looked at: `$env:TESTBRIDGE_QT_BIN, then the conan generators under
$BuildDir\generators.
Pass it explicitly:  -QtBin '<path-to-qt>\bin'
"@
    }

    $QtBin = (Resolve-Path -LiteralPath $QtBin).Path
    if (-not (Test-Path -LiteralPath (Join-Path $QtBin "Qt5Core.dll"))) {
        throw "No Qt5Core.dll in '$QtBin' -- that is not a Qt bin directory."
    }
    return $QtBin
}

function Resolve-QtPlugins {
    <#
    .SYNOPSIS
        Locate the Qt plugins directory for a given Qt bin directory.
    .DESCRIPTION
        The conan qt package puts them under bin\archdatadir\plugins; an
        upstream Qt install has them at <prefix>\plugins instead.
    #>
    param([Parameter(Mandatory)][string]$QtBin)

    foreach ($p in @((Join-Path $QtBin "archdatadir\plugins"),
                     (Join-Path (Split-Path -Parent $QtBin) "plugins"))) {
        if (Test-Path -LiteralPath (Join-Path $p "platforms\qwindows.dll")) { return $p }
    }
    throw "qwindows.dll not found under '$QtBin'. The Qt install looks incomplete."
}

function Enable-QtRuntimeEnv {
    <#
    .SYNOPSIS
        Set PATH / QT_PLUGIN_PATH / QML2_IMPORT_PATH in the current process so
        a Qt app (or windeployqt itself) can start. Returns the Qt bin dir.
    #>
    param(
        [Parameter(Mandatory)][string]$BuildDir,
        [string]$QtBin = ""
    )

    $QtBin  = Resolve-QtBin -BuildDir $BuildDir -QtBin $QtBin
    $plugins = Resolve-QtPlugins -QtBin $QtBin
    $qml     = Join-Path (Split-Path -Parent $plugins) "qml"

    $env:PATH = "$QtBin;$env:PATH"
    $env:QT_PLUGIN_PATH = $plugins
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $plugins "platforms"
    if (Test-Path -LiteralPath $qml) { $env:QML2_IMPORT_PATH = $qml }

    return $QtBin
}
