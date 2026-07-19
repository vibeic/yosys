/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/gzip.h"
#include "libparse.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// How a numeric attribute is combined across corners.
enum MergeRule {
	RULE_MAX,	// the pessimistic value is the largest
	RULE_MIN,	// the pessimistic value is the smallest
	RULE_DELAY,	// slowest for a setup view, fastest for a hold view
	RULE_SAME,	// must agree across corners
	RULE_UNKNOWN,	// not classified - refuse to guess
};

static const pool<std::string> &delay_ids()
{
	static const pool<std::string> ids = {
		"cell_rise", "cell_fall", "rise_transition", "fall_transition",
		"rise_propagation", "fall_propagation",
		"intrinsic_rise", "intrinsic_fall",
		"rise_delay_intercept", "fall_delay_intercept",
		"rise_pin_resistance", "fall_pin_resistance",
		"retaining_rise", "retaining_fall",
		"retain_rise_slew", "retain_fall_slew",
	};
	return ids;
}

// Timing checks: the pessimistic requirement is always the largest window,
// whichever analysis the merged view is meant for.
static const pool<std::string> &constraint_ids()
{
	static const pool<std::string> ids = {
		"rise_constraint", "fall_constraint",
		"rise_removal", "fall_removal", "rise_recovery", "fall_recovery",
	};
	return ids;
}

static MergeRule rule_for(const std::string &id)
{
	if (delay_ids().count(id))
		return RULE_DELAY;
	if (constraint_ids().count(id))
		return RULE_MAX;

	// Loads and costs: more is worse
	if (id == "area" || id == "capacitance" ||
	    id == "rise_capacitance" || id == "fall_capacitance" ||
	    id == "rise_capacitance_range" || id == "fall_capacitance_range" ||
	    id == "cell_leakage_power" || id == "leakage_power" ||
	    id == "intrinsic_parasitic" || id == "value" ||
	    id == "rise_power" || id == "fall_power" || id == "power" ||
	    id == "min_capacitance" || id == "min_fanout" || id == "min_transition")
		return RULE_MAX;

	// Design-rule ceilings: the tightest ceiling is the binding one
	if (id == "max_capacitance" || id == "max_fanout" ||
	    id == "max_transition" || id == "max_input_noise_width" ||
	    id == "max_output_noise_width")
		return RULE_MIN;

	// Table axes and template shape must line up or the merge is meaningless
	if (id == "index_1" || id == "index_2" || id == "index_3" ||
	    id == "variable_1" || id == "variable_2" || id == "variable_3")
		return RULE_SAME;

	return RULE_UNKNOWN;
}

static bool parse_number(const std::string &s, double &out)
{
	std::string t = s;
	// strip surrounding quotes and whitespace
	size_t b = t.find_first_not_of(" \t\r\n\"");
	size_t e = t.find_last_not_of(" \t\r\n\"");
	if (b == std::string::npos)
		return false;
	t = t.substr(b, e - b + 1);
	if (t.empty())
		return false;
	const char *p = t.c_str();
	char *end = nullptr;
	out = strtod(p, &end);
	return end != nullptr && *end == '\0';
}

// A stable identity for a child node, so corresponding nodes can be paired up
// across corners. Groups like timing() carry no arguments, so the key folds in
// the sub-attributes that distinguish one arc from another.
static const char *disambiguators[] = {
	"related_pin", "related_pg_pin", "timing_type", "timing_sense",
	"when", "sdf_cond", "mode", "related_output_pin", nullptr
};

// Identity of a node ignoring its contents. Several siblings can share it -
// a pin carries one timing() group per arc - which is what refine_key sorts out.
static std::string node_key(const LibertyAst *n)
{
	std::string key = n->id;

	// For these the arguments are the payload, not the identity, so they
	// must not be part of the key - that is exactly what differs per corner.
	if (n->id == "values" || n->id == "index_1" ||
	    n->id == "index_2" || n->id == "index_3")
		return key;

	key += "(";
	for (size_t i = 0; i < n->args.size(); i++) {
		if (i)
			key += ",";
		key += n->args[i];
	}
	key += ")";
	return key;
}

