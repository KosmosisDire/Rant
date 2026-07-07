#!/usr/bin/env bash
# Build the DART native library into csharp/runtimes/<rid>/native/ from dist/dart.h.
# Linux -> linux-x64/libdart.so, macOS -> osx-x64/libdart.dylib. The .nupkg and the
# Unity package bundle it.
#   sh csharp/native/build.sh
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
dist="$here/../../dist"
cc="${CC:-cc}"
case "$(uname -s)" in
  Linux)  rid="linux-x64"; lib="libdart.so";    extra="-lrt" ;;
  Darwin) rid="osx-x64";   lib="libdart.dylib"; extra="" ;;
  *)      echo "unsupported OS: $(uname -s)"; exit 1 ;;
esac
out="$here/../runtimes/$rid/native"
mkdir -p "$out"
"$cc" -O2 -std=c99 -fPIC -shared -DDART_IMPLEMENTATION -I"$dist" "$here/dart_impl.c" -o "$out/$lib" $extra
echo "built $out/$lib"
