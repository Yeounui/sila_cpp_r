#!/usr/bin/env bash
set -euo pipefail

# Runs cmake configure + build + ctest for each sanitizer preset.
# Usage: tests/sanitize.sh [preset ...]
# With no arguments, runs: asan tsan clang

PRESETS=("$@")
(( $# )) || PRESETS=(asan tsan clang)

JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"

for preset in "${PRESETS[@]}"; do
    echo "=== $preset ==="

    if [[ "$preset" == "tsan" ]]; then
        # tsan uses clang (GCC TSan crashes on WSL2 ASLR).
        # cmake --preset fails when build/tsan already exists (CMake 4.4
        # tries to read CMakePresets.json from the binaryDir), so configure
        # manually.
        rm -rf "$SRCDIR/build/tsan"
        cmake -S "$SRCDIR" -B "$SRCDIR/build/tsan" \
            -DCMAKE_TOOLCHAIN_FILE="$SRCDIR/third_party/vcpkg/scripts/buildsystems/vcpkg.cmake" \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_CXX_COMPILER=clang++ \
            -DCMAKE_NO_SYSTEM_FROM_IMPORTED:BOOL=ON \
            -DVCPKG_OVERLAY_PORTS="$SRCDIR/vcpkg-overlays/ports" \
            -DVCPKG_OVERLAY_TRIPLETS="$SRCDIR/vcpkg-overlays/triplets" \
            -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -I$SRCDIR/build/tsan/vcpkg_installed/x64-linux/include" \
            -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
            -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
    else
        cmake --preset "$preset"
    fi

    cmake --build "$SRCDIR/build/$preset" -j "$JOBS"

    if [[ "$preset" == "tsan" ]]; then
        TSAN_OPTIONS="suppressions=$SRCDIR/tests/tsan_suppressions.txt:halt_on_error=0" \
            ctest --test-dir "$SRCDIR/build/$preset" --output-on-failure
    else
        ctest --test-dir "$SRCDIR/build/$preset" --output-on-failure
    fi
    echo ""
done

echo "All presets passed."
