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
#include <functional>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Per-signal switching activity, in the terms the formats agree on.
struct Activity {
	// 0->1 and 1->0 transitions, summed over the signal's bits
	int64_t toggles = 0;
	// time spent at 1 and at 0, summed over the bits
	double time_one = 0;
	double time_zero = 0;
	int width = 1;
};

static std::string join_scope(const std::vector<std::string> &scope, const std::string &leaf)
{
	std::string s;
	for (auto &p : scope) {
		s += p;
		s += ".";
	}
	s += leaf;
	return s;
}

// ---------------------------------------------------------------------------
// VCD
// ---------------------------------------------------------------------------

struct VcdReader {
	dict<std::string, Activity> result;	// hierarchical name -> activity

	struct Var {
		std::vector<std::string> names;	// one id can alias several signals
		int width;
		// per-bit last value: 0, 1, or -1 for unknown
		std::vector<int> last;
	};

	void read(std::istream &f, const std::string &fname)
	{
		dict<std::string, Var> vars;
		std::vector<std::string> scope;
		std::string tok;
		bool in_defs = true;
		double now = 0, last_time = 0;
		// per-var accumulators, keyed the same as vars
		dict<std::string, Activity> acc;

		auto next = [&]() -> bool { return bool(f >> tok); };

		while (in_defs && next()) {
			if (tok == "$scope") {
				std::string type;
				f >> type >> tok;
				scope.push_back(tok);
				while (f >> tok && tok != "$end") {}
			} else if (tok == "$upscope") {
				if (!scope.empty())
					scope.pop_back();
				while (f >> tok && tok != "$end") {}
			} else if (tok == "$var") {
				std::string type, id, name;
				int width;
				f >> type >> width >> id >> name;
				std::string rest;
				// the index suffix arrives as its own token
				while (f >> rest && rest != "$end")
					name += rest;
				// '$var wire 4 # cnt [3:0] $end' names the same wire as
				// 'cnt'; the range is already carried by the width. A
				// single-bit select is folded onto its parent wire too,
				// which matches how the counts are summed over bits.
				size_t br = name.find('[');
				if (br != std::string::npos && name.back() == ']')
					name = name.substr(0, br);
				auto &v = vars[id];
				v.width = width;
				if (v.last.empty())
					v.last.assign(width, -1);
				v.names.push_back(join_scope(scope, name));
				if (!acc.count(id)) {
					acc[id] = Activity();
					acc[id].width = width;
				}
			} else if (tok == "$enddefinitions") {
				while (f >> tok && tok != "$end") {}
				in_defs = false;
			} else if (tok == "$timescale" || tok == "$date" ||
				   tok == "$version" || tok == "$comment") {
				while (f >> tok && tok != "$end") {}
			}
		}

		auto flush_time = [&](double upto) {
			double dt = upto - last_time;
			if (dt <= 0)
				return;
			for (auto &it : vars) {
				auto &a = acc.at(it.first);
				for (int b : it.second.last) {
					if (b == 1)
						a.time_one += dt;
					else if (b == 0)
						a.time_zero += dt;
				}
			}
			last_time = upto;
		};

		auto set_bits = [&](const std::string &id, const std::vector<int> &bits) {
			auto vit = vars.find(id);
			if (vit == vars.end())
				return;
			auto &v = vit->second;
			auto &a = acc.at(id);
			for (size_t i = 0; i < v.last.size() && i < bits.size(); i++) {
				int nv = bits[i];
				int ov = v.last[i];
				// Only a real 0<->1 crossing counts. Writing the same
				// value again, or passing through x/z, is not a toggle.
				if (ov >= 0 && nv >= 0 && ov != nv)
					a.toggles++;
				v.last[i] = nv;
			}
		};

		while (next()) {
			if (tok[0] == '#') {
				double t = atof(tok.c_str() + 1);
				flush_time(t);
				now = t;
				(void)now;
			} else if (tok[0] == 'b' || tok[0] == 'B') {
				std::string bits = tok.substr(1);
				std::string id;
				if (!(f >> id))
					break;
				auto vit = vars.find(id);
				if (vit == vars.end())
					continue;
				int w = vit->second.width;
				// VCD prints the MSB first and drops leading zeroes
				std::vector<int> b(w, 0);
				for (int i = 0; i < GetSize(bits); i++) {
					char c = bits[GetSize(bits) - 1 - i];
					if (i >= w)
						break;
					b[i] = (c == '1') ? 1 : (c == '0') ? 0 : -1;
				}
				// an unspecified upper part repeats the MSB per the spec
				if (GetSize(bits) < w) {
					int fill = b[GetSize(bits) - 1];
					if (bits[0] != '1')
						for (int i = GetSize(bits); i < w; i++)
							b[i] = fill;
				}
				set_bits(id, b);
			} else if (tok[0] == '0' || tok[0] == '1' ||
				   tok[0] == 'x' || tok[0] == 'X' ||
				   tok[0] == 'z' || tok[0] == 'Z') {
				int val = tok[0] == '1' ? 1 : tok[0] == '0' ? 0 : -1;
				std::string id = tok.substr(1);
				set_bits(id, std::vector<int>(1, val));
			} else if (tok[0] == 'r' || tok[0] == 'R') {
				std::string id;
				f >> id;
			}
		}
		flush_time(last_time);

		for (auto &it : vars)
			for (auto &nm : it.second.names) {
				auto &dst = result[nm];
				auto &src = acc.at(it.first);
				dst.toggles += src.toggles;
				dst.time_one += src.time_one;
				dst.time_zero += src.time_zero;
				dst.width = src.width;
			}
		log("Read %d signals from VCD `%s'.\n", GetSize(result), fname.c_str());
	}
};

