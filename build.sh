#!/usr/bin/env bash
# Configure CMake and build all DART targets. Output lands in bin/ for every
# config, so a Release build overwrites a Debug one (and vice versa).
# Usage: build.sh [release|debug]   (default: release)
set -euo pipefail
cd "$(dirname "$0")"

config="${1:-release}"
case "$config" in
  release) build_preset=linux ;;
  debug)   build_preset=linux-debug ;;
  *) echo "Unknown argument '$config'. Usage: $0 [release|debug]" >&2; exit 2 ;;
esac

cmake --preset linux -D DART_BUILD_JS_CLIENT=ON
cmake --build --preset "$build_preset"

echo
echo "$config build complete. Executables are in bin/."
