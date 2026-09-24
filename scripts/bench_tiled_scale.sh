#!/usr/bin/env bash
# bench_tiled_scale.sh — v0.4 §5.3 scale smoke + bench (Session 59).
#
# Runs Sponza at --target-size 896 (the largest Y_UP size fitting MC's
# 384-block height: Sponza's Y/longest ratio is 108/256 -> Y = 378) through
# the monolithic and tiled pipelines, recording wall-clock and peak RSS
# for each, then proves the AUTO trigger: with --max-grid-cells lowered
# below the frame size, --tiling off must FAIL (exit 4, pre-v0.4
# behaviour) while default AUTO must SUCCEED by tiling.
#
# Usage: scripts/bench_tiled_scale.sh <objtomc-binary> <sponza.obj> <out-dir>
# Results go to stdout; evidence is recorded in TESTS.md by the session
# that runs it.
set -u

BIN="$1"; SPONZA="$2"; OUT="$3"
mkdir -p "$OUT"

# Debug+ASan binaries need the same CUDA-under-ASan accommodation the
# ctest harness sets (tests/CMakeLists.txt Session 40 note); harmless for
# Release binaries. Prefer a Release build for honest numbers.
export ASAN_OPTIONS=protect_shadow_gap=0

run() {
    local label="$1"; shift
    local world="$1"; shift
    rm -rf "$world"
    echo "=== $label"
    local tf; tf=$(mktemp)
    /usr/bin/time -v -o "$tf" "$BIN" "$@" "$SPONZA" "$world" > /dev/null 2>&1
    local rc=$?
    echo "    exit=$rc"
    grep -E "Elapsed \(wall clock\)|Maximum resident set size" "$tf" |
        sed 's/^\s*/    /'
    rm -f "$tf"
    echo "    regions: $(ls "$world/region" 2>/dev/null | sort | tr '\n' ' ')"
}

run "monolithic (--tiling off)" "$OUT/w_mono" \
    --target-size 896 --tiling off
run "tiled (--tiling on)"       "$OUT/w_tiled" \
    --target-size 896 --tiling on

echo "=== payload equality (timestamp table at 4096-8191 excluded)"
ok=1
for f in "$OUT"/w_mono/region/*.mca; do
    b=$(basename "$f")
    if ! cmp -s <(tail -c +8193 "$f") <(tail -c +8193 "$OUT/w_tiled/region/$b") \
       || ! cmp -s <(head -c 4096 "$f") <(head -c 4096 "$OUT/w_tiled/region/$b"); then
        echo "    DIFFER: $b"; ok=0
    fi
done
[ "$ok" = 1 ] && echo "    all region payloads identical"

echo "=== AUTO trigger proof (frame 896x378x553 = 187M cells; cap lowered to 100M)"
rm -rf "$OUT/w_off_capped" "$OUT/w_auto_capped"
"$BIN" --target-size 896 --tiling off --max-grid-cells 100000000 \
    "$SPONZA" "$OUT/w_off_capped" > /dev/null 2>&1
echo "    --tiling off  exit=$? (expect 4: voxel_error oversize)"
"$BIN" --target-size 896 --max-grid-cells 100000000 \
    "$SPONZA" "$OUT/w_auto_capped" > /dev/null 2>&1
echo "    AUTO (default) exit=$? (expect 0: tiles under the per-tile cap)"
echo "    AUTO regions: $(ls "$OUT/w_auto_capped/region" 2>/dev/null | sort | tr '\n' ' ')"
