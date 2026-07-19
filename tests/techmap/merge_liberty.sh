#!/usr/bin/env bash
set -u

# merge_liberty_a.lib and merge_liberty_b.lib describe the same two cells at two
# corners, with the values deliberately interleaved: for every table each corner
# holds the larger value at some points and the smaller at others. A merge that
# just took one corner's file wholesale would therefore be visibly wrong.

tmp=$(mktemp -d)
trap 'rm -rf $tmp' EXIT

fail() { echo "FAIL: $1"; exit 1; }

expect() {
    # expect <file> <pattern...> - each pattern must appear
    local f=$1; shift
    for pat in "$@"; do
        grep -qF -- "$pat" "$f" || fail "expected '$pat' in $(basename $f)"
    done
}

# --- worst case for a setup view: slowest delays, largest constraints --------
${YOSYS} -q -p "merge_liberty -o $tmp/setup.lib merge_liberty_a.lib merge_liberty_b.lib" \
    || fail "setup merge did not run"

expect $tmp/setup.lib \
    'area : 3.5'                 `# max(3.5, 2.5)` \
    'cell_leakage_power : 0.9'   `# max(0.7, 0.9)` \
    'capacitance : 0.006'        `# max(0.004, 0.006)` \
    'max_transition : 1.2'       `# tightest ceiling, min(1.5, 1.2)` \
    'max_capacitance : 0.20'     `# tightest ceiling, min(0.20, 0.35)` \
    'area : 9.5'                 `# max(9.0, 9.5)` \
    'values("0.90, 0.99", "0.31, 0.99")' `# cell_rise, elementwise max` \
    'values("0.91, 0.21", "0.99, 0.99")' `# cell_fall, elementwise max` \
    'values("0.95, 0.06", "0.97, 0.99")' `# rise_transition, elementwise max` \
    'values("0.20, 0.25", "0.22, 0.24")' `# setup constraint, largest window`

# --- worst case for a hold view: fastest delays, constraints unchanged ------
${YOSYS} -q -p "merge_liberty -mode hold -o $tmp/hold.lib merge_liberty_a.lib merge_liberty_b.lib" \
    || fail "hold merge did not run"

expect $tmp/hold.lib \
    'values("0.10, 0.12", "0.30, 0.40")' \
    'values("0.11, 0.19", "0.13, 0.41")' \
    'values("0.05, 0.04", "0.07, 0.08")' \
    'values("0.20, 0.25", "0.22, 0.24")' `# still the largest window` \
    'area : 3.5' 'cell_leakage_power : 0.9' 'max_transition : 1.2'

# --- merging a library with itself changes nothing --------------------------
${YOSYS} -q -p "merge_liberty -o $tmp/self.lib merge_liberty_a.lib merge_liberty_a.lib" \
    || fail "self merge did not run"
${YOSYS} -q -p "merge_liberty -o $tmp/self2.lib $tmp/self.lib $tmp/self.lib" \
    || fail "second self merge did not run"
cmp -s $tmp/self.lib $tmp/self2.lib || fail "merging a library with itself is not idempotent"

# --- a library that is not the same library must be refused, not approximated
sed '/cell(DFF)/,$d' merge_liberty_a.lib > $tmp/no_dff.lib; echo '}' >> $tmp/no_dff.lib
sed '/rise_transition/d' merge_liberty_b.lib > $tmp/no_arc.lib
sed 's/index_2 ("0.001, 0.05")/index_2 ("0.001, 0.09")/' merge_liberty_b.lib > $tmp/bad_axis.lib
sed 's/timing() {/timing() { unclassified_thing : 1.25;/' merge_liberty_a.lib > $tmp/unk_a.lib
sed 's/timing() {/timing() { unclassified_thing : 9.75;/' merge_liberty_b.lib > $tmp/unk_b.lib

refuse() {
    if ${YOSYS} -q -p "merge_liberty -o $tmp/out.lib $1 $2" 2>/dev/null; then
        fail "$3 was merged instead of refused"
    fi
}
refuse merge_liberty_a.lib $tmp/no_dff.lib   "a missing cell"
refuse merge_liberty_a.lib $tmp/no_arc.lib   "a missing timing arc"
refuse merge_liberty_a.lib $tmp/bad_axis.lib "a differing table axis"
refuse $tmp/unk_a.lib      $tmp/unk_b.lib    "an unclassified in-cell attribute"

# --- -resample reconciles corners characterised on different grids ----------
# f(s,c) = 1 + 2s + 3c + 4sc is exactly bilinear, so resampling it from one grid
# onto another inside its range has to reproduce it to the digit. Corner B holds
# 10x the same surface on a different grid, so B must win everywhere.
mkgrid() {
    local name=$1 xs=$2 ys=$3 scale=$4
    python3 - "$name" "$xs" "$ys" "$scale" > $tmp/$name.lib <<'PY'
import sys
name, xs, ys, scale = sys.argv[1], [float(v) for v in sys.argv[2].split(',')], \
                      [float(v) for v in sys.argv[3].split(',')], float(sys.argv[4])
f = lambda s, c: scale * (1 + 2*s + 3*c + 4*s*c)
rows = ", ".join('"%s"' % ", ".join("%.10g" % f(x, y) for y in ys) for x in xs)
ax, ay = ", ".join(map(str, xs)), ", ".join(map(str, ys))
print(f'''library({name}) {{
  lu_table_template(t) {{ variable_1 : input_net_transition; variable_2 : total_output_net_capacitance;
    index_1 ("{ax}"); index_2 ("{ay}"); }}
  cell(BUF) {{ area : 1.0;
    pin(A) {{ direction : input; }}
    pin(Y) {{ direction : output; function : "A";
      timing() {{ related_pin : "A";
        cell_rise(t) {{ index_1 ("{ax}"); index_2 ("{ay}"); values({rows}); }}
      }} }} }} }}''')
PY
}
if python3 -c '' 2>/dev/null; then
    mkgrid ra "0.0,1.0,2.0" "0.0,1.0,2.0" 1
    mkgrid rb "0.0,0.5,1.5,2.0" "0.0,0.25,2.0" 10

    # without -resample the differing grids are refused
    if ${YOSYS} -q -p "merge_liberty -o $tmp/out.lib $tmp/ra.lib $tmp/rb.lib" 2>/dev/null; then
        fail "differing characterisation grids were merged without -resample"
    fi

    ${YOSYS} -q -p "merge_liberty -resample -o $tmp/res.lib $tmp/ra.lib $tmp/rb.lib" \
        || fail "resampling merge did not run"
    # 10*f evaluated on grid A
    expect $tmp/res.lib 'values("10, 40, 70", "30, 100, 170", "50, 160, 270")'
else
    echo "No python3, skipping the resampling checks"
fi

echo "merge_liberty: ok"