// ---------------------------------------------------------------------------
// SAIF
// ---------------------------------------------------------------------------

struct SaifReader {
	dict<std::string, Activity> result;

	// SAIF is a parenthesised s-expression; tokenise on parens and whitespace
	static std::vector<std::string> lex(std::istream &f)
	{
		std::vector<std::string> out;
		std::string cur;
		int c;
		while ((c = f.get()) != EOF) {
			if (c == '(' || c == ')') {
				if (!cur.empty()) { out.push_back(cur); cur.clear(); }
				out.push_back(std::string(1, char(c)));
			} else if (isspace(c)) {
				if (!cur.empty()) { out.push_back(cur); cur.clear(); }
			} else if (c == '"') {
				while ((c = f.get()) != EOF && c != '"')
					cur += char(c);
			} else {
				cur += char(c);
			}
		}
		if (!cur.empty())
			out.push_back(cur);
		return out;
	}

	void read(std::istream &f, const std::string &fname)
	{
		auto tok = lex(f);
		std::vector<std::string> scope;
		size_t i = 0;

		std::function<void()> parse_group = [&]() {
			// positioned just after '('
			if (i >= tok.size())
				return;
			std::string kind = tok[i++];
			if (kind == "INSTANCE") {
				if (i < tok.size() && tok[i] != "(" && tok[i] != ")")
					scope.push_back(tok[i++]);
				else
					scope.push_back("");
				while (i < tok.size() && tok[i] != ")") {
					if (tok[i] == "(") { i++; parse_group(); }
					else i++;
				}
				if (i < tok.size()) i++;	// ')'
				scope.pop_back();
				return;
			}
			if (kind == "NET" || kind == "PORT") {
				while (i < tok.size() && tok[i] != ")") {
					if (tok[i] == "(") {
						i++;
						// (signame (T0 n) (T1 n) (TX n) (TC n))
						if (i >= tok.size()) break;
						std::string name = tok[i++];
						Activity a;
						while (i < tok.size() && tok[i] != ")") {
							if (tok[i] == "(") {
								i++;
								std::string key = i < tok.size() ? tok[i++] : "";
								std::string val = i < tok.size() ? tok[i++] : "0";
								if (key == "T0") a.time_zero = atof(val.c_str());
								else if (key == "T1") a.time_one = atof(val.c_str());
								else if (key == "TC") a.toggles = atoll(val.c_str());
								while (i < tok.size() && tok[i] != ")") i++;
								if (i < tok.size()) i++;
							} else i++;
						}
						if (i < tok.size()) i++;
						result[join_scope(scope, name)] = a;
					} else i++;
				}
				if (i < tok.size()) i++;
				return;
			}
			// Any other group - SAIFILE, and the header groups inside it -
			// is a container: descend into it rather than skipping it, or
			// every INSTANCE nested below would be missed.
			while (i < tok.size() && tok[i] != ")") {
				if (tok[i] == "(") { i++; parse_group(); }
				else i++;
			}
			if (i < tok.size()) i++;
		};

		while (i < tok.size()) {
			if (tok[i] == "(") { i++; parse_group(); }
			else i++;
		}
		log("Read %d signals from SAIF `%s'.\n", GetSize(result), fname.c_str());
	}
};

// ---------------------------------------------------------------------------

struct ReadActivityPass : public Pass {
	ReadActivityPass() : Pass("read_activity", "annotate switching activity from SAIF or VCD") {}
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    read_activity [options] <filename>\n");
		log("\n");
		log("Read per-signal switching activity from a simulation and attach it to the\n");
		log("matching wires, so that later passes can tell a net that toggles every cycle\n");
		log("from one that almost never moves. Yosys otherwise has no notion of activity at\n");
		log("all, which is what stops power being optimised for rather than merely reported.\n");
		log("\n");
		log("Each matched wire gets:\n");
		log("\n");
		log("    toggle_count          0<->1 transitions, summed over the wire's bits\n");
		log("    static_probability    fraction of the run the bits were at 1\n");
		log("    toggle_rate           transitions per unit time\n");
		log("\n");
		log("Writing the same value again, or passing through x or z, is not counted as a\n");
		log("transition - only a real 0<->1 crossing is.\n");
		log("\n");
		log("    -saif | -vcd\n");
		log("        Input format. Guessed from the file extension when not given.\n");
		log("\n");
		log("    -scope <path>\n");
		log("        Dotted path in the file that corresponds to the selected top module.\n");
		log("        Signals outside it are ignored. Defaults to the file's own top scope.\n");
		log("\n");
		log("    -unmatched <n>\n");
		log("        Tolerate at most <n> percent of the file's signals failing to match a\n");
		log("        wire before the command gives up. Defaults to 100, i.e. report only.\n");
		log("        A file that matches nothing at all is always an error: silently\n");
		log("        annotating no activity would leave later passes quietly optimising\n");
		log("        against a design they think never switches.\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing READ_ACTIVITY pass (annotate switching activity).\n");

