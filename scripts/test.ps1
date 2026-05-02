param(
    [string]$Config = "Release",
    [string]$Label = ""
)

$args = @("--test-dir", ".build-release\build", "-C", $Config, "--output-on-failure")
if ($Label) {
    $args += @("-L", $Label)
}
ctest @args
