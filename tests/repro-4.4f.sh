#!/usr/bin/env bash
# audit 4.4f reproduction loop.
#
# The finding is an exit-time crash in OpenSSL's per-thread cleanup (resolved by BoringSSL swap), racing
# between two gRPC event-engine workers. Base rate against OpenSSL 3.6.3 was formerly
# 2 aborts in 84 iterations (~1 in 42), so a single green ctest proves
# nothing -- only a long loop does. It presents as either "double free or
# corruption (fasttop)" (abort) or a SEGFAULT, and always AFTER the test
# itself reports PASSED, since the crash is in teardown.
#
# Usage: repro-4.4f.sh <iterations> [build-dir]
set -u
ITERS="${1:-100}"
BUILD="${2:-build/gcc}"
BIN="$BUILD/tests/test_sila"
FILTER='SiLAServerBaseRun.StartsListeningAndServesUnaryRpc'

ulimit -c unlimited

fail=0
for i in $(seq 1 "$ITERS"); do
    out=$("$BIN" --gtest_filter="$FILTER" 2>&1)
    rc=$?
    if [ $rc -ne 0 ]; then
        fail=$((fail + 1))
        echo "=== iteration $i: exit $rc ==="
        echo "$out" | tail -5
    fi
done

echo "RESULT: $fail failure(s) in $ITERS iterations"
[ "$fail" -eq 0 ]
