param(
    [string]$Config = "Release",
    [string]$Label = "",
    [string]$QtBin = ""
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "qt_env.ps1")

$root = Split-Path -Parent $PSScriptRoot
$buildDir = if ($Config -eq "Release") { "$root\.build-release\build" } else { "$root\.build\build" }

# The app-launching tests read TESTBRIDGE_QT_BIN and prepend it to the child
# process PATH themselves (tests\test_smoke_testbridge_lab.py). Nothing in
# CMakeLists bakes it into the test ENVIRONMENT, so without it the app dies at
# startup with exit code 0xC0000135 (STATUS_DLL_NOT_FOUND) and ctest only
# reports "TestBridge app exited before port N opened".
if (-not $env:TESTBRIDGE_QT_BIN) {
    try {
        $env:TESTBRIDGE_QT_BIN = Resolve-QtBin -BuildDir $buildDir -QtBin $QtBin
    }
    catch {
        Write-Warning "Qt bin not resolved; app-launching tests will fail: $_"
    }
}

$ctestArgs = @("--test-dir", $buildDir, "-C", $Config, "--output-on-failure")
if ($Label) {
    $ctestArgs += @("-L", $Label)
}
ctest @ctestArgs
exit $LASTEXITCODE
