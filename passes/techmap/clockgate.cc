#include "kernel/yosys.h"
#include "kernel/ff.h"
#include "kernel/gzip.h"
#include "libparse.h"
#include <optional>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct ClockGateCell {
	IdString name;
	IdString ce_pin;
	IdString clk_in_pin;
	IdString clk_out_pin;
	std::vector<IdString> tie_lo_pins;
};

ClockGateCell icg_from_arg(std::string& name, std::string& str) {
	ClockGateCell c;
	c.name = RTLIL::escape_id(name);
	char delimiter = ':';
	size_t pos1 = str.find(delimiter);
	if (pos1 == std::string::npos)
		log_cmd_error("Not enough ports in descriptor string");
	size_t pos2 = str.find(delimiter, pos1 + 1);
	if (pos2 == std::string::npos)
		log_cmd_error("Not enough ports in descriptor string");
	size_t pos3 = str.find(delimiter, pos2 + 1);
	if (pos3 != std::string::npos)
		log_cmd_error("Too many ports in descriptor string");

	std::string ce = str.substr(0, pos1);
	c.ce_pin = RTLIL::escape_id(ce);

	std::string clk_in = str.substr(pos1 + 1, pos2 - (pos1 + 1));
	c.clk_in_pin = RTLIL::escape_id(clk_in);

	std::string clk_out = str.substr(pos2 + 1, str.size() - (pos2 + 1));
	c.clk_out_pin = RTLIL::escape_id(clk_out);
	return c;
}

