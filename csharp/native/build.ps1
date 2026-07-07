# Build the DART native library for Windows x64 into csharp/runtimes/win-x64/native/dart.dll.
# The .nupkg (dotnet pack) and the Unity package bundle it. Run after the C amalgamation
# exists in dist/. Requires clang-cl (finds MSVC automatically) or MinGW gcc.
#   powershell -File csharp/native/build.ps1
param([string]$Out = "")
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$dist = Join-Path $here "..\..\dist"
if ($Out -eq "") { $Out = Join-Path $here "..\runtimes\win-x64\native" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$outDll = Join-Path $Out "dart.dll"
$impl = Join-Path $here "dart_impl.c"
$def = Join-Path $here "dart.def"

$clangcl = Get-Command clang-cl -ErrorAction SilentlyContinue
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if ($clangcl) {
    & $clangcl.Source /nologo /O2 /LD /TC /DDART_IMPLEMENTATION /D_CRT_SECURE_NO_WARNINGS "/I$dist" `
        $impl "/Fe:$outDll" /link "/DEF:$def" ws2_32.lib bcrypt.lib winmm.lib
    # clang-cl / MSVC also emit dart.lib + dart.exp next to the dll; drop the byproducts
    Remove-Item (Join-Path $Out "dart.lib"), (Join-Path $Out "dart.exp") -ErrorAction SilentlyContinue
}
elseif ($gcc) {
    & $gcc.Source -O2 -std=c99 -shared -DDART_IMPLEMENTATION "-I$dist" $impl -o $outDll `
        -lws2_32 -lbcrypt -lwinmm -static-libgcc
}
else {
    throw "no clang-cl or gcc on PATH (install LLVM or MinGW)"
}
Write-Host "built $outDll"