// Tell sibling nodes apart, using only the attributes every corner actually
// carries. sky130's slow corner annotates a min_pulse_width arc with
// related_output_pin where its fast corner does not; keying on an attribute
// like that would report the same arc as missing from one of them.
static std::string refine_key(const LibertyAst *n, const pool<std::string> &use)
{
	std::string key = node_key(n);
	for (int i = 0; disambiguators[i]; i++) {
		if (!use.count(disambiguators[i]))
			continue;
		const LibertyAst *c = n->find(disambiguators[i]);
		key += std::string("|") + disambiguators[i] + "=" + (c ? c->value : "");
	}
	return key;
}

static std::vector<std::string> split_row(const std::string &row)
{
	std::vector<std::string> out;
	std::string cur;
	for (char c : row) {
		if (c == ',') {
			out.push_back(cur);
			cur.clear();
		} else if (c != '"') {
			cur += c;
		}
	}
	out.push_back(cur);
	return out;
}

static std::string trim(const std::string &s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	size_t e = s.find_last_not_of(" \t\r\n");
	if (b == std::string::npos)
		return "";
	return s.substr(b, e - b + 1);
}

// A lookup table pulled out of a liberty group, as plain numbers.
struct Table {
	std::vector<double> x;		// index_1
	std::vector<double> y;		// index_2, empty for a 1-D table
	std::vector<std::vector<double>> v;	// v[row][col], rows follow x
};

static std::vector<double> parse_axis(const LibertyAst *n)
{
	std::vector<double> out;
	for (auto &arg : n->args)
		for (auto &tok : split_row(arg)) {
			double d;
			if (parse_number(tok, d))
				out.push_back(d);
		}
	return out;
}

// Linear interpolation along one axis, clamped at both ends. Clamping rather
// than extrapolating keeps a resampled corner inside the range it was actually
// characterised over.
static void bracket(const std::vector<double> &axis, double q, size_t &i0, size_t &i1, double &t)
{
	if (axis.size() < 2) {
		i0 = i1 = 0;
		t = 0;
		return;
	}
	if (q <= axis.front()) { i0 = i1 = 0; t = 0; return; }
	if (q >= axis.back()) { i0 = i1 = axis.size() - 1; t = 0; return; }
	size_t k = 1;
	while (k + 1 < axis.size() && axis[k] < q)
		k++;
	i0 = k - 1;
	i1 = k;
	double d = axis[i1] - axis[i0];
	t = d != 0 ? (q - axis[i0]) / d : 0;
}

static double sample(const Table &tab, double qx, double qy)
{
	size_t x0, x1, y0, y1;
	double tx, ty;
	bracket(tab.x, qx, x0, x1, tx);
	if (tab.y.empty()) {
		double a = tab.v[x0][0], b = tab.v[x1][0];
		return a + (b - a) * tx;
	}
	bracket(tab.y, qy, y0, y1, ty);
	double v00 = tab.v[x0][y0], v01 = tab.v[x0][y1];
	double v10 = tab.v[x1][y0], v11 = tab.v[x1][y1];
	double a = v00 + (v01 - v00) * ty;
	double b = v10 + (v11 - v10) * ty;
	return a + (b - a) * tx;
}

static LibertyAst *clone(const LibertyAst *n)
{
	LibertyAst *out = new LibertyAst;
	out->id = n->id;
	out->args = n->args;
	out->value = n->value;
	for (auto c : n->children)
		out->children.push_back(clone(c));
	return out;
}

struct LibertyMerger {
	bool hold_mode;
	std::vector<std::string> names;
	int merged_scalars = 0;
	int merged_tables = 0;
	int merged_entries = 0;
	int header_conflicts = 0;
	int carried_annotations = 0;
	int resampled_tables = 0;
	bool resample = false;

	LibertyMerger(bool hold_mode) : hold_mode(hold_mode) {}

	// true when the candidate is more pessimistic than the incumbent
	bool wins(MergeRule rule, double cand, double best)
	{
		bool want_max = (rule == RULE_MAX) ||
				(rule == RULE_DELAY && !hold_mode);
		return want_max ? cand > best : cand < best;
	}