static std::pair<std::optional<ClockGateCell>, std::optional<ClockGateCell>>
	find_icgs(std::vector<const LibertyAst *> cells, std::vector<std::string> const& dont_use_cells) {
	// We will pick the most suitable ICG absed on tie_lo count and area
	struct ICGRankable : public ClockGateCell { double area; };
	std::optional<ICGRankable> best_pos;
	std::optional<ICGRankable> best_neg;

	// This is a lot of boilerplate, isn't it?
	for (auto cell : cells)
	{
		const LibertyAst *dn = cell->find("dont_use");
		if (dn != nullptr && dn->value == "true")
			continue;

		bool dont_use = false;
		for (auto dont_use_cell : dont_use_cells)
		{
			if (patmatch(dont_use_cell.c_str(), cell->args[0].c_str()))
			{
				dont_use = true;
				break;
			}
		}
		if (dont_use)
			continue;

		const LibertyAst *icg_kind_ast = cell->find("clock_gating_integrated_cell");
		if (icg_kind_ast == nullptr)
			continue;

		auto cell_name = cell->args[0];
		auto icg_kind = icg_kind_ast->value;
		auto starts_with = [&](std::string prefix) {
			return icg_kind.compare(0, prefix.size(), prefix) == 0;
		};
		bool clk_pol;
		if (icg_kind == "latch_posedge" || starts_with("latch_posedge_")) {
			clk_pol = true;
		} else if (icg_kind == "latch_negedge" || starts_with("latch_negedge_")) {
			clk_pol = false;
		} else {
			log("Ignoring ICG primitive %s of kind '%s'\n", cell_name, icg_kind);
			continue;
		}

		log_debug("maybe valid icg: %s\n", cell_name);
		ClockGateCell icg_interface;
		icg_interface.name = RTLIL::escape_id(cell_name);

		for (auto pin : cell->children) {
			if (pin->id != "pin" || pin->args.size() != 1)
				continue;

			if (pin->find("clock_gate_clock_pin")) {
				if (!icg_interface.clk_in_pin.empty()) {
					log_warning("Malformed liberty file - multiple clock_gate_clock_pin in cell %s\n",
						cell_name.c_str());
					continue;
				} else
					icg_interface.clk_in_pin = RTLIL::escape_id(pin->args[0]);
			} else if (pin->find("clock_gate_out_pin")) {
				if (!icg_interface.clk_out_pin.empty()) {
					log_warning("Malformed liberty file - multiple clock_gate_out_pin in cell %s\n",
						cell_name.c_str());
					continue;
				} else
					icg_interface.clk_out_pin = RTLIL::escape_id(pin->args[0]);
			} else if (pin->find("clock_gate_enable_pin")) {
				if (!icg_interface.ce_pin.empty()) {
					log_warning("Malformed liberty file - multiple clock_gate_enable_pin in cell %s\n",
						cell_name.c_str());
					continue;
				} else
					icg_interface.ce_pin = RTLIL::escape_id(pin->args[0]);
			} else if (pin->find("clock_gate_test_pin")) {
				icg_interface.tie_lo_pins.push_back(RTLIL::escape_id(pin->args[0]));
			} else {
				const LibertyAst *dir = pin->find("direction");
				if (dir->value == "internal")
					continue;

				log_warning("Malformed liberty file - extra pin %s in cell %s\n",
					pin->args[0].c_str(), cell_name.c_str());
				continue;
			}
		}

		if (icg_interface.clk_in_pin.empty()) {
			log_warning("Malformed liberty file - missing clock_gate_clock_pin in cell %s",
				cell_name.c_str());
			continue;
		}
		if (icg_interface.clk_out_pin.empty()) {
			log_warning("Malformed liberty file - missing clock_gate_out_pin in cell %s",
				cell_name.c_str());
			continue;
		}
		if (icg_interface.ce_pin.empty()) {
			log_warning("Malformed liberty file - missing clock_gate_enable_pin in cell %s",
				cell_name.c_str());
			continue;
		}

		double area = 0;
		const LibertyAst *ar = cell->find("area");
		if (ar != nullptr && !ar->value.empty())
			area = atof(ar->value.c_str());

		std::optional<ICGRankable>& icg_to_beat = clk_pol ? best_pos : best_neg;

		bool winning = false;
		if (icg_to_beat) {
			log_debug("ties: %zu ? %zu\n", icg_to_beat->tie_lo_pins.size(),
				icg_interface.tie_lo_pins.size());
			log_debug("area: %f ? %f\n", icg_to_beat->area, area);

			// Prefer fewer test enables over area reduction (unlikely to matter)
			auto goal = std::make_pair(icg_to_beat->tie_lo_pins.size(), icg_to_beat->area);
			auto cost = std::make_pair(icg_interface.tie_lo_pins.size(), area);
			winning = cost < goal;

			if (winning)
				log_debug("%s beats %s\n", icg_interface.name, icg_to_beat->name);
		} else {
			log_debug("%s is the first of its polarity\n", icg_interface.name);
			winning = true;
		}
		if (winning) {
			ICGRankable new_icg {icg_interface, area};
			icg_to_beat.emplace(new_icg);
		}
	}

	std::optional<ClockGateCell> pos;
	std::optional<ClockGateCell> neg;
	if (best_pos) {
		log("Selected rising edge ICG %s from Liberty file\n", best_pos->name);
		pos.emplace(*best_pos);
	}
	if (best_neg) {
		log("Selected falling edge ICG %s from Liberty file\n", best_neg->name);
		neg.emplace(*best_neg);
	}
	return std::make_pair(pos, neg);
}

