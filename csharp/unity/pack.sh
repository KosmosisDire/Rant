#!/usr/bin/env bash
# Assemble the Unity package (see pack.ps1). Copies csharp/Dart.cs + prebuilt native libs
# into Runtime/ (both gitignored). Build the native libs first (the dart_shared target).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
cp "$here/../Dart.cs" "$here/Runtime/Dart.cs"
plugins="$here/Runtime/Plugins/x86_64"
mkdir -p "$plugins"
[ -f "$here/../../dist/native/win-x64/dart.dll" ]    && cp "$here/../../dist/native/win-x64/dart.dll"    "$plugins/dart.dll"
[ -f "$here/../../dist/native/linux-x64/libdart.so" ] && cp "$here/../../dist/native/linux-x64/libdart.so" "$plugins/libdart.so"
echo "assembled Unity package: Runtime/Dart.cs + Runtime/Plugins/x86_64/*"