	// Pick the pessimistic element and return the winner's original text, so
	// the merged file carries the exact digits the corner file carried.
	std::string pick(MergeRule rule, const std::vector<std::string> &vals,
			 const std::string &what)
	{
		double best = 0;
		size_t best_i = 0;
		for (size_t i = 0; i < vals.size(); i++) {
			double v;
			if (!parse_number(vals[i], v))
				log_error("Non-numeric value '%s' where a number is required (%s, corner %s).\n",
					  vals[i].c_str(), what.c_str(), names[i].c_str());
			if (i == 0 || wins(rule, v, best)) {
				best = v;
				best_i = i;
			}
		}
		return trim(vals[best_i]);
	}

	// Returns true when it fully handled the group. Every corner is sampled
	// onto the first corner's axes, so the merged table stays on a grid the
	// library already uses rather than a synthesised one.
	bool merge_table_group(const std::vector<const LibertyAst*> &nodes, LibertyAst *out,
			       MergeRule rule, const std::string &path)
	{
		std::vector<Table> tabs;
		for (auto n : nodes) {
			const LibertyAst *i1 = n->find("index_1");
			const LibertyAst *i2 = n->find("index_2");
			const LibertyAst *vals = n->find("values");
			if (vals == nullptr)
				return false;
			Table t;
			if (i1) t.x = parse_axis(i1);
			if (i2) t.y = parse_axis(i2);
			for (auto &row : vals->args) {
				std::vector<double> r;
				for (auto &tok : split_row(row)) {
					double d;
					if (!parse_number(tok, d))
						log_error("Non-numeric table entry '%s' at %s.\n",
							  tok.c_str(), path.c_str());
					r.push_back(d);
				}
				t.v.push_back(r);
			}
			if (t.x.empty() || t.v.empty())
				return false;
			// A table stored as a single row of x*y entries is reshaped
			// so indexing stays uniform.
			if (t.v.size() == 1 && !t.y.empty() && t.v[0].size() == t.x.size() * t.y.size()) {
				std::vector<std::vector<double>> re;
				for (size_t r = 0; r < t.x.size(); r++)
					re.push_back(std::vector<double>(
						t.v[0].begin() + r * t.y.size(),
						t.v[0].begin() + (r + 1) * t.y.size()));
				t.v = re;
			}
			// A one-dimensional table is written as a single row of
			// index_1-many entries; store it as one entry per row so
			// indexing is uniform with the 2-D case.
			if (t.y.empty() && t.v.size() == 1 && t.v[0].size() == t.x.size()) {
				std::vector<std::vector<double>> re;
				for (double d : t.v[0])
					re.push_back({d});
				t.v = re;
			}
			if (t.v.size() != t.x.size())
				return false;
			if (!t.y.empty())
				for (auto &r : t.v)
					if (r.size() != t.y.size())
						return false;
			tabs.push_back(t);
		}

		bool axes_agree = true;
		for (size_t i = 1; i < tabs.size(); i++)
			if (tabs[i].x != tabs[0].x || tabs[i].y != tabs[0].y)
				axes_agree = false;
		if (axes_agree)
			return false;	// nothing to reconcile, use the exact-text path

		const Table &ref = tabs[0];
		bool one_dim = ref.y.empty();
		std::vector<std::string> rows;
		std::string flat;
		for (size_t r = 0; r < ref.x.size(); r++) {
			std::string row;
			size_t cols = one_dim ? 1 : ref.y.size();
			for (size_t c = 0; c < cols; c++) {
				double qx = ref.x[r];
				double qy = one_dim ? 0 : ref.y[c];
				double best = 0;
				for (size_t k = 0; k < tabs.size(); k++) {
					double v = sample(tabs[k], qx, qy);
					if (k == 0 || wins(rule, v, best))
						best = v;
				}
				char buf[32];
				snprintf(buf, sizeof(buf), "%.10g", best);
				if (c)
					row += ", ";
				row += buf;
				merged_entries++;
			}
			if (one_dim) {
				// Put it back the way liberty writes a 1-D table
				if (r)
					flat += ", ";
				flat += row;
			} else {
				rows.push_back("\"" + row + "\"");
			}
		}
		if (one_dim)
			rows.push_back("\"" + flat + "\"");

		// Rebuild the group on the reference axes
		for (auto c : nodes[0]->children) {
			if (c->id == "values") {
				LibertyAst *v = new LibertyAst;
				v->id = "values";
				v->args = rows;
				out->children.push_back(v);
			} else {
				out->children.push_back(clone(c));
			}
		}
		merged_tables++;
		resampled_tables++;
		return true;
	}

