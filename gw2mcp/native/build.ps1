# Builds gw2dat_cli.exe with MinGW-w64 g++ (C++20). No CMake required.
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here "gw2dat_cli.exe"

$srcs = @(
    (Join-Path $here "main.cpp"),
    (Join-Path $here "src\gw2dat.cpp"),
    (Join-Path $here "src\BinaryParser.cpp")
)

$args = @(
    "-std=c++20", "-O2", "-s",
    "-static", "-static-libgcc", "-static-libstdc++",
    "-I", (Join-Path $here "include"),
    "-I", (Join-Path $here "third_party"),
    "-I", (Join-Path $here "third_party\stb")
) + $srcs + @("-o", $out)

Write-Host "g++ $($args -join ' ')"
& g++ @args
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "OK -> $out"
