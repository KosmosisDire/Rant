#!/usr/bin/env bash
# Build the native library into csharp/runtimes/<rid>/native/ from dist/dart.h: Linux
# DART_BUILD_SHARED marks every public entry point visible, and -fvisibility=hidden
# keeps the internals in. What the library exports is what the public headers declare.
# gives linux-x64/libdart.so, macOS osx-x64/libdart.dylib. The packages bundle it.
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
"$cc" -O2 -std=c99 -fPIC -shared -DDART_IMPLEMENTATION -DDART_BUILD_SHARED \n  -fvisibility=hidden -I"$dist" "$here/dart_impl.c" -o "$out/$lib" $extra
echo "built $out/$lib"
