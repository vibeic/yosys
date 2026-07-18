// Golden behavioural model of the integrated clock gate in clock_gate.liberty:
// the enable is latched while the clock is low (glitch-free), and the gated
// clock is (clock AND latched-enable). read_liberty must import test_icg to an
// equivalent net.
module test_icg(input CLK, input GATE, output GCLK);
	reg m;
	always @(*)
		if (!CLK)
			m = GATE;
	assign GCLK = CLK & m;
endmodule
