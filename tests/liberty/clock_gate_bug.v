// Deliberately WRONG clock gate: the enable is combinationally ANDed with the
// clock with no latch, so a GATE change while CLK is high glitches the gated
// clock. This is NOT equivalent to the latch-based test_icg model, so the LEC
// flow MUST report it as non-equivalent (the sound negative control).
module test_icg(input CLK, input GATE, output GCLK);
	assign GCLK = CLK & GATE;
endmodule
