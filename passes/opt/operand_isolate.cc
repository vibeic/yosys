/*
 *  yosys -- Yosys Open SYnthesis Suite   (vibeic fork)
 *
 *  operand_isolate -- datapath operand isolation for switching-/glitch-power
 *  reduction (a.k.a. "operand isolation" / "datapath gating", the transform
 *  behind DC minPower and Genus low-power datapath gating).
 *
 *  When a wide combinational datapath operator (a multiplier, adder, shifter,
 *  ...) feeds ONLY the data input of a clock-enabled register, its result is a
 *  don't-care on every cycle the enable is deasserted -- yet its operands keep
 *  toggling, so the whole operator switches (and glitches) every cycle for no
 *  observable effect. This pass AND-masks the operator's data operands with the
 *  register's (active-high) clock enable, so that whenever the register is not
 *  capturing, the operator sees constant-0 inputs and stops switching. When the
 *  register IS capturing the mask is transparent, so the captured value -- and
 *  therefore the register output -- is bit-identical.
 *
 *  Detection is the clock-gate enable-extraction pattern: iterate enabled FFs
 *  (any $dffe/$sdffe/$adffe/... -- read via FfData.has_ce), and for each one
 *  whose data input D is driven entirely by a single isolatable operator cell C
 *  whose output is PRIVATE to that D (fans out nowhere else and reaches no
 *  module output), mask C's A/B operands with the enable.
 *
 *  Conservative by construction: if the enable is missing, the datapath result
 *  is shared (observed when the enable is low), the operand is already a
 *  constant, or the operator is not a recognised datapath cell, the FF is left
 *  untouched -- a missed isolation is always safe, a wrong isolation never
 *  happens. Equivalence is sequential (the masked operand differs from the
 *  original exactly when the register ignores it), so prove with a sequential
 *  CEC step (equiv_make; equiv_induct) rather than a purely combinational miter:
 *
 *      operand_isolate
 *      equiv_make gold gate equiv; equiv_induct equiv; equiv_status -assert
 *
 *  The inserted mask gates are logically redundant under the enable, so a plain
 *  logic optimiser would delete them and undo the power saving; they are marked
 *  (* keep *) by default so the isolation survives to the mapped netlist. Pass
 *  -nokeep to leave them removable (e.g. when a later stage re-derives intent).
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/modtools.h"
#include "kernel/ff.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Expensive combinational operators worth isolating: two data operands carried
// on the canonical A / B input ports.
static const pool<IdString> &isolatable_types()
{
	static const pool<IdString> types = {
		ID($add), ID($sub), ID($mul), ID($div), ID($mod),
		ID($and), ID($or), ID($xor), ID($xnor),
		ID($shl), ID($shr), ID($sshl), ID($sshr),
	};
	return types;
}

struct OperandIsolateWorker
{
	Module *module;
	SigMap sigmap;
	ModWalker modwalker;
	bool keep;

	int isolated = 0;   // datapath operators gated
	int masked = 0;     // operand ports AND-masked

	pool<SigBit> output_bits;   // bits that reach a module output port

	struct Match { Cell *ff, *dp; SigBit ce; bool pol_ce; };

	OperandIsolateWorker(Module *module, bool keep) :
			module(module), sigmap(module),
			modwalker(module->design, module), keep(keep)
	{
		for (auto wire : module->wires())
			if (wire->port_output)
				for (auto bit : sigmap(SigSpec(wire)))
					output_bits.insert(bit);
	}

	// The single cell driving every bit of `sig`, or nullptr if the bits are
	// undriven or driven by more than one cell.
	Cell *sole_driver(const SigSpec &sig)
	{
		Cell *drv = nullptr;
		for (auto bit : sigmap(sig)) {
			pool<ModWalker::PortBit> drivers;
			if (!modwalker.get_drivers(drivers, bit))
				return nullptr;
			if (GetSize(drivers) != 1)
				return nullptr;
			Cell *c = drivers.begin()->cell;
			if (drv == nullptr)
				drv = c;
			else if (drv != c)
				return nullptr;
		}
		return drv;
	}

	// Every bit of `sig` is consumed by exactly one cell input port and never
	// wired to a module output -- i.e. the net is private to a single sink.
	bool is_private(const SigSpec &sig)
	{
		for (auto bit : sigmap(sig)) {
			if (output_bits.count(bit))
				return false;
			pool<ModWalker::PortBit> consumers;
			modwalker.get_consumers(consumers, bit);
			if (GetSize(consumers) != 1)
				return false;
		}
		return true;
	}

	void run()
	{
		// Phase 1: collect matches read-only (keeps modwalker's index valid).
		std::vector<Match> matches;
		for (auto cell : module->selected_cells()) {
			if (!RTLIL::builtin_ff_cell_types().count(cell->type))
				continue;
			FfData ff(nullptr, cell);
			if (!ff.has_ce)
				continue;
			if (GetSize(ff.sig_ce) != 1 || !ff.sig_ce[0].is_wire())
				continue;

			SigSpec d = sigmap(ff.sig_d);
			Cell *dp = sole_driver(d);
			if (!dp || !isolatable_types().count(dp->type) || !dp->hasPort(ID::Y))
				continue;
			if (sigmap(dp->getPort(ID::Y)) != d)   // D must be exactly the whole result
				continue;
			if (!is_private(dp->getPort(ID::Y)))   // result must not be observed elsewhere
				continue;

			matches.push_back(Match{cell, dp, ff.sig_ce[0], ff.pol_ce});
		}

		// Phase 2: apply the isolation.
		for (auto &m : matches) {
			SigBit en = m.ce;
			if (!m.pol_ce) {                       // normalise to active-high
				Wire *inv = module->addWire(NEW_ID);
				module->addNotGate(NEW_ID, m.ce, inv);
				en = SigBit(inv);
			}

			bool did = false;
			for (IdString p : {ID::A, ID::B}) {
				if (!m.dp->hasPort(p))
					continue;
				SigSpec op = m.dp->getPort(p);
				if (op.is_fully_const())
					continue;                      // nothing to gate on a constant
				int w = GetSize(op);
				// name the isolated net deterministically (aids debug, and lets
				// the idle-zeroing mechanism be probed by net name)
				Wire *iso = module->addWire(module->uniquify(
						stringf("$opiso$%s$%s", log_id(m.dp), log_id(p))), w);
				Cell *andc = module->addAnd(NEW_ID, op, SigSpec(en, w), SigSpec(iso), false);
				if (keep)
					andc->set_bool_attribute(ID::keep);
				m.dp->setPort(p, SigSpec(iso));
				masked++;
				did = true;
			}
			if (did) {
				isolated++;
				log("  isolated %s %s (enable %s) feeding %s %s\n",
						log_id(m.dp->type), log_id(m.dp),
						log_signal(m.ce), log_id(m.ff->type), log_id(m.ff));
			}
		}
	}
};

struct OperandIsolatePass : public Pass {
	OperandIsolatePass() : Pass("operand_isolate", "gate idle datapath operands with the register clock-enable") {}
	void help() override
	{
		log("\n");
		log("    operand_isolate [options] [selection]\n");
		log("\n");
		log("Insert operand-isolation (datapath-gating) AND masks: when a wide\n");
		log("combinational operator ($mul/$add/$sub/$shl/... on ports A,B) drives ONLY\n");
		log("the data input of a clock-enabled register, its A/B operands are AND-masked\n");
		log("with the register's active-high clock enable, so the operator stops\n");
		log("switching on every cycle the register is not capturing. The captured value\n");
		log("is unchanged, so the transform is register-output equivalent.\n");
		log("\n");
		log("    -nokeep\n");
		log("        do not mark the inserted mask cells (* keep *). By default the\n");
		log("        masks are kept, because they are logically redundant under the\n");
		log("        enable and a later logic optimiser would otherwise remove them\n");
		log("        (undoing the power saving).\n");
		log("\n");
		log("Conservative: an FF is only gated when its data input is the whole, private\n");
		log("output of a single recognised operator, so a wrong isolation never happens.\n");
		log("Equivalence is sequential -- prove it with:\n");
		log("\n");
		log("    equiv_make gold gate equiv; equiv_induct equiv; equiv_status -assert\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing OPERAND_ISOLATE pass (gate idle datapath operands).\n");
		bool keep = true;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-nokeep") {
				keep = false;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		int total_iso = 0, total_mask = 0;
		for (auto module : design->selected_modules()) {
			OperandIsolateWorker worker(module, keep);
			worker.run();
			total_iso += worker.isolated;
			total_mask += worker.masked;
		}
		log("Isolated %d datapath operator(s), masked %d operand port(s).\n",
				total_iso, total_mask);
	}
} OperandIsolatePass;

PRIVATE_NAMESPACE_END