struct ClockgatePass : public Pass {
	ClockgatePass() : Pass("clockgate", "extract clock gating out of flip flops") { }
	void help() override {
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    clockgate [options] [selection]\n");
		log("\n");
		log("This pass transforms each set of FFs sharing the same clock and\n");
		log("enable signal into a clock-gating cell and a set of enable-less FFs.\n");
		log("Primarily a power-saving transformation on ASIC designs.\n");
		log("\n");
		log("    -pos <celltype> <ce>:<clk>:<gclk>\n");
		log("        If specified, rising-edge FFs will have CE inputs\n");
		log("        removed and a gated clock will be created by the\n");
		log("        user-specified <celltype> ICG (integrated clock gating)\n");
		log("        cell with ports named <ce>, <clk>, <gclk>.\n");
		log("        The ICG's clock enable pin must be active high.\n");
		log("    -neg <celltype> <ce>:<clk>:<gclk>\n");
		log("        If specified, falling-edge FFs will have CE inputs\n");
		log("        removed and a gated clock will be created by the\n");
		log("        user-specified <celltype> ICG (integrated clock gating)\n");
		log("        cell with ports named <ce>, <clk>, <gclk>.\n");
		log("        The ICG's clock enable pin must be active high.\n");
		log("    -liberty <filename>\n");
		log("        If specified, ICGs will be selected from the liberty files\n");
		log("        if available. Priority is given to cells with fewer tie_lo\n");
		log("        inputs and smaller size. This removes the need to manually\n");
		log("        specify -pos or -neg and -tie_lo.\n");
		log("    -dont_use <celltype>\n");
		log("        Cells <celltype> won't be considered when searching for ICGs\n");
		log("        in the liberty file specified by -liberty.\n");
		log("    -tie_lo <port_name>\n");
		log("        Port <port_name> of the ICG will be tied to zero.\n");
		log("        Intended for DFT scan-enable pins.\n");
		log("    -min_net_size <n>\n");
		log("        Only transform sets of at least <n> eligible FFs.\n");
		log("    -max_net_size <n>\n");
		log("        Limit each ICG to at most <n> gated FFs. Larger sets are\n");
		log("        split over several identical ICGs driving disjoint groups\n");
		log("        of FFs, bounding the gated-clock fanout a downstream CTS\n");
		log("        has to balance. Off (unlimited) by default.\n");
		log("    -share_hierarchy\n");
		log("        Chain ICGs whose enables are nested: if one group's enable\n");
		log("        is a conjunction that contains another group's enable as a\n");
		log("        sub-term, the narrower group's ICG is clocked from the\n");
		log("        wider group's gated clock instead of the root clock, so it\n");
		log("        stops toggling whenever the outer enable is low. Only\n");
		log("        applies to active-high enables not folded with a reset.\n");
		log("        Off by default.\n");
		log("    -gate_srst\n");
		log("        Also gate FFs whose synchronous reset takes priority over\n");
		log("        their clock enable (the common 'if (rst) .. else if (en) ..'\n");
		log("        idiom, i.e. $sdffe), which is otherwise left ungated. The\n");
		log("        ICG enable becomes (CE | SRST) so the reset still reaches\n");
		log("        the FF on the cycles it is asserted. Off by default.\n");
		log("        \n");
	}

	// One ICG will be generated per ClkNetInfo
	// if the number of FFs associated with it is sufficent
	struct ClkNetInfo {
		// Original, ungated clock into enabled FF
		SigBit clk_bit;
		// Original clock enable into enabled FF
		SigBit ce_bit;
		bool pol_clk;
		bool pol_ce;
		// Sync reset of an $sdffe (reset over enable) folded into the ICG
		// enable as (CE | SRST). Only set under -gate_srst.
		bool has_srst = false;
		SigBit srst_bit = State::S0;
		bool pol_srst = true;
		[[nodiscard]] Hasher hash_into(Hasher h) const {
			auto t = std::make_tuple(clk_bit, ce_bit, pol_clk, pol_ce,
						 has_srst, srst_bit, pol_srst);
			h.eat(t);
			return h;
		}
		bool operator==(const ClkNetInfo& other) const {
			return (clk_bit == other.clk_bit) &&
			       (ce_bit == other.ce_bit) &&
			       (pol_clk == other.pol_clk) &&
			       (pol_ce == other.pol_ce) &&
			       (has_srst == other.has_srst) &&
			       (srst_bit == other.srst_bit) &&
			       (pol_srst == other.pol_srst);
		}
	};

	struct GClkNetInfo {
		// How many CE FFs on this CLK net have we seen?
		int net_size;
		// After ICG generation, we have new gated CLK signals
		Wire* new_net;
		// Every ICG driving this group (>1 only under -max_net_size)
		std::vector<Cell*> icgs;
	};

