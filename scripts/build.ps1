param(
    [string]$Config = "Release",
    [string]$Target = "testbridge_lab"
)

cmake --build .build-release\build --config $Config --target $Target -- /m