	void merge_values(const std::vector<const LibertyAst*> &nodes, LibertyAst *out,
			  MergeRule rule, const std::string &path)
	{
		size_t rows = nodes[0]->args.size();
		for (auto n : nodes)
			if (n->args.size() != rows)
				log_error("Table shape differs across corners at %s (%zu vs %zu rows).\n",
					  path.c_str(), rows, n->args.size());

		for (size_t r = 0; r < rows; r++) {
			std::vector<std::vector<std::string>> cells;
			for (auto n : nodes)
				cells.push_back(split_row(n->args[r]));
			size_t cols = cells[0].size();
			for (auto &c : cells)
				if (c.size() != cols)
					log_error("Table row width differs across corners at %s.\n",
						  path.c_str());

			std::string row;
			for (size_t c = 0; c < cols; c++) {
				std::vector<std::string> vals;
				for (auto &cell : cells)
					vals.push_back(cell[c]);
				if (c)
					row += ", ";
				row += pick(rule, vals, path);
				merged_entries++;
			}
			out->args.push_back("\"" + row + "\"");
		}
		merged_tables++;
	}

	// nodes[] are the same node as seen in each corner, in input order.
	LibertyAst *merge(const std::vector<const LibertyAst*> &nodes,
			  const std::string &path, const std::string &parent_id,
			  bool in_cell)
	{
		LibertyAst *out = new LibertyAst;
		out->id = nodes[0]->id;
		out->args = nodes[0]->args;
		out->value = nodes[0]->value;

		bool is_cell = (out->id == "cell");
		bool inside_cell = in_cell || is_cell;

		// A lookup-table group is merged as a whole: under -resample the
		// axes have to be reconciled before the values can be compared.
		if (resample && nodes[0]->find("values") != nullptr &&
		    rule_for(out->id) != RULE_UNKNOWN && rule_for(out->id) != RULE_SAME) {
			if (merge_table_group(nodes, out, rule_for(out->id), path))
				return out;
		}

		// --- leaf data -------------------------------------------------
		if (out->id == "values" || out->id == "index_1" ||
		    out->id == "index_2" || out->id == "index_3") {
			MergeRule rule = out->id == "values" ? rule_for(parent_id) : RULE_SAME;
			if (out->id == "values" && rule == RULE_UNKNOWN) {
				// Inside a cell an unclassified table is timing data we do
				// not know how to combine, and guessing would corrupt every
				// downstream decision. In the library header it is
				// characterisation metadata such as a driver waveform.
				if (inside_cell)
					log_error("Refusing to merge table '%s' inside unclassified group '%s' at %s.\n",
						  out->id.c_str(), parent_id.c_str(), path.c_str());
				bool differs = false;
				for (size_t i = 1; i < nodes.size(); i++)
					if (nodes[i]->args != nodes[0]->args)
						differs = true;
				if (differs) {
					log_warning("Library header table '%s' at %s differs across corners; "
						    "keeping %s's.\n", parent_id.c_str(), path.c_str(),
						    names[0].c_str());
					header_conflicts++;
				}
				return out;	// args already copied from corner 0
			}
			if (rule == RULE_SAME) {
				for (size_t i = 1; i < nodes.size(); i++)
					if (nodes[i]->args != nodes[0]->args) {
						// Template defaults may legitimately differ per
						// corner once resampling reconciles the real
						// per-table axes. Inside a cell they may not:
						// that would silently compare values sampled at
						// different operating points.
						if (resample && !inside_cell) {
							log_warning("Template axis %s at %s differs across corners; "
								    "keeping %s's grid.\n", out->id.c_str(),
								    path.c_str(), names[0].c_str());
							header_conflicts++;
							break;
						}
						log_error("Table axis %s differs between corner %s and %s at %s; "
							  "the corners are not on a common template%s.\n",
							  out->id.c_str(), names[0].c_str(),
							  names[i].c_str(), path.c_str(),
							  resample ? "" : " (see -resample)");
					}
			} else {
				out->args.clear();
				merge_values(nodes, out, rule, path);
			}
		} else if (!out->value.empty() && nodes[0]->children.empty()) {
			bool differs = false;
			for (size_t i = 1; i < nodes.size(); i++)
				if (nodes[i]->value != nodes[0]->value)
					differs = true;
			if (differs) {
				MergeRule rule = rule_for(out->id);
				double dummy;
				bool numeric = parse_number(nodes[0]->value, dummy);
				if (rule != RULE_UNKNOWN && rule != RULE_SAME && numeric) {
					std::vector<std::string> vals;
					for (auto n : nodes)
						vals.push_back(n->value);
					out->value = pick(rule, vals, path);
					merged_scalars++;
				} else if (inside_cell) {
					log_error("Attribute '%s' at %s differs across corners (%s vs %s) and has "
						  "no worst-case rule; refusing to guess.\n",
						  out->id.c_str(), path.c_str(),
						  nodes[0]->value.c_str(), nodes[1]->value.c_str());
				} else {
					// Library header: operating conditions and
					// nominal PVT legitimately differ. Keep the
					// first corner's, but say so.
					log_warning("Library header attribute '%s' at %s differs across corners "
						    "(keeping %s's value '%s').\n",
						    out->id.c_str(), path.c_str(),
						    names[0].c_str(), nodes[0]->value.c_str());
					header_conflicts++;
				}
			}
		}

		// --- children --------------------------------------------------
		// Pair each corner's children up. Nodes are matched on identity
		// first; sibling nodes that share an identity (the several timing()
		// arcs on one pin) are then told apart using only the attributes
		// every corner carries for them.
		std::vector<std::string> order;
		dict<std::string, std::vector<std::vector<const LibertyAst*>>> bybase;
		for (size_t i = 0; i < nodes.size(); i++) {
			for (auto c : nodes[i]->children) {
				std::string b = node_key(c);
				if (!bybase.count(b)) {
					order.push_back(b);
					bybase[b].resize(nodes.size());
				}
				bybase[b][i].push_back(c);
			}
		}

		// What this pass merges is the NLDM view - the delay, transition,
		// constraint and power tables a mapper or an STA engine reads. Two
		// things sit outside it: plain annotation references, and the
		// alternate characterisation models (CCS noise, receiver
		// capacitance). Corner files are genuinely inconsistent about
		// carrying those - sky130's fast corner tags pins with
		// input_voltage and ships ccsn_first_stage where its slow corner
		// ships neither - so an absent one is not evidence of a mismatched
		// library. They are carried over from the first corner that has
		// them and counted, never silently blended.
		static const pool<std::string> annotations = {
			"input_voltage()", "output_voltage()", "driver_waveform()",
			"driver_waveform_rise()", "driver_waveform_fall()",
			"ccsn_first_stage()", "ccsn_last_stage()",
			"receiver_capacitance()", "receiver_capacitance1_rise()",
			"receiver_capacitance1_fall()", "receiver_capacitance2_rise()",
			"receiver_capacitance2_fall()",
			"related_output_pin()", "sim_opt()",
		};
		auto is_structural = [&](const std::string &k) {
			if (annotations.count(k))
				return false;
			return inside_cell || k.compare(0, 5, "cell(") == 0;
		};

		auto missing = [&](const std::string &b, size_t which_has, size_t which_lacks) {
			if (is_structural(b))
				log_error("%s exists in corner %s but not in %s (at %s); "
					  "refusing to merge a partial library.\n",
					  b.c_str(), names[which_has].c_str(),
					  names[which_lacks].c_str(), path.c_str());
			log_warning("%s at %s is not present in every corner; keeping the first "
				    "declaration.\n", b.c_str(), path.c_str());
			if (inside_cell)
				carried_annotations++;
			else
				header_conflicts++;
		};

		for (auto &b : order) {
			auto &per_corner = bybase.at(b);

			// Present in some corners only
			int have = -1;
			bool all_present = true;
			for (size_t i = 0; i < nodes.size(); i++) {
				if (per_corner[i].empty())
					all_present = false;
				else if (have < 0)
					have = int(i);
			}
			if (!all_present) {
				for (size_t i = 0; i < nodes.size(); i++)
					if (per_corner[i].empty())
						missing(b, size_t(have), i);
				out->children.push_back(clone(per_corner[have][0]));
				continue;
			}

			// The common case: one such node per corner
			bool unique = true;
			for (auto &v : per_corner)
				if (v.size() != 1)
					unique = false;
			if (unique) {
				std::vector<const LibertyAst*> g;
				for (auto &v : per_corner)
					g.push_back(v[0]);
				out->children.push_back(merge(g, path + "/" + b, out->id, inside_cell));
				continue;
			}

			// Siblings sharing an identity: disambiguate on the attributes
			// that every one of them carries.
			// Use an attribute only when every corner annotates the same
			// number of these siblings with it. Carrying a 'when' on one
			// of two internal_power groups in every corner tells the
			// conditional arc from the default one and is usable;
			// carrying related_output_pin in the slow corner only says
			// nothing about which arc is which, and keying on it would
			// report an arc as missing that is plainly there.
			pool<std::string> use;
			for (int d = 0; disambiguators[d]; d++) {
				int ref = -1;
				bool consistent = true;
				for (auto &v : per_corner) {
					int cnt = 0;
					for (auto n : v)
						if (n->find(disambiguators[d]) != nullptr)
							cnt++;
					if (ref < 0)
						ref = cnt;
					else if (cnt != ref)
						consistent = false;
				}
				if (consistent && ref > 0)
					use.insert(disambiguators[d]);
			}

			std::vector<std::string> suborder;
			dict<std::string, std::vector<const LibertyAst*>> sub;
			for (size_t i = 0; i < nodes.size(); i++)
				for (auto n : per_corner[i]) {
					std::string k = refine_key(n, use);
					if (!sub.count(k))
						suborder.push_back(k);
					sub[k].push_back(n);
				}
			for (auto &k : suborder) {
				auto &g = sub.at(k);
				if (g.size() != nodes.size()) {
					if (is_structural(b))
						log_error("%s does not appear once per corner at %s "
							  "(%zu of %zu); refusing to merge a partial "
							  "library.\n", k.c_str(), path.c_str(),
							  g.size(), nodes.size());
					log_warning("%s does not appear once per corner at %s; "
						    "keeping the first.\n", k.c_str(), path.c_str());
					if (inside_cell)
						carried_annotations++;
					else
						header_conflicts++;
					out->children.push_back(clone(g[0]));
					continue;
				}
				out->children.push_back(merge(g, path + "/" + k, out->id, inside_cell));
			}
		}

		return out;
	}
};