	// Leaves of the conjunction driving a one-bit enable, so that one
	// enable can be recognised as implying another: if the leaf set of A
	// is a subset of the leaf set of B then B is only ever true when A is.
	void collect_and_leaves(SigBit bit, SigMap &sigmap,
				const dict<SigBit, Cell*> &drivers,
				pool<SigBit> &leaves, int depth = 0) {
		bit = sigmap(bit);
		auto it = depth < 32 ? drivers.find(bit) : drivers.end();
		if (it != drivers.end()) {
			Cell *c = it->second;
			if (c->type.in(ID($_AND_), ID($and), ID($logic_and))) {
				SigSpec a = c->getPort(ID::A);
				SigSpec b = c->getPort(ID::B);
				if (GetSize(a) == 1 && GetSize(b) == 1) {
					collect_and_leaves(a[0], sigmap, drivers, leaves, depth + 1);
					collect_and_leaves(b[0], sigmap, drivers, leaves, depth + 1);
					return;
				}
			}
		}
		leaves.insert(bit);
	}

	ClkNetInfo clk_info_from_ff(FfData& ff) {
		SigBit clk = ff.sig_clk[0];
		SigBit ce = ff.sig_ce[0];
		ClkNetInfo info{clk, ce, ff.pol_clk, ff.pol_ce};
		if (ff.has_srst && !ff.ce_over_srst) {
			info.has_srst = true;
			info.srst_bit = ff.sig_srst[0];
			info.pol_srst = ff.pol_srst;
		}
		return info;
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override {
		log_header(design, "Executing CLOCK_GATE pass (extract clock gating out of flip flops).\n");

		std::optional<ClockGateCell> pos_icg_desc;
		std::optional<ClockGateCell> neg_icg_desc;
		std::vector<std::string> tie_lo_pins;
		std::vector<std::string> liberty_files;
		std::vector<std::string> dont_use_cells;
		int min_net_size = 0;
		int max_net_size = 0;
		bool gate_srst = false;
		bool share_hierarchy = false;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-pos" && argidx+2 < args.size()) {
				auto name = args[++argidx];
				auto rest = args[++argidx];
				pos_icg_desc = icg_from_arg(name, rest);
				continue;
			}
			if (args[argidx] == "-neg" && argidx+2 < args.size()) {
				auto name = args[++argidx];
				auto rest = args[++argidx];
				neg_icg_desc = icg_from_arg(name, rest);
				continue;
			}
			if (args[argidx] == "-tie_lo" && argidx+1 < args.size()) {
				tie_lo_pins.push_back(RTLIL::escape_id(args[++argidx]));
				continue;
			}
			if (args[argidx] == "-liberty" && argidx+1 < args.size()) {
				append_globbed(liberty_files, args[++argidx]);
				continue;
			}
			if (args[argidx] == "-dont_use" && argidx+1 < args.size()) {
				dont_use_cells.push_back(args[++argidx]);
				continue;
			}
			if (args[argidx] == "-min_net_size" && argidx+1 < args.size()) {
				min_net_size = atoi(args[++argidx].c_str());
				continue;
			}
			if (args[argidx] == "-max_net_size" && argidx+1 < args.size()) {
				max_net_size = atoi(args[++argidx].c_str());
				if (max_net_size < 0)
					log_cmd_error("-max_net_size must not be negative\n");
				continue;
			}
			if (args[argidx] == "-gate_srst") {
				gate_srst = true;
				continue;
			}
			if (args[argidx] == "-share_hierarchy") {
				share_hierarchy = true;
				continue;
			}
			break;
		}

		if (!liberty_files.empty()) {
			LibertyMergedCells merged;
			for (auto path : liberty_files) {
				std::istream* f = uncompressed(path);
				LibertyParser p(*f, path);
				merged.merge(p);
				delete f;
			}
			std::tie(pos_icg_desc, neg_icg_desc) =
				find_icgs(merged.cells, dont_use_cells);
		} else {
			for (auto pin : tie_lo_pins) {
				if (pos_icg_desc)
					pos_icg_desc->tie_lo_pins.push_back(pin);
				if (neg_icg_desc)
					neg_icg_desc->tie_lo_pins.push_back(pin);
			}
		}

