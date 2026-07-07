#!/usr/bin/env bash
# Assemble the Unity package (see pack.ps1). Copies csharp/Dart.cs + prebuilt native libs
# into Runtime/ (both gitignored). Build the native libs first (csharp/native/build.sh).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
cp "$here/../Dart.cs" "$here/Runtime/Dart.cs"
plugins="$here/Runtime/Plugins/x86_64"
mkdir -p "$plugins"
[ -f "$here/../runtimes/win-x64/native/dart.dll" ]    && cp "$here/../runtimes/win-x64/native/dart.dll"    "$plugins/dart.dll"
[ -f "$here/../runtimes/linux-x64/native/libdart.so" ] && cp "$here/../runtimes/linux-x64/native/libdart.so" "$plugins/libdart.so"
echo "assembled Unity package: Runtime/Dart.cs + Runtime/Plugins/x86_64/*"
