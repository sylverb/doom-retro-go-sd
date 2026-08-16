#!/usr/bin/env bash
#
# oplverify — byte-compare the OPL core's output between a reference commit and
# the working tree. Exits non-zero if they differ.
#
#   ./tools/oplverify/run.sh [REF_COMMIT]
#
# REF_COMMIT defaults to 01bbc281 (the hand-hoisted OPL baseline). The
# reference sources are extracted from git, so the working tree is never
# disturbed.
#
# The two builds MUST see identical EMU8950_* configuration except for the
# change under test, or the comparison is meaningless. The reference is built
# with the shipping config; the candidate adds whatever the working tree needs
# (see CAND_DEFS below).
#
set -euo pipefail

cd "$(dirname "$0")/../.."
ROOT=$PWD
REF=${1:-01bbc281}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

CC=${CC:-gcc}

# Config shared by both builds — mirrors the device build's OPL defines.
# -fms-extensions: OPL_SLOT embeds `struct SLOT_RENDER` anonymously (emu8950.h:41).
# __not_in_flash_func is a PICO_BUILD-ism; on the host it is identity.
COMMON_DEFS=(
  -O2 -w -fms-extensions
  -D'__not_in_flash_func(x)=x'
  -DUSE_EMU8950_OPL=1
  -DEMU8950_LINEAR=1
  -DEMU8950_LINEAR_SKIP=1
  -DEMU8950_NO_TLL=1
  -DEMU8950_NO_TIMER=1
  -DEMU8950_NO_TEST_FLAG=1
  -DEMU8950_NO_PERCUSSION_MODE=1
  -DEMU8950_NO_RATECONV=1
)

# Extra defines the CANDIDATE build needs. Override from the environment when
# testing a configuration change, e.g.
#   CAND_DEFS="-DEMU8950_SLOT_RENDER=1 -DEMU8950_NO_WAVE_TABLE_MAP=1 -DSLOT_RENDER_PORTABLE=1"
CAND_DEFS=${CAND_DEFS:-}

build() {                       # build <srcdir> <outbin> <extra-defs...>
  local src=$1 out=$2; shift 2
  local objs=("$src/emu8950.c")
  # slot_render.cpp only participates when EMU8950_SLOT_RENDER is on, and it is
  # C++, so it needs a separate compiler invocation.
  if [[ " $* " == *"EMU8950_SLOT_RENDER=1"* ]]; then
    g++ -c "${COMMON_DEFS[@]}" "$@" -I"$src" -o "$WORK/slot_render.o" "$src/slot_render.cpp"
    objs+=("$WORK/slot_render.o")
  fi
  $CC "${COMMON_DEFS[@]}" "$@" -I"$src" \
      -o "$out" "$ROOT/tools/oplverify/harness.c" "${objs[@]}" -lm -lstdc++
}

echo "== reference: $REF =="
mkdir -p "$WORK/ref"
git -C rp2040-doom archive "$REF" opl | tar -x -C "$WORK/ref"
build "$WORK/ref/opl" "$WORK/ref_harness"
"$WORK/ref_harness" "$WORK/ref.bin"

echo "== candidate: working tree =="
build "$ROOT/rp2040-doom/opl" "$WORK/new_harness" ${CAND_DEFS}
"$WORK/new_harness" "$WORK/new.bin"

echo "== compare =="
if cmp -s "$WORK/ref.bin" "$WORK/new.bin"; then
  echo "BIT-EXACT: outputs are byte-identical ($(stat -c%s "$WORK/ref.bin") bytes)"
  exit 0
fi

# Characterise the difference rather than just failing — "how wrong" decides
# whether a deviation is an acceptable near-silence artefact or a real bug.
python3 - "$WORK/ref.bin" "$WORK/new.bin" <<'PY'
import struct, sys
a = open(sys.argv[1],'rb').read()
b = open(sys.argv[2],'rb').read()
if len(a) != len(b):
    print(f"DIFFER: lengths {len(a)} vs {len(b)}"); sys.exit(1)
n = len(a)//4
A = struct.unpack(f"<{n}i", a); B = struct.unpack(f"<{n}i", b)
diff = [(i, x, y) for i,(x,y) in enumerate(zip(A,B)) if x != y]
mx = max((abs(x-y) for _,x,y in diff), default=0)
print(f"DIFFER: {len(diff)}/{n} samples ({100.0*len(diff)/n:.2f}%), max delta {mx}")
if diff:
    i,x,y = diff[0]
    print(f"  first at sample {i} (buffer {i//256}, offset {i%256}): ref={x} new={y}")
    big = [d for d in diff if abs(d[1]-d[2]) > 2]
    print(f"  samples differing by more than +/-2: {len(big)}")
PY
exit 1
