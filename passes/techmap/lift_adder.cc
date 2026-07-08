/*
 *  yosys -- Yosys Open SYnthesis Suite   (vibeic fork)
 *
 *  lift_adder -- lift a gate-level ripple $fa chain back to a word-level $add.
 *
 *  `extract_fa -fa -ha` recognises the full-adders in a gate-level netlist and
 *  emits width-1 $fa cells (Y = A^B^C sum, X = majority(A,B,C) carry), but it
 *  PRESERVES the ripple carry topology: the cells are still wired X[i]->C[i+1],
 *  so the critical path stays O(n). Nothing in-tree groups those $fa cells back
 *  into a word-level adder, so the parallel-prefix techmap
 *  (+/choices/{kogge-stone,han-carlson,sklansky}.v, Brent-Kung default) can
 *  never restructure an already-gate-level ripple adder.
 *
 *  This pass closes that gap: it walks a maximal chain of width-1 $fa cells
 *  connected X[i]->C[i+1], and — only when it can prove the chain is a pure
 *  unsigned A+B ripple with PRIVATE internal carries — replaces it with one
 *  word-level $add whose Y is one bit wider (the top bit is the carry-out).
 *  Downstream `alumacc; techmap -map +/choices/<topology>.v -map +/techmap.v`
 *  then lowers that $add to a log-depth prefix adder, and a CEC step proves the
 *  restructured design equals the original.
 *
 *  Conservative by construction (see the bail list in run()): anything that is
 *  not a clean, privately-carried ripple is left untouched — a missed lift is
 *  always safe (the gate-level adder is simply mapped as before), a wrong lift
 *  never happens. NOTE: like all yosys arithmetic lowering, the lifted $add has
 *  stronger 2-state semantics than the $fa chain's pessimistic x-propagation on
 *  the carry; this is standard and CEC-equivalent over 2-state.
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct LiftAdderWorker
{
	Module *module;
	SigMap sigmap;
	int lifted = 0;

	LiftAdderWorker(Module *module) : module(module), sigmap(module) {}

	void run()
	{
		// --- index the width-1 $fa cells ------------------------------------
		dict<Cell *, SigBit> ci_of, co_of;      // cell -> sigmap(C), sigmap(X)
		dict<SigBit, Cell *> carryin_net_to_cell;  // sigmap(C) -> the cell whose C it is
		std::vector<Cell *> fa_cells;
		for (auto cell : module->cells()) {
			if (cell->type != ID($fa))
				continue;
			if (cell->getParam(ID::WIDTH).as_int() != 1)
				continue;
			SigBit c = sigmap(cell->getPort(ID::C));
			SigBit x = sigmap(cell->getPort(ID::X));
			ci_of[cell] = c;
			co_of[cell] = x;
			// A carry-in net feeding two different $fa cells is not a clean
			// ripple; the fanout check below rejects that chain anyway, so a
			// last-writer-wins map is sufficient here.
			carryin_net_to_cell[c] = cell;
			fa_cells.push_back(cell);
		}
		if (fa_cells.empty())
			return;

		// --- fanout (input-port consumers) of every net + module outputs -----
		dict<SigBit, int> fanout;
		for (auto cell : module->cells()) {
			// For a cell whose type is UNRESOLVED (an undefined module, or a
			// blackbox declared without port directions), Cell::input() returns
			// false for every port, so a real consumer of an internal carry would
			// be undercounted and the private-carry check could wrongly pass.
			// Conservatively treat EVERY connected bit of such a cell as a
			// consumer, so any carry it observes trips the fanout!=1 bail. (On a
			// hierarchy-resolved netlist no such cell exists; this keeps the "a
			// wrong lift never happens" guarantee true even standalone.)
			bool dirs_known = cell->known();
			for (auto &conn : cell->connections())
				if (!dirs_known || cell->input(conn.first))
					for (auto bit : sigmap(conn.second))
						fanout[bit]++;
		}
		pool<SigBit> output_bits;
		for (auto wire : module->wires())
			if (wire->port_output)
				for (auto bit : sigmap(SigSpec(wire)))
					output_bits.insert(bit);

		pool<Cell *> claimed;

		// --- walk each chain from its LSB (carry-in tied to constant 0) ------
		for (Cell *head : fa_cells) {
			if (claimed.count(head))
				continue;
			// Head = the LSB: its carry-in must be a hard 0 (a real carry-in or
			// a subtractor's S1 LSB carry cannot be modelled by a 2-input $add).
			SigBit ci = ci_of.at(head);
			if (ci.wire != nullptr || ci != State::S0)
				continue;

			std::vector<Cell *> chain;
			std::vector<SigBit> A_bits, B_bits, S_bits;
			pool<Cell *> seen;
			Cell *cur = head;
			bool ok = true;

			while (true) {
				if (claimed.count(cur) || seen.count(cur)) { ok = false; break; }
				seen.insert(cur);
				chain.push_back(cur);
				A_bits.push_back(sigmap(cur->getPort(ID::A)));
				B_bits.push_back(sigmap(cur->getPort(ID::B)));
				S_bits.push_back(sigmap(cur->getPort(ID::Y)));  // sum bit i

				SigBit co = co_of.at(cur);
				bool next_is_fa = carryin_net_to_cell.count(co) &&
				                  !claimed.count(carryin_net_to_cell.at(co)) &&
				                  carryin_net_to_cell.at(co) != cur;
				if (next_is_fa) {
					// Internal carry: it MUST be private — feed exactly the next
					// $fa's C and nothing else, and never a module output. An
					// observed internal carry means this is not a plain ripple.
					if (fanout.at(co) != 1 || output_bits.count(co)) { ok = false; break; }
					cur = carryin_net_to_cell.at(co);
					continue;
				}
				// End of chain: co is the adder's carry-out = top sum bit. It may
				// legitimately be a module output / fan out anywhere.
				S_bits.push_back(co);
				break;
			}
			if (!ok || GetSize(chain) < 2)
				continue;

			// Output nets must be distinct and must not also be addends (a $add
			// cannot drive one net twice, nor read-and-drive the same net).
			pool<SigBit> sset;
			bool bad = false;
			for (auto b : S_bits) { if (sset.count(b)) { bad = true; break; } sset.insert(b); }
			if (!bad)
				for (auto b : A_bits) if (sset.count(b)) { bad = true; break; }
			if (!bad)
				for (auto b : B_bits) if (sset.count(b)) { bad = true; break; }
			if (bad)
				continue;

			// --- lift: one word-level unsigned $add, Y = {carryout, sum} -----
			SigSpec A(A_bits), B(B_bits), S(S_bits);
			log_assert(GetSize(A) == GetSize(B));
			log_assert(GetSize(S) == GetSize(A) + 1);
			log("  lifting %d-bit ripple ($fa x%d) -> word-level $add in %s.\n",
			    GetSize(chain), GetSize(chain), log_id(module));
			module->addAdd(NEW_ID, A, B, S, /*is_signed=*/false);
			for (Cell *c : chain)
				claimed.insert(c);
			for (Cell *c : chain)
				module->remove(c);
			lifted++;
		}
	}
};

