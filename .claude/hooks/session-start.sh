#!/bin/bash
# Install build deps and configure/build in cloud sessions only.
set -euo pipefail
[ "${CLAUDE_CODE_REMOTE:-}" = "true" ] || exit 0
cd "$CLAUDE_PROJECT_DIR"
if ! dpkg -s libgtest-dev libbenchmark-dev ninja-build clang libclang-rt-18-dev zlib1g-dev >/dev/null 2>&1; then
  apt-get update -q && apt-get install -y -q ninja-build libgtest-dev libbenchmark-dev clang libclang-rt-18-dev zlib1g-dev
fi
[ -f build/build.ninja ] || cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build >/dev/null
