#!/usr/bin/env bash
# Composix build. Deps (Ubuntu/Debian): libgtkmm-4.0-dev libspdlog-dev liblcms2-dev
#   libharfbuzz-dev zlib1g-dev cmake g++ zip (s180: lcms2 was missing from this list).
# Optional (s187): libtiff-dev (Fedora: libtiff-devel) -- CMYK TIFFs print as CMYK.
# Optional (s194), for faster builds: ccache ninja-build mold (or lld).
# Optional (s213): clipper2-devel (Fedora) -- else the copy in third_party/clipper2 builds.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
# s194: Ninja when it's installed (a fresh build dir only -- an old Makefile
# build keeps its generator). ccache / mold / lld: CMakeLists.txt finds them.
gen=()
if [ ! -f build/CMakeCache.txt ] && command -v ninja >/dev/null 2>&1; then gen=(-G Ninja); fi
cmake -B build "${gen[@]}"
cmake --build build --parallel "$(nproc)"
echo ""
echo "Built: ./build/composix  and  ./build/composix_selftest"
echo "Smoke: ./build/composix_selftest"
