# Assemble the Unity package: copy the shared wrapper (csharp/Dart.cs) and the prebuilt
# native libs into Runtime/ (both are gitignored here -- this keeps ONE Dart.cs and no
# binaries in git). Build the native libs first (csharp/native/build.ps1 on Windows,
# build.sh on Linux). Afterwards, add csharp/unity/ to a Unity project via Package
# Manager -> "Add package from disk", or zip Runtime/ into a .unitypackage / .tgz.
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
Copy-Item (Join-Path $here "..\Dart.cs") (Join-Path $here "Runtime\Dart.cs") -Force
$plugins = Join-Path $here "Runtime\Plugins\x86_64"
New-Item -ItemType Directory -Force -Path $plugins | Out-Null
$win = Join-Path $here "..\runtimes\win-x64\native\dart.dll"
$lin = Join-Path $here "..\runtimes\linux-x64\native\libdart.so"
if (Test-Path $win) { Copy-Item $win (Join-Path $plugins "dart.dll")   -Force }
if (Test-Path $lin) { Copy-Item $lin (Join-Path $plugins "libdart.so") -Force }
Write-Host "assembled Unity package: Runtime/Dart.cs + Runtime/Plugins/x86_64/*"
