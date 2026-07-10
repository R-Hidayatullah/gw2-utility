# Builds gw2gsviewer.exe (Direct3D 11, game-shader renderer) with MinGW-w64 g++.
$ErrorActionPreference = "Stop"
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$native = Join-Path (Split-Path $here -Parent) "native"
$out    = Join-Path $here "gw2gsviewer.exe"

$args = @(
    "-std=c++20", "-O2",
    "-I", (Join-Path $native "include"),
    "-I", (Join-Path $native "third_party"),
    "-I", (Join-Path $native "third_party\stb"),
    "-I", $native,                              # gw2model.hpp
    (Join-Path $here "gw2gsviewer.cpp"),
    (Join-Path $native "src\gw2dat.cpp"),
    (Join-Path $native "src\BinaryParser.cpp"),
    "-o", $out,
    "-ld3d11", "-ldxgi", "-ld3dcompiler", "-ldxguid",
    "-lgdi32", "-luser32", "-lole32",
    "-static", "-static-libgcc", "-static-libstdc++"
)
Write-Host "g++ $($args -join ' ')"
& g++ @args
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "OK -> $out"