static void emit(FILE *f, const LibertyAst *n, const std::string &indent)
{
	fprintf(f, "%s%s", indent.c_str(), n->id.c_str());
	if (!n->args.empty() || !n->children.empty()) {
		fprintf(f, "(");
		for (size_t i = 0; i < n->args.size(); i++)
			fprintf(f, "%s%s", i > 0 ? ", " : "", n->args[i].c_str());
		fprintf(f, ")");
	}
	if (!n->value.empty())
		fprintf(f, " : %s", n->value.c_str());
	if (!n->children.empty()) {
		fprintf(f, " {\n");
		for (auto c : n->children)
			emit(f, c, indent + "  ");
		fprintf(f, "%s}\n", indent.c_str());
	} else
		fprintf(f, " ;\n");
}

struct MergeLibertyPass : public Pass {
	MergeLibertyPass() : Pass("merge_liberty", "merge liberty corners into a worst-case view") {}
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    merge_liberty [options] -o <output> <liberty> <liberty> [...]\n");
		log("\n");
		log("Combine several liberty files describing the same cell library at different\n");
		log("PVT corners into one synthetic worst-case library, so that mapping and cell\n");
		log("selection can be done against every corner at once instead of optimistically\n");
		log("against whichever single corner was handed to 'abc' or 'dfflibmap'.\n");
		log("\n");
		log("Values are combined per attribute, and the winning corner's original text is\n");
		log("emitted verbatim so no precision is invented:\n");
		log("\n");
		log("  - propagation delays and output slews take the slowest corner (-mode setup,\n");
		log("    the default) or the fastest corner (-mode hold)\n");
		log("  - setup/hold/recovery/removal constraints take the largest window\n");
		log("  - areas, pin loads and leakage take the largest value\n");
		log("  - max_transition / max_capacitance / max_fanout take the tightest ceiling\n");
		log("\n");
		log("The merge is refused rather than approximated when the inputs do not describe\n");
		log("the same library: a cell, pin or timing arc present in one corner and absent\n");
		log("in another, a lookup table on a different axis, or a numeric attribute inside\n");
		log("a cell with no worst-case rule, are all hard errors. Library header attributes\n");
		log("that legitimately vary by corner (nominal voltage, temperature, operating\n");
		log("conditions) are taken from the first input and reported as warnings.\n");
		log("\n");
		log("    -mode setup|hold\n");
		log("        Which analysis the merged view is meant to be pessimistic for.\n");
		log("        Default is 'setup'.\n");
		log("\n");
		log("    -resample\n");
		log("        Corner files are often characterised on different slew/load\n");
		log("        grids, which makes an element-by-element comparison meaningless;\n");
		log("        without this option such a library is refused rather than\n");
		log("        silently mis-merged. With it, every corner is bilinearly\n");
		log("        interpolated onto the first input's axes before being compared,\n");
		log("        clamping at the edges rather than extrapolating beyond what was\n");
		log("        actually characterised. Resampled entries are recomputed, so\n");
		log("        they no longer carry the original file's exact digits.\n");
		log("\n");
		log("    -o <output>\n");
		log("        Write the merged library here. Required.\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing MERGE_LIBERTY pass (merge liberty corners).\n");

