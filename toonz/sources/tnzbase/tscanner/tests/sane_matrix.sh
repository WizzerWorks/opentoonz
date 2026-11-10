#!/bin/bash
# SANE test matrix for the OpenToonz SANE scanner driver (no hardware needed).
#
# Runs scanimage against SANE's "test" backend using a private config
# directory (nothing in /etc is touched) and checks every image format and
# feeder branch that TScannerSane::acquire() has to handle. A failure here
# means the backend/environment misbehaves; a pass gives the reference
# output to compare OpenToonz results against (see doc/scanner_sane.md).
#
# Usage: sane_matrix.sh [output-dir]      (default: a fresh temp dir)

set -u
OUT=${1:-$(mktemp -d)}
CFG=$OUT/cfg
mkdir -p "$CFG"
printf 'test\n' > "$CFG/dll.conf"
for f in /etc/sane.d/test.conf; do [ -f "$f" ] && cp "$f" "$CFG/"; done
export SANE_CONFIG_DIR=$CFG

command -v scanimage >/dev/null || { echo "scanimage not found (install sane-utils)"; exit 2; }
scanimage -L 2>/dev/null | grep -q "test:0" ||
  { echo "SANE test backend not available (install libsane / sane-backends)"; exit 2; }

pass=0; fail=0
# near "W H" "W H": each within 3 px (the backend quantises the window)
near() {
  set -- $1 $2
  [ $(( $1 > $3 ? $1 - $3 : $3 - $1 )) -le 3 ] && [ $(( $2 > $4 ? $2 - $4 : $4 - $2 )) -le 3 ]
}
# check NAME EXPECTED_MAGIC EXPECTED_SIZE_OR_- scanimage-args...
check() {
  local name=$1 magic=$2 size=$3; shift 3
  local f=$OUT/$name.pnm
  if scanimage -d test "$@" -o "$f" 2>"$OUT/$name.err"; then
    local m; m=$(head -c2 "$f")
    local dims; dims=$(sed -n '2,3p' "$f" | grep -v '^#' | head -1)
    if [ "$m" = "$magic" ] && { [ "$size" = - ] || near "$dims" "$size"; }; then
      echo "PASS  $name ($m $dims)"; pass=$((pass+1)); return
    fi
    echo "FAIL  $name: got '$m' '$dims', want '$magic' '$size'"
  else
    echo "FAIL  $name: scanimage exited $? ($(head -1 "$OUT/$name.err"))"
  fi
  fail=$((fail+1))
}

# Mode / depth / test-picture matrix. P4=1-bit, P5=gray, P6=colour.
for pic in "Solid black" "Solid white" "Color pattern" "Grid"; do
  tag=${pic// /_}
  check "gray8_$tag"    P5 - --mode Gray  --depth 8  --test-picture "$pic" --resolution 75
  check "color8_$tag"   P6 - --mode Color --depth 8  --test-picture "$pic" --resolution 75
  check "lineart_$tag"  P4 - --mode Gray  --depth 1  --test-picture "$pic" --resolution 75
done
check gray16  P5 - --mode Gray  --depth 16 --resolution 75   # OpenToonz down-converts
check color16 P6 - --mode Color --depth 16 --resolution 75

# Geometry: scan window and dpi must be honoured (area in mm).
check area_100dpi P5 "100 100" --mode Gray --resolution 100 -l 0 -t 0 -x 25.4 -y 25.4

# Hand scanner: image height is unknown (-1) until EOF.
check hand_scanner P5 - --mode Gray --hand-scanner=yes --resolution 75

# Three-pass colour: separate R/G/B frames. OpenToonz must report a clear
# error for these; scanimage itself should still succeed.
check three_pass P6 - --mode Color --three-pass=yes --resolution 75

# ADF: the test backend runs dry after 10 pages ("out of documents").
n=$(scanimage -d test --source "Automatic Document Feeder" --resolution 50 \
      --batch="$OUT/adf%d.pnm" --batch-count=12 2>/dev/null; true; ls "$OUT"/adf*.pnm 2>/dev/null | wc -l)
if [ "$n" = 10 ]; then echo "PASS  adf_10_pages_then_empty"; pass=$((pass+1))
else echo "FAIL  adf: expected 10 pages, got $n"; fail=$((fail+1)); fi

echo; echo "passed $pass, failed $fail   (output in $OUT)"
[ $fail -eq 0 ]
