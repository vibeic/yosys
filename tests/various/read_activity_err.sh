#!/usr/bin/env bash
set -u
# A file that annotates nothing must fail loudly: later passes would otherwise
# treat a live design as static.
fail() { echo "FAIL: $1"; exit 1; }

tmp=$(mktemp -d); trap 'rm -rf $tmp' EXIT
cat > $tmp/d.v <<'EOT'
module top(input clk, input a, input [3:0] bus, output y);
	assign y = a ^ bus[0];
endmodule
EOT

if ${YOSYS} -q -p "read_verilog $tmp/d.v; hierarchy -top top; proc; read_activity -scope nosuch.scope read_activity.vcd" </dev/null 2>/dev/null; then
    fail "a VCD matching no wire was accepted"
fi

cat > $tmp/ghost.vcd <<'EOT'
$scope module tb $end
$scope module top $end
$var wire 1 ! clk $end
$var wire 1 " ghost1 $end
$var wire 1 # ghost2 $end
$var wire 1 $ ghost3 $end
$upscope $end
$upscope $end
$enddefinitions $end
#0
0! 0" 0# 0$
#10
1! 1" 1# 1$
EOT
if ${YOSYS} -q -p "read_verilog $tmp/d.v; hierarchy -top top; proc; read_activity -scope tb.top -unmatched 50 $tmp/ghost.vcd" </dev/null 2>/dev/null; then
    fail "75% unmatched was accepted under -unmatched 50"
fi

echo "read_activity_err: ok"