		std::string filename, scope_prefix;
		bool saif = false, vcd = false, have_fmt = false;
		int unmatched_pct = 100;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-saif") { saif = true; have_fmt = true; continue; }
			if (args[argidx] == "-vcd") { vcd = true; have_fmt = true; continue; }
			if (args[argidx] == "-scope" && argidx+1 < args.size()) {
				scope_prefix = args[++argidx];
				continue;
			}
			if (args[argidx] == "-unmatched" && argidx+1 < args.size()) {
				unmatched_pct = atoi(args[++argidx].c_str());
				continue;
			}
			break;
		}
		if (argidx < args.size())
			filename = args[argidx++];
		extra_args(args, argidx, design);

		if (filename.empty())
			log_cmd_error("Missing input filename.\n");
		if (!have_fmt) {
			if (filename.size() > 5 && filename.compare(filename.size()-5, 5, ".saif") == 0)
				saif = true;
			else if (filename.size() > 4 && filename.compare(filename.size()-4, 4, ".vcd") == 0)
				vcd = true;
			else
				log_cmd_error("Cannot tell the format of `%s' from its name; pass -saif or -vcd.\n",
					      filename.c_str());
		}
		if (saif && vcd)
			log_cmd_error("-saif and -vcd are mutually exclusive.\n");

		dict<std::string, Activity> act;
		{
			std::istream *f = uncompressed(filename);
			if (saif) {
				SaifReader r;
				r.read(*f, filename);
				act = r.result;
			} else {
				VcdReader r;
				r.read(*f, filename);
				act = r.result;
			}
			delete f;
		}
		if (act.empty())
			log_error("No signals found in `%s'.\n", filename.c_str());

		// Strip the scope the caller says corresponds to the selected modules.
		// Without one, drop the file's own outermost scope, which is the usual
		// testbench wrapper.
		std::string prefix = scope_prefix;
		if (prefix.empty()) {
			std::string common;
			bool first = true;
			for (auto &it : act) {
				size_t dot = it.first.find('.');
				if (dot == std::string::npos) { common.clear(); break; }
				std::string head = it.first.substr(0, dot);
				if (first) { common = head; first = false; }
				else if (common != head) { common.clear(); break; }
			}
			prefix = common;
		}
		if (!prefix.empty())
			prefix += ".";

		dict<std::string, const Activity*> byname;
		for (auto &it : act) {
			std::string n = it.first;
			if (!prefix.empty()) {
				if (n.compare(0, prefix.size(), prefix) != 0)
					continue;
				n = n.substr(prefix.size());
			}
			byname[n] = &it.second;
		}

		IdString A_toggle_count = RTLIL::escape_id("toggle_count");
		IdString A_static_probability = RTLIL::escape_id("static_probability");
		IdString A_toggle_rate = RTLIL::escape_id("toggle_rate");

		int matched = 0, annotated_bits = 0;
		pool<std::string> used;
		for (auto module : design->selected_modules()) {
			for (auto wire : module->selected_wires()) {
				std::string wn = RTLIL::unescape_id(wire->name);
				auto it = byname.find(wn);
				if (it == byname.end())
					continue;
				const Activity &a = *it->second;
				double total = a.time_one + a.time_zero;
				wire->attributes[A_toggle_count] = Const(int(a.toggles));
				wire->attributes[A_static_probability] =
					Const(stringf("%.6g", total > 0 ? a.time_one / total : 0.0));
				wire->attributes[A_toggle_rate] =
					Const(stringf("%.6g", total > 0 ? a.toggles / total : 0.0));
				matched++;
				annotated_bits += wire->width;
				used.insert(wn);
			}
		}

		int total_signals = GetSize(byname);
		int unmatched = total_signals - GetSize(used);
		log("Annotated %d wires (%d bits); %d of %d signals in the file matched no wire.\n",
		    matched, annotated_bits, unmatched, total_signals);

		if (matched == 0)
			log_error("`%s' matched no wire in the selected modules. Check -scope: annotating "
				  "nothing would leave later passes treating a live design as static.\n",
				  filename.c_str());
		if (total_signals > 0 && unmatched * 100 > unmatched_pct * total_signals)
			log_error("%d of %d signals (%d%%) matched no wire, over the %d%% allowed by "
				  "-unmatched.\n", unmatched, total_signals,
				  unmatched * 100 / total_signals, unmatched_pct);
	}
} ReadActivityPass;

PRIVATE_NAMESPACE_END
