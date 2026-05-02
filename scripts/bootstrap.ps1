param(
    [string]$QtPrefix = $env:QT_PREFIX
)

if (-not $QtPrefix) {
    throw "Qt prefix not configured. Set QT_PREFIX or pass -QtPrefix <path-to-qt>."
}

git submodule update --init --recursive
$env:QT_PREFIX = $QtPrefix
cmake -S . -B .build-release\build -DCMAKE_PREFIX_PATH="$QtPrefix"