struct LiftAdderPass : public Pass {
	LiftAdderPass() : Pass("lift_adder", "lift gate-level ripple $fa chains to word-level $add") {}
	void help() override
	{
		log("\n");
		log("    lift_adder [selection]\n");
		log("\n");
		log("Groups a maximal chain of width-1 $fa cells wired X[i]->C[i+1] (as produced\n");
		log("by 'extract_fa -fa -ha' on a gate-level ripple adder) into a single word-level\n");
		log("$add, so the parallel-prefix techmap (+/choices/*.v) can restructure it to a\n");
		log("log-depth carry tree.\n");
		log("\n");
		log("Conservative: only a pure unsigned A+B ripple with a constant-0 LSB carry-in\n");
		log("and PRIVATE internal carries is lifted; anything else is left untouched, so a\n");
		log("wrong lift never happens. Run 'opt_clean' afterwards to sweep the now-dangling\n");
		log("internal carry nets (or rely on the subsequent alumacc/techmap cleanup).\n");
		log("\n");
		log("Typical use (equivalence-preserving, prove with a CEC step):\n");
		log("    extract_fa -fa -ha; lift_adder; alumacc;\n");
		log("    techmap -map +/choices/kogge-stone.v -map +/techmap.v\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing LIFT_ADDER pass (lift gate-level ripple $fa chains to $add).\n");
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
			break;
		extra_args(args, argidx, design);

		int total = 0;
		for (auto module : design->selected_modules()) {
			LiftAdderWorker worker(module);
			worker.run();
			total += worker.lifted;
		}
		log("Lifted %d ripple adder(s) to word-level $add.\n", total);
	}
} LiftAdderPass;

PRIVATE_NAMESPACE_END
