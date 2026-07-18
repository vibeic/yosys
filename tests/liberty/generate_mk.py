#!/usr/bin/env python3

import glob
import os
import sys
sys.path.append("..")

import gen_tests_makefile

def lib_tests():
    for lib in sorted(glob.glob("*.lib")):
        base = os.path.splitext(lib)[0]

        gen_tests_makefile.generate_cmd_test(lib, [
            f'$(YOSYS) -p "read_verilog small.v; synth -top small; dfflibmap -info -liberty {lib}" -ql {base}.log',

            f'$(YOSYS_FILTERLIB) - {lib} > {lib}.filtered',
            f'$(YOSYS_FILTERLIB) -verilogsim {lib} > {lib}.verilogsim',

            f'diff {lib}.filtered {lib}.filtered.ok',
            f'diff {lib}.verilogsim {lib}.verilogsim.ok',

            f'if [ -e {base}.log.ok ]; then '
            f'$(YOSYS) -p "dfflibmap -info -liberty {lib}" -TqqQl {base}.log; '
            f'diff {base}.log {base}.log.ok; '
            f'fi',
        ])


def ys_tests():
    for ys in sorted(glob.glob("*.ys")):
        gen_tests_makefile.generate_ys_test(ys)

def clock_gate_test():
    # Integrated clock-gating (ICG) cell modelling: read_liberty must import a
    # gated-clock output described by state_function/statetable (no plain
    # `function`) as a latch-based clock gate rather than aborting. The positive
    # check proves the imported model equals the behavioural golden gate; the
    # negative check proves a wrong (unlatched) gate is reported non-equivalent.
    equiv = ("rename test_icg icg_lib; read_verilog {vlog}; proc; "
             "rename test_icg icg_vlog; async2sync; "
             "equiv_make icg_lib icg_vlog equiv; equiv_induct equiv; "
             "equiv_status -assert equiv")
    pos = equiv.format(vlog="clock_gate_ref.v")
    neg = equiv.format(vlog="clock_gate_bug.v")
    gen_tests_makefile.generate_cmd_test("clock_gate", [
        f'$(YOSYS) -qp "read_liberty clock_gate.liberty; {pos}"',
        f'! $(YOSYS) -qp "read_liberty clock_gate.liberty; {neg}"',
    ])

def main():
    def callback():
        lib_tests()
        ys_tests()
        clock_gate_test()

    gen_tests_makefile.generate_custom(callback)


if __name__ == "__main__":
    main()