		std::string output;
		bool hold_mode = false;
		bool resample = false;
		std::vector<std::string> inputs;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-o" && argidx+1 < args.size()) {
				output = args[++argidx];
				continue;
			}
			if (args[argidx] == "-resample") {
				resample = true;
				continue;
			}
			if (args[argidx] == "-mode" && argidx+1 < args.size()) {
				std::string m = args[++argidx];
				if (m == "hold")
					hold_mode = true;
				else if (m == "setup")
					hold_mode = false;
				else
					log_cmd_error("-mode must be 'setup' or 'hold', not '%s'\n", m.c_str());
				continue;
			}
			break;
		}
		for (; argidx < args.size(); argidx++)
			append_globbed(inputs, args[argidx]);

		if (output.empty())
			log_cmd_error("Missing -o <output>.\n");
		if (inputs.size() < 2)
			log_cmd_error("Need at least two liberty files to merge, got %zu.\n", inputs.size());

		std::vector<std::shared_ptr<const LibertyAst>> asts;
		for (auto &path : inputs) {
			log("Reading corner `%s'.\n", path.c_str());
			std::istream *f = uncompressed(path);
			LibertyParser p(*f, LibertyParser::RetainQuotes{});
			asts.push_back(p.shared_ast);
			delete f;
		}

		LibertyMerger merger(hold_mode);
		merger.names = inputs;
		merger.resample = resample;

		std::vector<const LibertyAst*> roots;
		for (auto &a : asts)
			roots.push_back(a.get());
		for (auto r : roots)
			if (r->id != "library")
				log_error("Expected a 'library' group at the top of every input, got '%s'.\n",
					  r->id.c_str());

		LibertyAst *merged = merger.merge(roots, "/library", "", false);

		FILE *f = fopen(output.c_str(), "w");
		if (f == nullptr)
			log_error("Can't open output file `%s' for writing.\n", output.c_str());
		emit(f, merged, "");
		fclose(f);
		delete merged;

		log("Merged %zu corners for a %s view: %d scalar attributes, %d tables (%d entries).\n",
		    inputs.size(), hold_mode ? "hold" : "setup",
		    merger.merged_scalars, merger.merged_tables, merger.merged_entries);
		if (merger.resampled_tables)
			log("%d tables were characterised on differing axes and were resampled onto "
			    "`%s'\'s grid.\n", merger.resampled_tables, inputs[0].c_str());
		if (merger.header_conflicts)
			log("%d library header attributes differed and were taken from `%s'.\n",
			    merger.header_conflicts, inputs[0].c_str());
		if (merger.carried_annotations)
			log("%d annotations and alternate-model groups were not present in every "
			    "corner and were carried over from `%s' (these are outside the merged "
			    "NLDM view).\n", merger.carried_annotations, inputs[0].c_str());
		log("Wrote `%s'.\n", output.c_str());
	}
} MergeLibertyPass;

PRIVATE_NAMESPACE_END