		extra_args(args, argidx, design);

		pool<Cell*> ce_ffs;
		dict<ClkNetInfo, GClkNetInfo> clk_nets;
		// Deterministic per-group FF order (module->cells() order), only
		// consulted when -max_net_size splits a group over several ICGs.
		dict<ClkNetInfo, std::vector<Cell*>> group_ffs;
		// FF -> gated clock of the ICG it was assigned to when splitting
		dict<Cell*, Wire*> split_gclk;

		int gated_flop_count = 0;
		int icg_count = 0;
		int chained_icg_count = 0;
		for (auto module : design->selected_unboxed_whole_modules()) {
			for (auto cell : module->cells()) {
				if (!cell->is_builtin_ff())
					continue;

				FfData ff(nullptr, cell);
				// It would be odd to get constants, but we better handle it
				if (ff.has_ce) {
					if (ff.has_srst && !ff.ce_over_srst) {
						// The sync reset outranks the enable, so the FF
						// still has to see a clock edge whenever the reset
						// is asserted. Foldable into the ICG enable, but
						// only on request.
						if (!gate_srst)
							continue;
						if (!ff.sig_srst.is_bit() || !ff.sig_srst[0].is_wire())
							continue;
					}
					if (!ff.sig_clk.is_bit() || !ff.sig_ce.is_bit())
						continue;
					if (!ff.sig_clk[0].is_wire() || !ff.sig_ce[0].is_wire())
						continue;

					ce_ffs.insert(cell);

					ClkNetInfo info = clk_info_from_ff(ff);
					auto it = clk_nets.find(info);
					if (it == clk_nets.end())
						clk_nets[info] = GClkNetInfo();
					clk_nets[info].net_size++;
					group_ffs[info].push_back(cell);
				}
			}

			for (auto& clk_net : clk_nets) {
				auto& clk = clk_net.first;
				auto& gclk = clk_net.second;

				if (gclk.net_size < min_net_size)
					continue;

				std::optional<ClockGateCell> matching_icg_desc;

				if (pos_icg_desc && clk.pol_clk)
					matching_icg_desc = pos_icg_desc;
				else if (neg_icg_desc && !clk.pol_clk)
					matching_icg_desc = neg_icg_desc;

				if (!matching_icg_desc)
					continue;

				// One ICG unless -max_net_size caps the gated-clock fanout
				int chunk = max_net_size > 0 ? max_net_size : gclk.net_size;
				int n_icgs = max_net_size > 0
					? (gclk.net_size + max_net_size - 1) / max_net_size : 1;
				auto& members = group_ffs.at(clk);

				for (int i = 0; i < n_icgs; i++) {
					Cell* icg = module->addCell(NEW_ID, matching_icg_desc->name);
					icg->setPort(matching_icg_desc->ce_pin, clk.ce_bit);
					icg->setPort(matching_icg_desc->clk_in_pin, clk.clk_bit);
					Wire* gclk_net = module->addWire(NEW_ID);
					if (i == 0)
						gclk.new_net = gclk_net;
					icg->setPort(matching_icg_desc->clk_out_pin, gclk_net);
					// Tie low DFT ports like scan chain enable
					for (auto port : matching_icg_desc->tie_lo_pins)
						icg->setPort(port, Const(0, 1));
					// Fix CE polarity if needed
					SigBit ce_fixed_pol = clk.ce_bit;
					if (!clk.pol_ce) {
						ce_fixed_pol = module->NotGate(NEW_ID, clk.ce_bit);
						icg->setPort(matching_icg_desc->ce_pin, ce_fixed_pol);
					}
					// A sync reset that outranks the enable must not be
					// gated away: enable the ICG on (CE | SRST).
					if (clk.has_srst) {
						SigBit srst_fixed_pol = clk.pol_srst
							? clk.srst_bit
							: module->NotGate(NEW_ID, clk.srst_bit);
						SigBit ce_or_srst = module->OrGate(NEW_ID,
							ce_fixed_pol, srst_fixed_pol);
						icg->setPort(matching_icg_desc->ce_pin, ce_or_srst);
					}
					icg_count++;
					gclk.icgs.push_back(icg);

					if (n_icgs > 1)
						for (int j = i * chunk;
						     j < std::min<int>((i + 1) * chunk, GetSize(members)); j++)
							split_gclk[members[j]] = gclk_net;
				}
			}

			// Re-parent nested ICGs. Done after every ICG exists so the
			// order in which they were created is untouched.
			if (share_hierarchy) {
				SigMap sigmap(module);
				dict<SigBit, Cell*> drivers;
				for (auto cell : module->cells())
					for (auto &conn : cell->connections())
						if (cell->output(conn.first))
							for (auto bit : sigmap(conn.second))
								drivers[bit] = cell;

				// Enables we can reason about: active high, no folded reset
				std::vector<ClkNetInfo> cand;
				dict<ClkNetInfo, pool<SigBit>> leaves;
				for (auto& clk_net : clk_nets) {
					auto& clk = clk_net.first;
					if (!clk.pol_ce || clk.has_srst)
						continue;
					if (clk_net.second.icgs.empty())
						continue;
					pool<SigBit> l;
					collect_and_leaves(clk.ce_bit, sigmap, drivers, l);
					leaves[clk] = l;
					cand.push_back(clk);
				}

				for (auto& child : cand) {
					auto& child_leaves = leaves.at(child);
					// Deepest strict subset wins: the tightest enable
					// that is still implied by this one.
					const ClkNetInfo *parent = nullptr;
					size_t best = 0;
					for (auto& other : cand) {
						if (other == child)
							continue;
						if (other.clk_bit != child.clk_bit ||
						    other.pol_clk != child.pol_clk)
							continue;
						auto& other_leaves = leaves.at(other);
						if (other_leaves.size() >= child_leaves.size())
							continue;
						bool subset = true;
						for (auto bit : other_leaves)
							if (!child_leaves.count(bit)) {
								subset = false;
								break;
							}
						if (!subset)
							continue;
						if (other_leaves.size() > best) {
							best = other_leaves.size();
							parent = &other;
						}
					}
					if (!parent)
						continue;

					// Subset is a strict partial order, so chaining
					// along it cannot close a loop.
					Wire *parent_gclk = clk_nets.at(*parent).new_net;
					auto matching_icg_desc = child.pol_clk
						? pos_icg_desc : neg_icg_desc;
					for (auto icg : clk_nets.at(child).icgs)
						icg->setPort(matching_icg_desc->clk_in_pin, parent_gclk);
					log_debug("Chained ICG of enable %s onto %s\n",
						log_signal(child.ce_bit), log_signal(parent->ce_bit));
					chained_icg_count++;
				}
			}

			for (auto cell : ce_ffs) {
				FfData ff(nullptr, cell);
				ClkNetInfo info = clk_info_from_ff(ff);
				auto it = clk_nets.find(info);
				log_assert(it != clk_nets.end() && "Bug: desync ce_ffs and clk_nets");

				if (!it->second.new_net)
					continue;

				log_debug("Fix up FF %s\n", cell->name);
				// Now we start messing with the design
				ff.has_ce = false;
				// Construct the clock gate
				// ICG = integrated clock gate, industry shorthand
				auto split_it = split_gclk.find(cell);
				ff.sig_clk = split_it != split_gclk.end()
					? split_it->second : (*it).second.new_net;

				// Rebuild the flop
				(void)ff.emit();

				gated_flop_count++;
			}
			ce_ffs.clear();
			clk_nets.clear();
			group_ffs.clear();
			split_gclk.clear();
		}

		log("Converted %d FFs into %d ICGs.\n", gated_flop_count, icg_count);
		if (chained_icg_count)
			log("Chained %d ICGs onto an enclosing gated clock.\n", chained_icg_count);
    }
} ClockgatePass;


PRIVATE_NAMESPACE_END
