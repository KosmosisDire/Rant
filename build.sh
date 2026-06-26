#!/usr/bin/env bash
# Configure CMake and build all DART executables in Release.
# Outputs land in the repo root (Debug builds go in Debug/).
set -euo pipefail
cd "$(dirname "$0")"

cmake --preset linux
cmake --build --preset linux

echo
echo "Release build complete. Executables are in the repo root."
