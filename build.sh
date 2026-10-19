#!/usr/bin/env bash
# Composix build. Deps (Ubuntu/Debian): libgtkmm-4.0-dev libspdlog-dev liblcms2-dev
#   libharfbuzz-dev zlib1g-dev cmake g++ zip (s180: lcms2 was missing from this list).
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
cmake -B build
cmake --build build -- -j"$(nproc)"
echo ""
echo "Built: ./build/composix  and  ./build/composix_selftest"
echo "Smoke: ./build/composix_selftest"
