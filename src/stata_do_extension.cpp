#define DUCKDB_EXTENSION_MAIN

#include "stata_do_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/binder.hpp"

#include <fstream>
#include <regex>

namespace duckdb {

// Helper: Trim that returns a new string
static string Trim(const string &s) {
	string result = s;
	StringUtil::Trim(result);
	return result;
}

//===--------------------------------------------------------------------===//
// Stata Command Tokenizer
//===--------------------------------------------------------------------===//

static const vector<string> STATA_COMMANDS = {"use",       "list",       "clear",   "keep",    "drop",
                                              "generate",  "replace",    "rename",  "sort",    "order",
                                              "egen",      "collapse",   "count",   "describe", "summarize",
                                              "tabulate",  "head",       "tail",    "save",    "append",
                                              "mvencode",  "reshape",    "do",      "label",   "codebook",
                                              "duplicates", "expand",    "export",  "import",
                                              "merge",     "tempfile",  "preserve", "restore",
                                              "xtset",     "tsset",    "bysort",  "by"};

// Command classification for do-file execution
// Transformation: modifies the CTE chain state
// Terminal: materializes the chain, returns the data frame
// Side-effect: returns a derived result (count, stats, freq table, file write)
static bool IsTransformationCommand(const string &command) {
	static const vector<string> TRANSFORMATION = {"use", "clear", "do", "keep", "drop", "generate", "replace",
	                                              "rename", "sort", "order", "egen", "collapse", "mvencode",
	                                              "reshape", "append", "label", "duplicates", "expand", "import",
	                                              "merge", "tempfile", "preserve", "restore",
	                                              "xtset", "tsset", "bysort", "by"};
	for (auto &cmd : TRANSFORMATION) {
		if (command == cmd) {
			return true;
		}
	}
	return false;
}

static bool IsStataCommand(const string &query, string &command_out) {
	string trimmed = Trim(query);
	if (!trimmed.empty() && trimmed.back() == ';') {
		trimmed.pop_back();
		trimmed = Trim(trimmed);
	}
	string lower = StringUtil::Lower(trimmed);
	for (auto &cmd : STATA_COMMANDS) {
		if (StringUtil::StartsWith(lower, cmd)) {
			if (lower.size() == cmd.size() || lower[cmd.size()] == ' ' || lower[cmd.size()] == ',') {
				command_out = cmd;
				return true;
			}
		}
	}
	return false;
}

struct StataCommand {
	string command;
	string arguments;
	string condition;
	string options;
	// bysort context (set when command has bysort/by prefix)
	string bysort_partition;  // PARTITION BY vars (comma-separated)
	string bysort_order;      // ORDER BY vars (comma-separated), empty if none
};

static StataCommand TokenizeCommand(const string &query) {
	StataCommand result;
	string trimmed = Trim(query);
	if (!trimmed.empty() && trimmed.back() == ';') {
		trimmed.pop_back();
		trimmed = Trim(trimmed);
	}

	string lower = StringUtil::Lower(trimmed);
	for (auto &cmd : STATA_COMMANDS) {
		if (StringUtil::StartsWith(lower, cmd) &&
		    (lower.size() == cmd.size() || lower[cmd.size()] == ' ' || lower[cmd.size()] == ',')) {
			result.command = cmd;
			trimmed = Trim(trimmed.substr(cmd.size()));
			break;
		}
	}

	// Split off options after comma (not inside quotes or parens)
	int paren_depth = 0;
	bool in_quotes = false;
	idx_t comma_pos = string::npos;
	for (idx_t i = 0; i < trimmed.size(); i++) {
		char c = trimmed[i];
		if (c == '"') {
			in_quotes = !in_quotes;
		} else if (!in_quotes) {
			if (c == '(') {
				paren_depth++;
			} else if (c == ')') {
				paren_depth--;
			} else if (c == ',' && paren_depth == 0) {
				comma_pos = i;
				break;
			}
		}
	}

	string before_comma = trimmed;
	if (comma_pos != string::npos) {
		before_comma = Trim(trimmed.substr(0, comma_pos));
		result.options = Trim(trimmed.substr(comma_pos + 1));
	}

	// Split on "if " keyword (at start or after space, not inside quotes)
	string lower_bc = StringUtil::Lower(before_comma);
	idx_t if_pos = string::npos;
	idx_t cond_start = string::npos;

	// Check if it starts with "if "
	if (StringUtil::StartsWith(lower_bc, "if ")) {
		if_pos = 0;
		cond_start = 3;
	} else {
		// Look for " if " in the middle
		idx_t search_start = 0;
		while (true) {
			idx_t pos = lower_bc.find(" if ", search_start);
			if (pos == string::npos) {
				break;
			}
			bool inside_quotes = false;
			for (idx_t i = 0; i < pos; i++) {
				if (before_comma[i] == '"') {
					inside_quotes = !inside_quotes;
				}
			}
			if (!inside_quotes) {
				if_pos = pos;
				cond_start = pos + 4;
				break;
			}
			search_start = pos + 4;
		}
	}

	if (if_pos != string::npos) {
		result.arguments = Trim(before_comma.substr(0, if_pos));
		result.condition = Trim(before_comma.substr(cond_start));
	} else {
		result.arguments = before_comma;
	}

	return result;
}

//===--------------------------------------------------------------------===//
// SQL Generation Helpers
//===--------------------------------------------------------------------===//

static string ExtractQuotedString(const string &s) {
	string trimmed = Trim(s);
	if (trimmed.size() >= 2) {
		if ((trimmed.front() == '"' && trimmed.back() == '"') ||
		    (trimmed.front() == '\'' && trimmed.back() == '\'')) {
			return trimmed.substr(1, trimmed.size() - 2);
		}
	}
	return trimmed;
}

static string FileReadFunction(const string &filename) {
	string lower = StringUtil::Lower(filename);
	if (StringUtil::EndsWith(lower, ".csv")) {
		return "read_csv('" + filename + "')";
	} else if (StringUtil::EndsWith(lower, ".parquet")) {
		return "read_parquet('" + filename + "')";
	} else if (StringUtil::EndsWith(lower, ".dta")) {
		return "st_read('" + filename + "')";
	} else if (StringUtil::EndsWith(lower, ".json")) {
		return "read_json('" + filename + "')";
	}
	return filename;
}

// Parse by(var1, var2) from options string. Returns comma-separated var list or empty.
static string ParseByOption(const string &options) {
	string lower = StringUtil::Lower(options);
	// Find by(...)
	idx_t pos = lower.find("by(");
	if (pos == string::npos) {
		return "";
	}
	idx_t start = pos + 3;
	idx_t end = options.find(')', start);
	if (end == string::npos) {
		throw ParserException("Unmatched parenthesis in by() option");
	}
	string by_content = Trim(options.substr(start, end - start));
	// by() content may be space-separated or comma-separated; normalize to comma-separated
	if (by_content.find(',') == string::npos) {
		auto vars = StringUtil::Split(by_content, ' ');
		string result;
		for (idx_t i = 0; i < vars.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += Trim(vars[i]);
		}
		return result;
	}
	return by_content;
}

// Map Stata aggregate function names to SQL equivalents
static string TranslateAggFunction(const string &func_name) {
	string lower = StringUtil::Lower(func_name);
	if (lower == "mean") return "AVG";
	if (lower == "sd") return "STDDEV";
	if (lower == "count") return "COUNT";
	if (lower == "sum") return "SUM";
	if (lower == "min") return "MIN";
	if (lower == "max") return "MAX";
	if (lower == "median") return "MEDIAN";
	if (lower == "first" || lower == "firstnm") return "FIRST";
	if (lower == "last" || lower == "lastnm") return "LAST";
	return func_name;  // pass through unknown functions
}

// Parse a function call like "mean(x)" into {func_name, arg}
static bool ParseFunctionCall(const string &expr, string &func_name, string &arg) {
	idx_t paren = expr.find('(');
	if (paren == string::npos) {
		return false;
	}
	idx_t end = expr.rfind(')');
	if (end == string::npos || end <= paren) {
		return false;
	}
	func_name = Trim(expr.substr(0, paren));
	arg = Trim(expr.substr(paren + 1, end - paren - 1));
	return true;
}

// Check if expression contains _n, _N, L., F., or D. (which expand to window functions)
static bool ExpressionUsesWindowFunctions(const string &expr) {
	std::regex window_re("\\b(_[nN]\\b|[LFD]\\d*\\.)");
	return std::regex_search(expr, window_re);
}

// Translate a cond(test, a, b) call to CASE WHEN test THEN a ELSE b END
// Handles nested parentheses in arguments
static string TranslateCond(const string &args) {
	// Split on commas, respecting parentheses
	int depth = 0;
	vector<string> parts;
	string current;
	for (idx_t i = 0; i < args.size(); i++) {
		char c = args[i];
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
		} else if (c == ',' && depth == 0) {
			parts.push_back(Trim(current));
			current.clear();
			continue;
		}
		current += c;
	}
	parts.push_back(Trim(current));
	if (parts.size() != 3) {
		throw ParserException("cond() requires exactly 3 arguments: cond(test, true_val, false_val)");
	}
	return "CASE WHEN " + parts[0] + " THEN " + parts[1] + " ELSE " + parts[2] + " END";
}

// Translate inrange(x, lo, hi) to (x BETWEEN lo AND hi)
static string TranslateInrange(const string &args) {
	int depth = 0;
	vector<string> parts;
	string current;
	for (idx_t i = 0; i < args.size(); i++) {
		char c = args[i];
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
		} else if (c == ',' && depth == 0) {
			parts.push_back(Trim(current));
			current.clear();
			continue;
		}
		current += c;
	}
	parts.push_back(Trim(current));
	if (parts.size() != 3) {
		throw ParserException("inrange() requires exactly 3 arguments: inrange(x, lo, hi)");
	}
	return "(" + parts[0] + " BETWEEN " + parts[1] + " AND " + parts[2] + ")";
}

// Translate inlist(x, a, b, c, ...) to (x IN (a, b, c, ...))
static string TranslateInlist(const string &args) {
	int depth = 0;
	vector<string> parts;
	string current;
	for (idx_t i = 0; i < args.size(); i++) {
		char c = args[i];
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			depth--;
		} else if (c == ',' && depth == 0) {
			parts.push_back(Trim(current));
			current.clear();
			continue;
		}
		current += c;
	}
	parts.push_back(Trim(current));
	if (parts.size() < 2) {
		throw ParserException("inlist() requires at least 2 arguments: inlist(x, val1, ...)");
	}
	string x = parts[0];
	string vals;
	for (idx_t i = 1; i < parts.size(); i++) {
		if (i > 1) {
			vals += ", ";
		}
		vals += parts[i];
	}
	return "(" + x + " IN (" + vals + "))";
}

// Extract the arguments of a function call: find matching parens for func_name(...)
// Returns the position after the closing paren, or string::npos if not found
static idx_t FindFunctionArgs(const string &s, idx_t open_paren, string &args_out) {
	int depth = 1;
	idx_t i = open_paren + 1;
	while (i < s.size() && depth > 0) {
		if (s[i] == '(') {
			depth++;
		} else if (s[i] == ')') {
			depth--;
		}
		i++;
	}
	if (depth != 0) {
		return string::npos;
	}
	args_out = s.substr(open_paren + 1, i - open_paren - 2);
	return i;
}

static string TranslateExpression(const string &expr, const string &by_cols = "",
                                  const string &panel_var = "", const string &time_var = "",
                                  const string &bysort_order = "") {
	string result = expr;

	// missing(x) -> (x IS NULL) — must be done BEFORE L./F./D. so that
	// missing(L.var) becomes (L.var IS NULL), then L. is translated
	std::regex missing_re("\\bmissing\\s*\\(([^)]+)\\)");
	result = std::regex_replace(result, missing_re, "($1 IS NULL)");

	// L., L2., F., F2., D. time-series operators (gap-aware)
	if (!time_var.empty()) {
		string partition = panel_var.empty() ? "" : "PARTITION BY " + panel_var + " ";
		string order = "ORDER BY " + time_var;
		string over = "OVER (" + partition + order + ")";

		// D.var -> (var - L.var) — expand before L. translation
		std::regex d_re("\\bD(\\d*)\\.(\\w+)");
		std::smatch d_match;
		string d_result = result;
		while (std::regex_search(d_result, d_match, d_re)) {
			string n_str = d_match[1].str();
			int n = n_str.empty() ? 1 : std::stoi(n_str);
			string var = d_match[2].str();
			string replacement = "(" + var + " - L" + to_string(n) + "." + var + ")";
			d_result = d_match.prefix().str() + replacement + d_match.suffix().str();
		}
		result = d_result;

		// L.var, L2.var etc -> gap-aware LAG
		// Pattern: L followed by optional digits, then . then word
		std::regex l_re("\\bL(\\d*)\\.(\\w+)");
		std::smatch l_match;
		string l_result = result;
		while (std::regex_search(l_result, l_match, l_re)) {
			string n_str = l_match[1].str();
			int n = n_str.empty() ? 1 : std::stoi(n_str);
			string var = l_match[2].str();
			// Gap-aware: check that time difference equals n
			string lag_expr = "CASE WHEN (" + time_var + " - LAG(" + time_var + ", " +
			                  to_string(n) + ") " + over + ") = " + to_string(n) +
			                  " THEN LAG(" + var + ", " + to_string(n) + ") " + over +
			                  " ELSE NULL END";
			l_result = l_match.prefix().str() + lag_expr + l_match.suffix().str();
		}
		result = l_result;

		// F.var, F2.var etc -> gap-aware LEAD
		std::regex f_re("\\bF(\\d*)\\.(\\w+)");
		std::smatch f_match;
		string f_result = result;
		while (std::regex_search(f_result, f_match, f_re)) {
			string n_str = f_match[1].str();
			int n = n_str.empty() ? 1 : std::stoi(n_str);
			string var = f_match[2].str();
			// Gap-aware: check that time difference equals n
			string lead_expr = "CASE WHEN (LEAD(" + time_var + ", " + to_string(n) + ") " +
			                   over + " - " + time_var + ") = " + to_string(n) +
			                   " THEN LEAD(" + var + ", " + to_string(n) + ") " + over +
			                   " ELSE NULL END";
			f_result = f_match.prefix().str() + lead_expr + f_match.suffix().str();
		}
		result = f_result;
	}

	// cond(test, a, b) -> CASE WHEN test THEN a ELSE b END
	// Must be done before simple regex replacements since args may contain commas
	{
		string out;
		idx_t pos = 0;
		while (pos < result.size()) {
			// Look for "cond("
			idx_t found = result.find("cond(", pos);
			if (found == string::npos || (found > 0 && (isalnum(result[found - 1]) || result[found - 1] == '_'))) {
				if (found == string::npos) {
					out += result.substr(pos);
					break;
				}
				out += result.substr(pos, found + 1 - pos);
				pos = found + 1;
				continue;
			}
			out += result.substr(pos, found - pos);
			string args;
			idx_t end = FindFunctionArgs(result, found + 4, args);
			if (end == string::npos) {
				out += result.substr(found);
				break;
			}
			out += TranslateCond(args);
			pos = end;
		}
		result = out;
	}

	// inrange(x, lo, hi) -> (x BETWEEN lo AND hi)
	{
		string out;
		idx_t pos = 0;
		while (pos < result.size()) {
			idx_t found = result.find("inrange(", pos);
			if (found == string::npos || (found > 0 && (isalnum(result[found - 1]) || result[found - 1] == '_'))) {
				if (found == string::npos) {
					out += result.substr(pos);
					break;
				}
				out += result.substr(pos, found + 1 - pos);
				pos = found + 1;
				continue;
			}
			out += result.substr(pos, found - pos);
			string args;
			idx_t end = FindFunctionArgs(result, found + 7, args);
			if (end == string::npos) {
				out += result.substr(found);
				break;
			}
			out += TranslateInrange(args);
			pos = end;
		}
		result = out;
	}

	// inlist(x, a, b, ...) -> (x IN (a, b, ...))
	{
		string out;
		idx_t pos = 0;
		while (pos < result.size()) {
			idx_t found = result.find("inlist(", pos);
			if (found == string::npos || (found > 0 && (isalnum(result[found - 1]) || result[found - 1] == '_'))) {
				if (found == string::npos) {
					out += result.substr(pos);
					break;
				}
				out += result.substr(pos, found + 1 - pos);
				pos = found + 1;
				continue;
			}
			out += result.substr(pos, found - pos);
			string args;
			idx_t end = FindFunctionArgs(result, found + 6, args);
			if (end == string::npos) {
				out += result.substr(found);
				break;
			}
			out += TranslateInlist(args);
			pos = end;
		}
		result = out;
	}

	// log() -> LN()
	std::regex log_re("\\blog\\s*\\(");
	result = std::regex_replace(result, log_re, "LN(");
	// missing() already handled above (before L./F./D.)
	// substr(s, start, len) -> SUBSTRING(s, start, len)
	std::regex substr_re("\\bsubstr\\s*\\(");
	result = std::regex_replace(result, substr_re, "SUBSTRING(");
	// strlen(s) -> LENGTH(s)
	std::regex strlen_re("\\bstrlen\\s*\\(");
	result = std::regex_replace(result, strlen_re, "LENGTH(");
	// strlower(s) -> LOWER(s)
	std::regex strlower_re("\\bstrlower\\s*\\(");
	result = std::regex_replace(result, strlower_re, "LOWER(");
	// strupper(s) -> UPPER(s)
	std::regex strupper_re("\\bstrupper\\s*\\(");
	result = std::regex_replace(result, strupper_re, "UPPER(");
	// strtrim(s) -> TRIM(s)
	std::regex strtrim_re("\\bstrtrim\\s*\\(");
	result = std::regex_replace(result, strtrim_re, "TRIM(");
	// real(s) -> CAST(s AS DOUBLE)
	std::regex real_re("\\breal\\s*\\(([^)]+)\\)");
	result = std::regex_replace(result, real_re, "CAST($1 AS DOUBLE)");
	// int(x) -> CAST(x AS INTEGER)
	std::regex int_re("\\bint\\s*\\(([^)]+)\\)");
	result = std::regex_replace(result, int_re, "CAST($1 AS INTEGER)");
	// round(x) and round(x, d) — DuckDB supports ROUND natively, pass through
	// abs(x) — DuckDB supports ABS natively, pass through

	// Convert double-quoted strings to single-quoted (Stata uses " for strings, SQL uses ')
	// But be careful not to convert column name references — only convert within expressions
	{
		string out;
		bool in_dquote = false;
		for (idx_t i = 0; i < result.size(); i++) {
			if (result[i] == '"') {
				out += '\'';
				in_dquote = !in_dquote;
			} else {
				out += result[i];
			}
		}
		result = out;
	}

	// Build partition clause for _n, _N, [_n-1], running aggregates
	string partition = by_cols.empty() ? "" : "PARTITION BY " + by_cols + " ";
	string order_clause = bysort_order.empty() ? "" : "ORDER BY " + bysort_order + " ";

	// var[_n-1] → LAG(var, 1) OVER (...) — MUST be before _n expansion
	// var[_n+1] → LEAD(var, 1) OVER (...)
	{
		std::regex subscript_re("(\\w+)\\[_n\\s*([+-])\\s*(\\d+)\\]");
		std::smatch m;
		string sr = result;
		string out;
		while (std::regex_search(sr, m, subscript_re)) {
			out += m.prefix().str();
			string var = m[1].str();
			string sign = m[2].str();
			int offset = std::stoi(m[3].str());
			string func = (sign == "-") ? "LAG" : "LEAD";
			out += func + "(" + var + ", " + to_string(offset) + ") OVER (" + partition + order_clause + ")";
			sr = m.suffix().str();
		}
		out += sr;
		result = out;
	}

	// _n -> ROW_NUMBER() OVER (PARTITION BY ... ORDER BY ...)
	std::regex n_re("\\b_n\\b");
	result = std::regex_replace(result, n_re, "ROW_NUMBER() OVER (" + partition + order_clause + ")");
	// _N -> COUNT(*) OVER (PARTITION BY ...)
	std::regex N_re("\\b_N\\b");
	result = std::regex_replace(result, N_re, "COUNT(*) OVER (" + partition + ")");

	// Running aggregate functions in bysort context:
	// sum(x) with by_cols → SUM(x) OVER (PARTITION BY ... ORDER BY ... ROWS UNBOUNDED PRECEDING)
	// This only triggers when partition context exists (from bysort)
	// Note: for egen, the OVER() is added separately; this handles generate's running sums
	if (!partition.empty()) {
		// Detect aggregate functions that should become running windows
		// sum() in generate context = running sum (cumulative)
		static const vector<pair<string, string>> running_aggs = {
		    {"sum", "SUM"}, {"mean", "AVG"}, {"count", "COUNT"}, {"min", "MIN"}, {"max", "MAX"}
		};
		for (auto &[stata_fn, sql_fn] : running_aggs) {
			string search = stata_fn + "(";
			idx_t pos = 0;
			while ((pos = result.find(search, pos)) != string::npos) {
				// Check word boundary
				if (pos > 0 && (isalnum(result[pos - 1]) || result[pos - 1] == '_')) {
					pos++;
					continue;
				}
				// Find matching close paren
				string args;
				idx_t end = FindFunctionArgs(result, pos + stata_fn.size(), args);
				if (end == string::npos) {
					pos++;
					continue;
				}
				string replacement = sql_fn + "(" + args + ") OVER (" + partition + order_clause + "ROWS UNBOUNDED PRECEDING)";
				result = result.substr(0, pos) + replacement + result.substr(end);
				pos += replacement.size();
			}
		}
	}

	return result;
}

//===--------------------------------------------------------------------===//
// Process command: update state, return SQL
//===--------------------------------------------------------------------===//
static string ProcessCommand(const StataCommand &cmd, StataDoStateInfo &state) {
	// Helper: translate expression with panel/time and bysort context
	auto TrExpr = [&state, &cmd](const string &expr, const string &by_cols = "") {
		// bysort context overrides by_cols for _n, _N, [_n-1] etc.
		string effective_by = by_cols;
		if (!cmd.bysort_partition.empty() && effective_by.empty()) {
			effective_by = cmd.bysort_partition;
		}
		return TranslateExpression(expr, effective_by, state.panel_var, state.time_var, cmd.bysort_order);
	};

	if (cmd.command == "bysort" || cmd.command == "by") {
		// bysort group_vars (sort_vars): command ...
		// by group_vars: command ...
		// Parse the prefix, extract inner command, process it with bysort context
		string rest = cmd.arguments;
		if (!cmd.condition.empty()) {
			rest += " if " + cmd.condition;
		}
		if (!cmd.options.empty()) {
			rest += ", " + cmd.options;
		}

		// Find the colon separator
		idx_t colon_pos = rest.find(':');
		if (colon_pos == string::npos) {
			throw ParserException("'bysort'/'by' requires a colon: bysort vars: command");
		}
		string prefix_part = Trim(rest.substr(0, colon_pos));
		string inner_cmd_str = Trim(rest.substr(colon_pos + 1));

		// Parse group_vars and optional (sort_vars) from prefix
		string group_vars_str, sort_vars_str;
		idx_t paren_pos = prefix_part.find('(');
		if (paren_pos != string::npos) {
			group_vars_str = Trim(prefix_part.substr(0, paren_pos));
			idx_t close = prefix_part.find(')', paren_pos);
			if (close != string::npos) {
				sort_vars_str = Trim(prefix_part.substr(paren_pos + 1, close - paren_pos - 1));
			}
		} else {
			group_vars_str = prefix_part;
		}

		// Convert space-separated to comma-separated
		auto gvars = StringUtil::Split(group_vars_str, ' ');
		string partition_cols;
		for (idx_t i = 0; i < gvars.size(); i++) {
			if (i > 0) {
				partition_cols += ", ";
			}
			partition_cols += Trim(gvars[i]);
		}
		string order_cols;
		if (!sort_vars_str.empty()) {
			auto svars = StringUtil::Split(sort_vars_str, ' ');
			for (idx_t i = 0; i < svars.size(); i++) {
				if (i > 0) {
					order_cols += ", ";
				}
				order_cols += Trim(svars[i]);
			}
		}

		// Tokenize and process the inner command with bysort context
		auto inner_cmd = TokenizeCommand(inner_cmd_str);
		inner_cmd.bysort_partition = partition_cols;
		inner_cmd.bysort_order = order_cols;

		// For generate with bysort: if the expression uses aggregate functions
		// like sum(), they become running aggregates with ORDER BY
		// The bysort context is passed through to TrExpr via the cmd struct

		return ProcessCommand(inner_cmd, state);
	}

	if (cmd.command == "use") {
		state.Clear();
		string source = ExtractQuotedString(cmd.arguments);
		string read_expr = FileReadFunction(source);
		state.AddStep("SELECT * FROM " + read_expr);
		state.current_source = cmd.arguments;
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "clear") {
		// Generate cleanup SQL for tracked tables, then clear state
		string cleanup = state.BuildCleanupSQL();
		state.ClearAll();
		if (!cleanup.empty()) {
			// Return cleanup SQL + OK. Multiple statements separated by ;
			// will be handled by the parser
			return cleanup + "SELECT 'OK' AS status";
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "tempfile") {
		// tempfile name — register a tempfile identifier and ensure schema exists
		auto names = StringUtil::Split(cmd.arguments, ' ');
		for (auto &n : names) {
			string name = Trim(n);
			if (!name.empty()) {
				state.tempfile_names.insert(name);
			}
		}
		if (!state.tempfiles_schema_created) {
			state.tempfiles_schema_created = true;
			return "CREATE SCHEMA IF NOT EXISTS _tempfiles";
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "preserve") {
		if (state.preserve_checkpoint >= 0) {
			throw ParserException("'preserve' called while already preserved. Use 'restore' first.");
		}
		state.preserve_checkpoint = static_cast<int>(state.cte_steps.size());
		state.preserve_step_counter = state.step_counter;
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "restore") {
		if (state.preserve_checkpoint < 0) {
			throw ParserException("'restore' called without a prior 'preserve'.");
		}
		// Truncate CTE chain back to the checkpoint
		state.cte_steps.resize(state.preserve_checkpoint);
		state.step_counter = state.preserve_step_counter;
		state.preserve_checkpoint = -1;
		state.preserve_step_counter = -1;
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "xtset" || cmd.command == "tsset") {
		// xtset panelvar timevar  OR  tsset timevar  OR  tsset panelvar timevar
		auto vars = StringUtil::Split(cmd.arguments, ' ');
		if (vars.empty()) {
			throw ParserException("'%s' requires at least a time variable", cmd.command);
		}
		if (vars.size() == 1) {
			// tsset timevar (pure time-series, no panel var)
			state.panel_var = "";
			state.time_var = Trim(vars[0]);
		} else {
			// xtset panelvar timevar  OR  tsset panelvar timevar
			state.panel_var = Trim(vars[0]);
			state.time_var = Trim(vars[1]);
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "do") {
		// do "script.do" — read and execute a .do file line by line
		string filename = ExtractQuotedString(cmd.arguments);
		std::ifstream file(filename);
		if (!file.is_open()) {
			throw ParserException("Cannot open file: %s", filename);
		}

		string line;
		bool in_block_comment = false;
		string continued_line;

		while (std::getline(file, line)) {
			string trimmed = Trim(line);

			// Handle block comments /* ... */
			if (in_block_comment) {
				idx_t end_pos = trimmed.find("*/");
				if (end_pos != string::npos) {
					in_block_comment = false;
					trimmed = Trim(trimmed.substr(end_pos + 2));
				} else {
					continue;
				}
			}

			// Check for block comment start
			idx_t block_start = trimmed.find("/*");
			if (block_start != string::npos) {
				idx_t block_end = trimmed.find("*/", block_start + 2);
				if (block_end != string::npos) {
					// Single-line block comment — remove it
					trimmed = Trim(trimmed.substr(0, block_start) + trimmed.substr(block_end + 2));
				} else {
					trimmed = Trim(trimmed.substr(0, block_start));
					in_block_comment = true;
				}
			}

			// Strip // line comments (not inside quotes)
			idx_t comment_pos = trimmed.find("//");
			if (comment_pos != string::npos) {
				// Check it's not /// (line continuation)
				if (comment_pos + 2 < trimmed.size() && trimmed[comment_pos + 2] == '/') {
					// Line continuation: strip /// and join with next line
					continued_line += Trim(trimmed.substr(0, comment_pos)) + " ";
					continue;
				}
				trimmed = Trim(trimmed.substr(0, comment_pos));
			}

			// Strip * line-start comments (Stata convention)
			if (!trimmed.empty() && trimmed[0] == '*') {
				continue;
			}

			// Handle line continuation from previous line
			if (!continued_line.empty()) {
				trimmed = continued_line + trimmed;
				continued_line.clear();
			}

			// Skip empty lines
			if (trimmed.empty()) {
				continue;
			}

			// Strip trailing semicolons (in case the .do file has them)
			if (!trimmed.empty() && trimmed.back() == ';') {
				trimmed.pop_back();
				trimmed = Trim(trimmed);
			}
			if (trimmed.empty()) {
				continue;
			}

			// Check if this is a Stata command we know
			string sub_command;
			if (!IsStataCommand(trimmed, sub_command)) {
				// Not a Stata command — skip
				continue;
			}

			// In do-file execution, only run transformation commands
			// Terminal (list, head, tail, describe) and side-effect (count, summarize,
			// tabulate, save) commands are skipped — user runs them interactively after
			if (!IsTransformationCommand(sub_command)) {
				continue;
			}

			auto sub_cmd = TokenizeCommand(trimmed);
			ProcessCommand(sub_cmd, state);
		}

		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "label") {
		// label variable varname "label text"
		// label define labelname val1 "text1" val2 "text2" ...
		// label values varname labelname
		string lower_args = StringUtil::Lower(cmd.arguments);

		if (StringUtil::StartsWith(lower_args, "variable ")) {
			// label variable varname "label text"
			string rest = Trim(cmd.arguments.substr(9));
			// First token is the variable name
			idx_t space = rest.find(' ');
			if (space == string::npos) {
				throw ParserException("'label variable' requires: label variable varname \"label text\"");
			}
			string varname = Trim(rest.substr(0, space));
			string label_text = ExtractQuotedString(Trim(rest.substr(space + 1)));
			state.variable_labels[varname] = label_text;
			return "SELECT 'OK' AS status";
		}

		if (StringUtil::StartsWith(lower_args, "define ")) {
			// label define labelname val1 "text1" val2 "text2" ...
			string rest = Trim(cmd.arguments.substr(7));
			// First token is the label name
			idx_t space = rest.find(' ');
			if (space == string::npos) {
				throw ParserException("'label define' requires: label define labelname value \"text\" ...");
			}
			string label_name = Trim(rest.substr(0, space));
			string remaining = Trim(rest.substr(space + 1));

			unordered_map<int, string> mapping;
			while (!remaining.empty()) {
				// Parse: integer "text" pairs
				idx_t sp = remaining.find(' ');
				if (sp == string::npos) {
					break;
				}
				string val_str = Trim(remaining.substr(0, sp));
				remaining = Trim(remaining.substr(sp + 1));

				// Parse the quoted text
				string text;
				if (!remaining.empty() && remaining[0] == '"') {
					idx_t end_quote = remaining.find('"', 1);
					if (end_quote == string::npos) {
						throw ParserException("Unmatched quote in label define");
					}
					text = remaining.substr(1, end_quote - 1);
					remaining = Trim(remaining.substr(end_quote + 1));
				} else {
					// Unquoted text — take until next space
					idx_t sp2 = remaining.find(' ');
					if (sp2 != string::npos) {
						text = Trim(remaining.substr(0, sp2));
						remaining = Trim(remaining.substr(sp2 + 1));
					} else {
						text = Trim(remaining);
						remaining = "";
					}
				}

				int val = std::stoi(val_str);
				mapping[val] = text;
			}

			state.value_label_defs[label_name] = mapping;
			return "SELECT 'OK' AS status";
		}

		if (StringUtil::StartsWith(lower_args, "values ")) {
			// label values varname labelname
			string rest = Trim(cmd.arguments.substr(7));
			auto parts = StringUtil::Split(rest, ' ');
			if (parts.size() != 2) {
				throw ParserException("'label values' requires: label values varname labelname");
			}
			string varname = Trim(parts[0]);
			string label_name = Trim(parts[1]);
			// Verify the label definition exists
			if (state.value_label_defs.find(label_name) == state.value_label_defs.end()) {
				throw ParserException("Value label '%s' not defined. Use 'label define' first.", label_name);
			}
			state.column_labels[varname] = label_name;
			return "SELECT 'OK' AS status";
		}

		if (StringUtil::StartsWith(lower_args, "list")) {
			// label list — show all defined labels
			// Build a UNION ALL of variable labels and value label definitions
			string sql = "SELECT * FROM (VALUES ";
			bool has_rows = false;

			// Variable labels
			for (auto &[col, label] : state.variable_labels) {
				string escaped = label;
				size_t pos = 0;
				while ((pos = escaped.find('\'', pos)) != string::npos) {
					escaped.replace(pos, 1, "''");
					pos += 2;
				}
				if (has_rows) {
					sql += ", ";
				}
				sql += "('" + col + "', 'variable', '" + escaped + "')";
				has_rows = true;
			}

			// Column-to-value-label mappings
			for (auto &[col, lbl_name] : state.column_labels) {
				if (has_rows) {
					sql += ", ";
				}
				sql += "('" + col + "', 'value_label', '" + lbl_name + "')";
				has_rows = true;
			}

			// Value label definitions
			for (auto &[lbl_name, mapping] : state.value_label_defs) {
				for (auto &[val, text] : mapping) {
					string escaped = text;
					size_t pos = 0;
					while ((pos = escaped.find('\'', pos)) != string::npos) {
						escaped.replace(pos, 1, "''");
						pos += 2;
					}
					if (has_rows) {
						sql += ", ";
					}
					sql += "('" + lbl_name + "', '" + to_string(val) + "', '" + escaped + "')";
					has_rows = true;
				}
			}

			if (!has_rows) {
				return "SELECT 'No labels defined' AS message";
			}

			sql += ") AS t(name, type, value) ORDER BY name, type";
			return sql;
		}

		throw ParserException("Unknown label subcommand. Use: label variable, label define, label values, or label list");
	}

	if (cmd.command == "import") {
		// import delimited "file", clear — same as use for CSV
		string lower_args = StringUtil::Lower(cmd.arguments);
		if (!StringUtil::StartsWith(lower_args, "delimited ")) {
			throw ParserException("'import' only supports 'import delimited'");
		}
		string rest = Trim(cmd.arguments.substr(10));
		string filename = ExtractQuotedString(rest);
		state.Clear();
		state.AddStep("SELECT * FROM read_csv('" + filename + "')");
		state.current_source = filename;
		return "SELECT 'OK' AS status";
	}

	if (!state.HasData()) {
		throw ParserException("No dataset in memory. Use 'use' to load data first.");
	}

	string prev = state.LatestStep();

	// --- Transformation commands ---
	if (cmd.command == "keep") {
		if (cmd.arguments.empty() && !cmd.condition.empty()) {
			string cond = TrExpr(cmd.condition);
			if (ExpressionUsesWindowFunctions(cmd.condition)) {
				// Window functions can't be in WHERE — add intermediate step
				state.AddStep("SELECT *, (" + cond + ") AS _keep_cond FROM " + prev);
				prev = state.LatestStep();
				state.AddStep("SELECT * EXCLUDE (_keep_cond) FROM " + prev + " WHERE _keep_cond");
			} else {
				state.AddStep("SELECT * FROM " + prev + " WHERE " + cond);
			}
		} else if (!cmd.arguments.empty() && cmd.condition.empty()) {
			// Space-separated var list -> comma-separated
			auto vars = StringUtil::Split(cmd.arguments, ' ');
			string col_list;
			for (idx_t i = 0; i < vars.size(); i++) {
				if (i > 0) {
					col_list += ", ";
				}
				col_list += Trim(vars[i]);
			}
			state.AddStep("SELECT " + col_list + " FROM " + prev);
		} else if (!cmd.arguments.empty() && !cmd.condition.empty()) {
			auto vars = StringUtil::Split(cmd.arguments, ' ');
			string col_list;
			for (idx_t i = 0; i < vars.size(); i++) {
				if (i > 0) {
					col_list += ", ";
				}
				col_list += Trim(vars[i]);
			}
			state.AddStep("SELECT " + col_list + " FROM " + prev + " WHERE " +
			              TrExpr(cmd.condition));
		} else {
			throw ParserException("Invalid 'keep' syntax");
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "drop") {
		if (cmd.arguments.empty() && !cmd.condition.empty()) {
			string cond = TrExpr(cmd.condition);
			if (ExpressionUsesWindowFunctions(cmd.condition)) {
				state.AddStep("SELECT *, (" + cond + ") AS _drop_cond FROM " + prev);
				prev = state.LatestStep();
				state.AddStep("SELECT * EXCLUDE (_drop_cond) FROM " + prev + " WHERE NOT _drop_cond");
			} else {
				state.AddStep("SELECT * FROM " + prev + " WHERE NOT (" + cond + ")");
			}
		} else if (!cmd.arguments.empty() && cmd.condition.empty()) {
			auto vars = StringUtil::Split(cmd.arguments, ' ');
			string exclude_list;
			for (idx_t i = 0; i < vars.size(); i++) {
				if (i > 0) {
					exclude_list += ", ";
				}
				exclude_list += Trim(vars[i]);
			}
			state.AddStep("SELECT * EXCLUDE (" + exclude_list + ") FROM " + prev);
		} else {
			throw ParserException("Invalid 'drop' syntax. Use 'drop var1 var2' or 'drop if condition'.");
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "rename") {
		auto parts = StringUtil::Split(cmd.arguments, ' ');
		if (parts.size() != 2) {
			throw ParserException("'rename' requires exactly two arguments: rename oldname newname");
		}
		string old_name = Trim(parts[0]);
		string new_name = Trim(parts[1]);
		state.AddStep("SELECT * EXCLUDE (" + old_name + "), " + old_name + " AS " + new_name + " FROM " + prev);
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "generate") {
		idx_t eq_pos = cmd.arguments.find('=');
		if (eq_pos == string::npos) {
			throw ParserException("'generate' requires an assignment: generate varname = expression");
		}
		string var_name = Trim(cmd.arguments.substr(0, eq_pos));
		string expr = Trim(cmd.arguments.substr(eq_pos + 1));
		string sql_expr = TrExpr(expr);

		if (!cmd.condition.empty()) {
			string sql_cond = TrExpr(cmd.condition);
			state.AddStep("SELECT *, CASE WHEN " + sql_cond + " THEN " + sql_expr + " ELSE NULL END AS " +
			              var_name + " FROM " + prev);
		} else {
			state.AddStep("SELECT *, (" + sql_expr + ") AS " + var_name + " FROM " + prev);
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "replace") {
		idx_t eq_pos = cmd.arguments.find('=');
		if (eq_pos == string::npos) {
			throw ParserException("'replace' requires an assignment: replace varname = expression");
		}
		string var_name = Trim(cmd.arguments.substr(0, eq_pos));
		string expr = Trim(cmd.arguments.substr(eq_pos + 1));
		string sql_expr = TrExpr(expr);

		if (!cmd.condition.empty()) {
			string sql_cond = TrExpr(cmd.condition);
			state.AddStep("SELECT * REPLACE (CASE WHEN " + sql_cond + " THEN " + sql_expr + " ELSE " + var_name +
			              " END AS " + var_name + ") FROM " + prev);
		} else {
			state.AddStep("SELECT * REPLACE ((" + sql_expr + ") AS " + var_name + ") FROM " + prev);
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "sort") {
		string order = "ASC";
		if (StringUtil::Lower(cmd.options) == "desc") {
			order = "DESC";
		}
		auto vars = StringUtil::Split(cmd.arguments, ' ');
		string order_clause;
		for (idx_t i = 0; i < vars.size(); i++) {
			if (i > 0) {
				order_clause += ", ";
			}
			order_clause += Trim(vars[i]) + " " + order;
		}
		state.AddStep("SELECT * FROM " + prev + " ORDER BY " + order_clause);
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "egen") {
		// egen y = func(x) [if cond], by(g)
		// Translates to window function: SELECT *, FUNC(x) OVER (PARTITION BY g) AS y FROM _prev
		idx_t eq_pos = cmd.arguments.find('=');
		if (eq_pos == string::npos) {
			throw ParserException("'egen' requires an assignment: egen varname = function(arg)");
		}
		string var_name = Trim(cmd.arguments.substr(0, eq_pos));
		string rhs = Trim(cmd.arguments.substr(eq_pos + 1));

		string func_name, func_arg;
		if (!ParseFunctionCall(rhs, func_name, func_arg)) {
			throw ParserException("'egen' requires a function call on the right side: egen y = mean(x)");
		}

		string sql_func = TranslateAggFunction(func_name);
		string by_cols = ParseByOption(cmd.options);
		string partition = by_cols.empty() ? "" : "PARTITION BY " + by_cols;

		string window_expr = sql_func + "(" + func_arg + ") OVER (" + partition + ")";

		if (!cmd.condition.empty()) {
			string sql_cond = TrExpr(cmd.condition, by_cols);
			state.AddStep("SELECT *, CASE WHEN " + sql_cond + " THEN " + window_expr +
			              " ELSE NULL END AS " + var_name + " FROM " + prev);
		} else {
			state.AddStep("SELECT *, " + window_expr + " AS " + var_name + " FROM " + prev);
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "collapse") {
		// collapse (func) y = x [y2 = x2 ...] [if cond], by(g)
		// Translates to: SELECT g, FUNC(x) AS y, ... FROM _prev [WHERE cond] GROUP BY g
		string by_cols = ParseByOption(cmd.options);

		// Parse assignments: each is either "y = func(x)" or "(func) y = x"
		// Stata syntax: collapse (mean) wage = income (sum) hours = workhrs, by(group)
		// Also supports: collapse mean_wage = mean(income), by(group)
		string args = cmd.arguments;

		// Build SELECT and GROUP BY
		string select_exprs;
		if (!by_cols.empty()) {
			select_exprs = by_cols + ", ";
		}

		// Parse the collapse arguments
		// Support two syntaxes:
		// 1. collapse (func) y = x (func2) y2 = x2, by(g)
		// 2. collapse y = func(x) y2 = func2(x2), by(g)
		string current_func = "mean";  // default aggregate function
		string remaining = Trim(args);
		bool first_expr = true;

		while (!remaining.empty()) {
			// Check for (func) prefix
			if (remaining[0] == '(') {
				idx_t close = remaining.find(')');
				if (close == string::npos) {
					throw ParserException("Unmatched parenthesis in collapse");
				}
				current_func = Trim(remaining.substr(1, close - 1));
				remaining = Trim(remaining.substr(close + 1));
				continue;
			}

			// Find the next assignment: y = x, y = func(x), or just varname (shorthand)
			idx_t eq_pos = remaining.find('=');
			// Check if there's a ( before the = — if so, the = might be inside func(x)
			idx_t first_paren = remaining.find('(');
			// Also check: no = at all, or = comes after a ( — means it's a bare variable name
			bool has_assignment = (eq_pos != string::npos &&
			                      (first_paren == string::npos || eq_pos < first_paren));

			if (!has_assignment) {
				// No assignment: "collapse (mean) var1 var2" — apply current_func to each var
				// Parse space-separated variable names until next ( or end
				while (!remaining.empty() && remaining[0] != '(') {
					idx_t sp = remaining.find(' ');
					string var;
					if (sp != string::npos) {
						var = Trim(remaining.substr(0, sp));
						remaining = Trim(remaining.substr(sp + 1));
					} else {
						var = Trim(remaining);
						remaining = "";
					}
					if (var.empty()) {
						continue;
					}
					string sql_func = TranslateAggFunction(current_func);
					if (!first_expr) {
						select_exprs += ", ";
					}
					select_exprs += sql_func + "(" + var + ") AS " + var;
					first_expr = false;
				}
				continue;
			}

			string var_name = Trim(remaining.substr(0, eq_pos));
			remaining = Trim(remaining.substr(eq_pos + 1));

			// Get the source expression
			string source_expr;
			string func_name_used, func_arg_used;

			// Check if it's func(x) syntax
			idx_t paren_pos = remaining.find('(');
			idx_t space_pos = remaining.find(' ');
			if (paren_pos != string::npos && (space_pos == string::npos || paren_pos < space_pos)) {
				// func(x) syntax
				idx_t close_paren = remaining.find(')', paren_pos);
				if (close_paren == string::npos) {
					throw ParserException("Unmatched parenthesis in collapse expression");
				}
				source_expr = Trim(remaining.substr(0, close_paren + 1));
				remaining = Trim(remaining.substr(close_paren + 1));

				ParseFunctionCall(source_expr, func_name_used, func_arg_used);
				string sql_func = TranslateAggFunction(func_name_used);
				if (!first_expr) {
					select_exprs += ", ";
				}
				select_exprs += sql_func + "(" + func_arg_used + ") AS " + var_name;
			} else {
				// Simple variable name, use current_func
				if (space_pos != string::npos) {
					source_expr = Trim(remaining.substr(0, space_pos));
					remaining = Trim(remaining.substr(space_pos + 1));
				} else {
					source_expr = Trim(remaining);
					remaining = "";
				}

				string sql_func = TranslateAggFunction(current_func);
				if (!first_expr) {
					select_exprs += ", ";
				}
				select_exprs += sql_func + "(" + source_expr + ") AS " + var_name;
			}
			first_expr = false;
		}

		string sql = "SELECT " + select_exprs + " FROM " + prev;
		if (!cmd.condition.empty()) {
			sql += " WHERE " + TrExpr(cmd.condition, by_cols);
		}
		if (!by_cols.empty()) {
			sql += " GROUP BY " + by_cols;
		}

		state.AddStep(sql);
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "order") {
		// order var1 var2 — move listed columns to front, rest follow
		// options: last, after(var), before(var), alphabetical
		auto vars = StringUtil::Split(cmd.arguments, ' ');
		if (vars.empty()) {
			throw ParserException("'order' requires at least one variable name");
		}
		string col_list;
		for (idx_t i = 0; i < vars.size(); i++) {
			if (i > 0) {
				col_list += ", ";
			}
			col_list += Trim(vars[i]);
		}
		// Put listed columns first, then all others with COLUMNS(*)
		// DuckDB supports: SELECT col1, col2, * EXCLUDE (col1, col2) FROM t
		state.AddStep("SELECT " + col_list + ", * EXCLUDE (" + col_list + ") FROM " + prev);
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "merge") {
		// merge 1:1 varlist using "file" [, keep(match) keepusing(varlist) nogenerate]
		// merge m:1 varlist using "file" [, ...]
		// merge 1:m varlist using "file" [, ...]
		string args = cmd.arguments;
		string lower_args = StringUtil::Lower(args);

		// Parse merge type: 1:1, m:1, 1:m
		string merge_type;
		if (StringUtil::StartsWith(lower_args, "1:1 ")) {
			merge_type = "1:1";
			args = Trim(args.substr(4));
		} else if (StringUtil::StartsWith(lower_args, "m:1 ")) {
			merge_type = "m:1";
			args = Trim(args.substr(4));
		} else if (StringUtil::StartsWith(lower_args, "1:m ")) {
			merge_type = "1:m";
			args = Trim(args.substr(4));
		} else if (StringUtil::StartsWith(lower_args, "m:m ")) {
			merge_type = "m:m";
			args = Trim(args.substr(4));
		} else {
			throw ParserException("'merge' requires a type: merge 1:1, m:1, 1:m, or m:m");
		}

		// Find "using" keyword to split key vars from filename
		string lower_rest = StringUtil::Lower(args);
		idx_t using_pos = lower_rest.find(" using ");
		if (using_pos == string::npos) {
			throw ParserException("'merge' requires 'using': merge %s varlist using filename", merge_type);
		}
		string key_vars_str = Trim(args.substr(0, using_pos));
		string filename_str = Trim(args.substr(using_pos + 7));
		string filename = ExtractQuotedString(filename_str);
		string using_read = FileReadFunction(filename);

		// Parse key variables (space-separated -> comma-separated)
		auto key_vars = StringUtil::Split(key_vars_str, ' ');
		string key_cols;
		for (idx_t i = 0; i < key_vars.size(); i++) {
			if (i > 0) {
				key_cols += ", ";
			}
			key_cols += Trim(key_vars[i]);
		}

		// Parse options: keep(), keepusing(), nogenerate
		string keep_filter;   // empty = keep all
		string keepusing_cols; // empty = keep all using vars
		bool nogen = false;

		if (!cmd.options.empty()) {
			string opts = cmd.options;
			string lower_opts = StringUtil::Lower(opts);

			// Parse keep()
			idx_t keep_pos = lower_opts.find("keep(");
			if (keep_pos != string::npos) {
				idx_t start = keep_pos + 5;
				idx_t end = opts.find(')', start);
				if (end != string::npos) {
					string keep_str = StringUtil::Lower(Trim(opts.substr(start, end - start)));
					// Parse keep values: match/master/using or 1/2/3
					bool keep_master = false, keep_using = false, keep_match = false;
					if (keep_str.find("master") != string::npos || keep_str.find("1") != string::npos) {
						keep_master = true;
					}
					if (keep_str.find("using") != string::npos || keep_str.find("2") != string::npos) {
						keep_using = true;
					}
					if (keep_str.find("match") != string::npos || keep_str.find("3") != string::npos) {
						keep_match = true;
					}
					// Build filter expression
					vector<string> conditions;
					if (keep_master) {
						conditions.push_back("_merge = 1");
					}
					if (keep_using) {
						conditions.push_back("_merge = 2");
					}
					if (keep_match) {
						conditions.push_back("_merge = 3");
					}
					if (!conditions.empty()) {
						keep_filter = "(";
						for (idx_t i = 0; i < conditions.size(); i++) {
							if (i > 0) {
								keep_filter += " OR ";
							}
							keep_filter += conditions[i];
						}
						keep_filter += ")";
					}
				}
			}

			// Parse keepusing()
			idx_t ku_pos = lower_opts.find("keepusing(");
			if (ku_pos != string::npos) {
				idx_t start = ku_pos + 10;
				idx_t end = opts.find(')', start);
				if (end != string::npos) {
					string ku_str = Trim(opts.substr(start, end - start));
					auto ku_vars = StringUtil::Split(ku_str, ' ');
					for (idx_t i = 0; i < ku_vars.size(); i++) {
						if (i > 0) {
							keepusing_cols += ", ";
						}
						keepusing_cols += Trim(ku_vars[i]);
					}
				}
			}

			// Parse nogenerate / nogen
			if (lower_opts.find("nogen") != string::npos) {
				nogen = true;
			}
		}

		// Build the JOIN SQL
		// Strategy: FULL OUTER JOIN, compute _merge, then filter with keep()
		// For the using dataset, alias as _using to avoid column name conflicts
		string using_select;
		if (keepusing_cols.empty()) {
			using_select = "SELECT * FROM " + using_read;
		} else {
			using_select = "SELECT " + key_cols + ", " + keepusing_cols + " FROM " + using_read;
		}

		// Build USING clause for the JOIN (key columns)
		string using_clause;
		for (idx_t i = 0; i < key_vars.size(); i++) {
			if (i > 0) {
				using_clause += ", ";
			}
			using_clause += Trim(key_vars[i]);
		}

		// Determine JOIN type based on keep() option for optimization
		string join_type;
		if (keep_filter.find("_merge = 1") == string::npos &&
		    keep_filter.find("_merge = 2") == string::npos && !keep_filter.empty()) {
			// keep(match) only → INNER JOIN
			join_type = "INNER JOIN";
		} else if (keep_filter.find("_merge = 2") == string::npos && !keep_filter.empty()) {
			// keep(master) or keep(master match) → LEFT JOIN
			join_type = "LEFT JOIN";
		} else {
			// Default: keep all → FULL OUTER JOIN
			join_type = "FULL OUTER JOIN";
		}

		// Build the _merge column
		// We need a way to detect which side each row came from
		// Add a tag column to each side before joining
		string master_sql = "SELECT *, 1 AS _m_tag FROM " + prev;
		string using_sql = "SELECT *, 1 AS _u_tag FROM (" + using_select + ")";

		string merge_expr = "CASE WHEN _m_tag IS NOT NULL AND _u_tag IS NOT NULL THEN 3 "
		                    "WHEN _m_tag IS NOT NULL THEN 1 "
		                    "ELSE 2 END AS _merge";

		// Build the full join
		string join_sql = "SELECT * EXCLUDE (_m_tag, _u_tag), " + merge_expr +
		                  " FROM (" + master_sql + ") AS _master " + join_type +
		                  " (" + using_sql + ") AS _using USING (" + using_clause + ")";

		// Apply keep() filter
		if (!keep_filter.empty()) {
			join_sql = "SELECT * FROM (" + join_sql + ") WHERE " + keep_filter;
		}

		// Remove _merge if nogenerate
		if (nogen) {
			join_sql = "SELECT * EXCLUDE (_merge) FROM (" + join_sql + ")";
		}

		state.AddStep(join_sql);
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "duplicates") {
		// duplicates drop [varlist]
		string lower_args = StringUtil::Lower(cmd.arguments);
		if (!StringUtil::StartsWith(lower_args, "drop")) {
			throw ParserException("'duplicates' only supports 'duplicates drop [varlist]'");
		}
		string rest = Trim(cmd.arguments.substr(4));
		if (rest.empty()) {
			// duplicates drop — deduplicate on all columns
			state.AddStep("SELECT DISTINCT * FROM " + prev);
		} else {
			// duplicates drop var1 var2 — keep first row per group of varlist
			auto vars = StringUtil::Split(rest, ' ');
			string col_list;
			for (idx_t i = 0; i < vars.size(); i++) {
				if (i > 0) {
					col_list += ", ";
				}
				col_list += Trim(vars[i]);
			}
			// Use ROW_NUMBER to keep first row per group
			state.AddStep("SELECT * EXCLUDE (_dedup_rn) FROM ("
			              "SELECT *, ROW_NUMBER() OVER (PARTITION BY " + col_list + ") AS _dedup_rn FROM " + prev +
			              ") WHERE _dedup_rn = 1");
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "expand") {
		// expand N [, generate(newvar)]
		// N can be a constant or a variable name
		string n_expr = Trim(cmd.arguments);
		string gen_var;
		// Check for generate() option
		if (!cmd.options.empty()) {
			string lower_opts = StringUtil::Lower(cmd.options);
			idx_t gen_pos = lower_opts.find("generate(");
			if (gen_pos == string::npos) {
				gen_pos = lower_opts.find("gen(");
			}
			if (gen_pos != string::npos) {
				idx_t start = cmd.options.find('(', gen_pos) + 1;
				idx_t end = cmd.options.find(')', start);
				if (end != string::npos) {
					gen_var = Trim(cmd.options.substr(start, end - start));
				}
			}
		}

		// Generate a series for each row and cross join
		// If N is a variable, each row can have a different expansion count
		if (gen_var.empty()) {
			state.AddStep("SELECT * EXCLUDE (_expand_idx) FROM ("
			              "SELECT t.*, g.generate_series AS _expand_idx FROM " + prev +
			              " t, LATERAL GENERATE_SERIES(1, " + n_expr + ") g)");
		} else {
			// generate(newvar) — newvar is 0 for original row, 1 for copies
			state.AddStep("SELECT * EXCLUDE (_expand_idx), "
			              "CASE WHEN _expand_idx = 1 THEN 0 ELSE 1 END AS " + gen_var +
			              " FROM (SELECT t.*, g.generate_series AS _expand_idx FROM " + prev +
			              " t, LATERAL GENERATE_SERIES(1, " + n_expr + ") g)");
		}
		return "SELECT 'OK' AS status";
	}

	// --- Side-effect commands ---
	if (cmd.command == "export") {
		// export delimited using "file", replace
		string lower_args = StringUtil::Lower(cmd.arguments);
		if (!StringUtil::StartsWith(lower_args, "delimited ")) {
			throw ParserException("'export' only supports 'export delimited'");
		}
		string rest = Trim(cmd.arguments.substr(10));
		// Strip optional "using" keyword
		string lower_rest = StringUtil::Lower(rest);
		if (StringUtil::StartsWith(lower_rest, "using ")) {
			rest = Trim(rest.substr(6));
		}
		string filename = ExtractQuotedString(rest);
		return "COPY (" + state.BuildQuery("SELECT * FROM " + prev) + ") TO '" + filename + "' (FORMAT CSV, HEADER)";
	}

	// --- Terminal commands ---
	if (cmd.command == "list") {
		string cols = "*";
		if (!cmd.arguments.empty()) {
			auto vars = StringUtil::Split(cmd.arguments, ' ');
			cols = "";
			for (idx_t i = 0; i < vars.size(); i++) {
				if (i > 0) {
					cols += ", ";
				}
				cols += Trim(vars[i]);
			}
		}
		string sql = "SELECT " + cols + " FROM " + prev;
		if (!cmd.condition.empty()) {
			sql += " WHERE " + TrExpr(cmd.condition);
		}
		return state.BuildQuery(sql);
	}

	if (cmd.command == "count") {
		string sql = "SELECT COUNT(*) AS n FROM " + prev;
		if (!cmd.condition.empty()) {
			sql += " WHERE " + TrExpr(cmd.condition);
		}
		return state.BuildQuery(sql);
	}

	if (cmd.command == "head") {
		string n = cmd.arguments.empty() ? "5" : Trim(cmd.arguments);
		return state.BuildQuery("SELECT * FROM " + prev + " LIMIT " + n);
	}

	if (cmd.command == "tail") {
		string n = cmd.arguments.empty() ? "5" : Trim(cmd.arguments);
		return state.BuildQuery(
		    "SELECT * EXCLUDE (_rn, _total) FROM ("
		    "SELECT *, ROW_NUMBER() OVER () AS _rn, COUNT(*) OVER () AS _total FROM " +
		    prev + ") sub WHERE _rn > _total - " + n);
	}

	if (cmd.command == "describe" || cmd.command == "codebook") {
		// codebook is an alias for describe that doesn't conflict with SQL
		// Build a query that shows column metadata including labels
		// Base: column_name, column_type from DESCRIBE
		// Enhanced: add variable_label and value_label columns from state
		string base_sql = "SELECT column_name, column_type FROM (DESCRIBE " +
		                  state.BuildQuery("SELECT * FROM " + prev) + ")";

		if (state.variable_labels.empty() && state.column_labels.empty()) {
			return base_sql;
		}

		// Build a CASE expression for variable labels
		string var_label_expr;
		if (!state.variable_labels.empty()) {
			var_label_expr = "CASE column_name";
			for (auto &[col, label] : state.variable_labels) {
				string escaped = label;
				size_t pos = 0;
				while ((pos = escaped.find('\'', pos)) != string::npos) {
					escaped.replace(pos, 1, "''");
					pos += 2;
				}
				var_label_expr += " WHEN '" + col + "' THEN '" + escaped + "'";
			}
			var_label_expr += " ELSE '' END";
		} else {
			var_label_expr = "''";
		}

		// Build a CASE expression for value label names
		string val_label_expr;
		if (!state.column_labels.empty()) {
			val_label_expr = "CASE column_name";
			for (auto &[col, label_name] : state.column_labels) {
				val_label_expr += " WHEN '" + col + "' THEN '" + label_name + "'";
			}
			val_label_expr += " ELSE '' END";
		} else {
			val_label_expr = "''";
		}

		return "SELECT column_name, column_type, " + var_label_expr + " AS variable_label, " +
		       val_label_expr + " AS value_label FROM (DESCRIBE " +
		       state.BuildQuery("SELECT * FROM " + prev) + ")";
	}

	if (cmd.command == "summarize") {
		if (cmd.arguments.empty()) {
			throw ParserException("'summarize' requires at least one variable name");
		}
		string var = Trim(cmd.arguments);
		string where_clause;
		if (!cmd.condition.empty()) {
			where_clause = " WHERE " + TrExpr(cmd.condition);
		}
		string sql = "SELECT "
		             "COUNT(" + var + ") AS N, "
		             "AVG(" + var + ") AS mean, "
		             "STDDEV(" + var + ") AS sd, "
		             "MIN(" + var + ") AS min, "
		             "PERCENTILE_CONT(0.25) WITHIN GROUP (ORDER BY " + var + ") AS p25, "
		             "PERCENTILE_CONT(0.50) WITHIN GROUP (ORDER BY " + var + ") AS p50, "
		             "PERCENTILE_CONT(0.75) WITHIN GROUP (ORDER BY " + var + ") AS p75, "
		             "MAX(" + var + ") AS max "
		             "FROM " + prev + where_clause;
		return state.BuildQuery(sql);
	}

	if (cmd.command == "tabulate") {
		auto vars = StringUtil::Split(cmd.arguments, ' ');
		if (vars.empty()) {
			throw ParserException("'tabulate' requires at least one variable name");
		}
		string group_cols;
		for (idx_t i = 0; i < vars.size(); i++) {
			if (i > 0) {
				group_cols += ", ";
			}
			group_cols += Trim(vars[i]);
		}
		string where_clause;
		if (!cmd.condition.empty()) {
			where_clause = " WHERE " + TrExpr(cmd.condition);
		}
		return state.BuildQuery("SELECT " + group_cols + ", COUNT(*) AS freq FROM " + prev + where_clause +
		                        " GROUP BY " + group_cols + " ORDER BY " + group_cols);
	}

	if (cmd.command == "save") {
		string target = ExtractQuotedString(cmd.arguments);
		string lower_opts = StringUtil::Lower(cmd.options);
		bool save_as_table = (lower_opts.find("table") != string::npos ||
		                      lower_opts.find("memory") != string::npos);

		// Check if target is a registered tempfile
		bool is_tempfile = (state.tempfile_names.find(target) != state.tempfile_names.end());

		if (save_as_table || is_tempfile) {
			// Save as in-memory table
			string table_name;
			string full_query = state.BuildQuery("SELECT * FROM " + prev);

			if (is_tempfile) {
				// Tempfile → _tempfiles schema (schema created by tempfile command)
				table_name = "_tempfiles." + target;
				state.tempfile_tables.push_back(table_name);
				return "CREATE OR REPLACE TABLE " + table_name + " AS (" + full_query + ")";
			} else {
				// Explicit table save — not garbage collected
				table_name = target;
				return "CREATE OR REPLACE TABLE " + table_name + " AS (" + full_query + ")";
			}
		}

		// Save to disk file
		string lower_fn = StringUtil::Lower(target);
		string format_clause;
		if (StringUtil::EndsWith(lower_fn, ".csv")) {
			format_clause = " (FORMAT CSV, HEADER)";
		} else if (StringUtil::EndsWith(lower_fn, ".parquet")) {
			format_clause = " (FORMAT PARQUET)";
		}
		return "COPY (" + state.BuildQuery("SELECT * FROM " + prev) + ") TO '" + target + "'" + format_clause;
	}

	if (cmd.command == "append") {
		// append using "file.csv"
		string args = cmd.arguments;
		string lower_args = StringUtil::Lower(args);
		// Strip optional "using" keyword
		if (StringUtil::StartsWith(lower_args, "using ")) {
			args = Trim(args.substr(6));
		}
		string filename = ExtractQuotedString(args);
		string read_expr = FileReadFunction(filename);
		state.AddStep("SELECT * FROM " + prev + " UNION ALL BY NAME SELECT * FROM " + read_expr);
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "mvencode") {
		// mvencode var1 var2 [_all], mv(value)
		// Replace missing values with a specified value
		string mv_value = "0";  // default
		// Parse mv() from options
		string lower_opts = StringUtil::Lower(cmd.options);
		idx_t mv_pos = lower_opts.find("mv(");
		if (mv_pos != string::npos) {
			idx_t start = mv_pos + 3;
			idx_t end = cmd.options.find(')', start);
			if (end != string::npos) {
				mv_value = Trim(cmd.options.substr(start, end - start));
			}
		}

		auto vars = StringUtil::Split(cmd.arguments, ' ');
		// Check for _all
		bool all_vars = false;
		for (auto &v : vars) {
			if (StringUtil::Lower(Trim(v)) == "_all") {
				all_vars = true;
				break;
			}
		}

		if (all_vars) {
			// Replace missing in all columns — use COALESCE with COLUMNS(*)
			// DuckDB doesn't support COALESCE on COLUMNS(*) directly, so we use a workaround:
			// SELECT COLUMNS(c -> COALESCE(c, mv_value)) FROM _prev
			// Actually DuckDB supports: SELECT * REPLACE (...) but we'd need column names
			// Simplest: just pass through, user should list columns explicitly
			throw ParserException("'mvencode _all' is not yet supported. List column names explicitly.");
		}

		string replace_list;
		for (idx_t i = 0; i < vars.size(); i++) {
			string var = Trim(vars[i]);
			if (var.empty()) {
				continue;
			}
			if (i > 0) {
				replace_list += ", ";
			}
			replace_list += "COALESCE(" + var + ", " + mv_value + ") AS " + var;
		}
		state.AddStep("SELECT * REPLACE (" + replace_list + ") FROM " + prev);
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "reshape") {
		// reshape long varlist, i(id_vars) j(time_var)
		// reshape wide varlist, i(id_vars) j(time_var)
		string lower_args = StringUtil::Lower(cmd.arguments);
		bool is_long = StringUtil::StartsWith(lower_args, "long ");
		bool is_wide = StringUtil::StartsWith(lower_args, "wide ");

		if (!is_long && !is_wide) {
			throw ParserException("'reshape' requires 'long' or 'wide': reshape long/wide varlist, i(ids) j(timevar)");
		}

		string varlist_str = Trim(cmd.arguments.substr(5)); // skip "long " or "wide "
		auto value_vars = StringUtil::Split(varlist_str, ' ');

		// Parse i() and j() from options
		string i_vars, j_var;
		string opts = cmd.options;
		string lower_opt = StringUtil::Lower(opts);

		idx_t i_pos = lower_opt.find("i(");
		if (i_pos != string::npos) {
			idx_t start = i_pos + 2;
			idx_t end = opts.find(')', start);
			if (end != string::npos) {
				i_vars = Trim(opts.substr(start, end - start));
			}
		}

		idx_t j_pos = lower_opt.find("j(");
		if (j_pos != string::npos) {
			idx_t start = j_pos + 2;
			idx_t end = opts.find(')', start);
			if (end != string::npos) {
				j_var = Trim(opts.substr(start, end - start));
			}
		}

		if (j_var.empty()) {
			throw ParserException("'reshape' requires j(varname) option");
		}

		if (is_long) {
			// UNPIVOT: wide → long
			// Stata convention: "reshape long revenue, i(id) j(year)" means
			// unpivot columns matching revenue_* pattern, strip the prefix from j values
			// Use COLUMNS('stub_.*') regex to match, then strip prefix from j var
			if (value_vars.size() == 1) {
				string stub = Trim(value_vars[0]);
				state.AddStep(
				    "SELECT * REPLACE (REPLACE(" + j_var + ", '" + stub + "_', '') AS " + j_var +
				    ") FROM (UNPIVOT " + prev + " ON COLUMNS('" + stub + "_.*') INTO NAME " +
				    j_var + " VALUE " + stub + ")");
			} else {
				// Multiple value vars: each stub becomes a value column
				// UNPIVOT ON COLUMNS('stub1_.*'), COLUMNS('stub2_.*') INTO NAME j VALUE stub1, stub2
				string on_clause;
				string value_names;
				string replace_expr = "REPLACE(" + j_var;
				for (idx_t i = 0; i < value_vars.size(); i++) {
					string stub = Trim(value_vars[i]);
					if (i > 0) {
						on_clause += ", ";
						value_names += ", ";
					}
					on_clause += "COLUMNS('" + stub + "_.*')";
					value_names += stub;
					replace_expr += ", '" + stub + "_', ''";
				}
				// Strip all prefixes from j var
				replace_expr = "REGEXP_REPLACE(" + j_var + ", '^[^_]+_', '') AS " + j_var;
				state.AddStep(
				    "SELECT * REPLACE (" + replace_expr +
				    ") FROM (UNPIVOT " + prev + " ON " + on_clause + " INTO NAME " +
				    j_var + " VALUE " + value_names + ")");
			}
		} else {
			// PIVOT: long → wide
			// DuckDB PIVOT internally expands to multiple statements, so it can't live
			// inside a CTE. We create a temp table from the current chain, PIVOT it,
			// then restart the chain from the temp table.
			if (i_vars.empty()) {
				throw ParserException("'reshape wide' requires i(id_vars) option");
			}
			if (value_vars.size() != 1) {
				throw ParserException("'reshape wide' currently supports one value variable");
			}
			string value_var = Trim(value_vars[0]);
			auto i_var_list = StringUtil::Split(i_vars, ' ');
			string i_cols;
			for (idx_t i = 0; i < i_var_list.size(); i++) {
				if (i > 0) {
					i_cols += ", ";
				}
				i_cols += Trim(i_var_list[i]);
			}
			// PIVOT generates MULTI statements that can't live in CTEs.
			// Strategy: create temp table via PIVOT, restart chain from it.
			string subquery = "(" + state.BuildQuery("SELECT * FROM " + prev) + ")";
			state.Clear();
			// __PIVOT__ marker tells plan_function/parser_override to handle specially
			return "__PIVOT__:CREATE OR REPLACE TEMP TABLE _stata_pivot AS (PIVOT " +
			       subquery + " ON " + j_var + " USING FIRST(" + value_var +
			       ") GROUP BY " + i_cols + ");" +
			       "||STATE||_stata_pivot";
		}
		return "SELECT 'OK' AS status";
	}

	throw ParserException("Unimplemented Stata command: " + cmd.command);
}

//===--------------------------------------------------------------------===//
// parse_function: detect Stata commands (called when standard parser fails)
//===--------------------------------------------------------------------===//
static ParserExtensionParseResult stata_do_parse(ParserExtensionInfo *info, const string &query) {
	string command;
	if (!IsStataCommand(query, command)) {
		return ParserExtensionParseResult();
	}
	auto parse_data = make_uniq<StataDoParseData>(query);
	return ParserExtensionParseResult(std::move(parse_data));
}

// Check if a single statement needs parser_override (SQL keyword conflict or PIVOT)
static bool NeedsParserOverride(const string &stmt, StataDoStateInfo &state) {
	string lower = StringUtil::Lower(stmt);
	if (state.HasData() || (!state.cte_steps.empty())) {
		if (StringUtil::StartsWith(lower, "describe") &&
		    (lower.size() == 8 || lower[8] == ' ')) {
			return true;
		}
		if (StringUtil::StartsWith(lower, "summarize") &&
		    (lower.size() == 9 || lower[9] == ' ')) {
			return true;
		}
		if (StringUtil::StartsWith(lower, "reshape") && lower.find("wide") != string::npos) {
			return true;
		}
	}
	return false;
}

//===--------------------------------------------------------------------===//
// parser_override: handles commands that conflict with SQL keywords
// (describe, summarize, reshape wide) — called BEFORE the standard parser.
// Must handle full multi-statement strings by splitting on ';'.
//===--------------------------------------------------------------------===//
static ParserOverrideResult stata_do_parser_override(ParserExtensionInfo *info, const string &query,
                                                     ParserOptions &options) {
	auto &state = dynamic_cast<StataDoStateInfo &>(*info);

	// Split the full query into individual statements by ';'
	auto statements_str = StringUtil::Split(query, ';');

	// Check if ANY statement needs our override
	// We must take over the ENTIRE query if any statement is describe/summarize/reshape wide,
	// because the standard parser would handle those before our parse_function sees them.
	// We also take over if earlier Stata commands will load data (making later describe valid).
	bool has_stata_commands = false;
	bool has_conflict_commands = false;
	for (auto &s : statements_str) {
		string trimmed = Trim(s);
		if (trimmed.empty()) {
			continue;
		}
		string lower = StringUtil::Lower(trimmed);

		// Check for SQL-conflicting commands:
		// use "file" — SQL USE schema conflicts
		// import delimited — SQL IMPORT conflicts
		// describe, summarize — SQL DESCRIBE/SUMMARIZE conflicts
		// reshape wide — PIVOT generates MULTI statements
		if (StringUtil::StartsWith(lower, "use ") && (lower.find('"') != string::npos || lower.find('\'') != string::npos)) {
			has_conflict_commands = true;
		}
		if (StringUtil::StartsWith(lower, "import ")) {
			has_conflict_commands = true;
		}
		if (StringUtil::StartsWith(lower, "describe") || StringUtil::StartsWith(lower, "summarize")) {
			has_conflict_commands = true;
		}
		if (StringUtil::StartsWith(lower, "reshape") && lower.find("wide") != string::npos) {
			has_conflict_commands = true;
		}

		string cmd_name;
		if (IsStataCommand(trimmed, cmd_name)) {
			has_stata_commands = true;
		}
	}

	// Take over if there's a SQL-conflicting command AND either Stata commands or existing state.
	// When we take over, we process ALL statements ourselves (updating state for use/label/etc
	// before generating SQL for describe/summarize).
	bool any_needs_override = has_conflict_commands && (has_stata_commands || state.HasData());

	if (!any_needs_override) {
		return ParserOverrideResult();
	}

	// Process ALL statements ourselves
	try {
		vector<unique_ptr<SQLStatement>> all_statements;

		for (auto &s : statements_str) {
			string trimmed = Trim(s);
			if (trimmed.empty()) {
				continue;
			}

			// Check if this is a Stata command
			string cmd_name;
			if (IsStataCommand(trimmed, cmd_name)) {
				auto cmd = TokenizeCommand(trimmed);
				string sql = ProcessCommand(cmd, state);

				// Handle __PIVOT__ marker
				if (StringUtil::StartsWith(sql, "__PIVOT__:")) {
					string rest = sql.substr(10);
					idx_t state_pos = rest.find("||STATE||");
					string pivot_sql = rest.substr(0, state_pos);
					string table_name = rest.substr(state_pos + 9);

					Parser parser;
					parser.ParseQuery(pivot_sql);
					state.AddStep("SELECT * FROM " + table_name);
					for (auto &stmt : parser.statements) {
						all_statements.push_back(std::move(stmt));
					}
					continue;
				}

				Parser parser;
				parser.ParseQuery(sql);
				for (auto &stmt : parser.statements) {
					all_statements.push_back(std::move(stmt));
				}
			} else {
				// Not a Stata command — parse as regular SQL
				Parser parser;
				parser.ParseQuery(trimmed);
				for (auto &stmt : parser.statements) {
					all_statements.push_back(std::move(stmt));
				}
			}
		}

		if (all_statements.empty()) {
			return ParserOverrideResult();
		}
		return ParserOverrideResult(std::move(all_statements));
	} catch (std::exception &ex) {
		return ParserOverrideResult(ex);
	}
}

//===--------------------------------------------------------------------===//
// plan_function: generate SQL, store parsed statement, throw to redirect
//===--------------------------------------------------------------------===//
static ParserExtensionPlanResult stata_do_plan(ParserExtensionInfo *info, ClientContext &context,
                                               unique_ptr<ParserExtensionParseData> parse_data) {
	auto &stata_data = dynamic_cast<StataDoParseData &>(*parse_data);
	auto &state = dynamic_cast<StataDoStateInfo &>(*info);

	// Generate the SQL from the Stata command
	auto cmd = TokenizeCommand(stata_data.raw_query);
	string sql = ProcessCommand(cmd, state);

	// Parse the generated SQL with DuckDB's parser
	Parser parser;
	try {
		parser.ParseQuery(sql);
	} catch (std::exception &ex) {
		throw BinderException("stata_do: failed to parse generated SQL: %s\nSQL: %s", ex.what(), sql);
	}
	if (parser.statements.empty()) {
		throw BinderException("stata_do: generated SQL produced no statements");
	}

	// Store the parsed statement in client context state
	auto bind_state = make_shared_ptr<StataDoBindState>(std::move(parser.statements[0]));
	context.registered_state->Remove("stata_do_bind");
	context.registered_state->Insert("stata_do_bind", bind_state);

	// Throw a BinderException to redirect to OperatorExtension::Bind
	throw BinderException("stata_do redirect to operator bind");
}

//===--------------------------------------------------------------------===//
// OperatorExtension::Bind — picks up stored statement and binds it
//===--------------------------------------------------------------------===//
BoundStatement stata_do_bind(ClientContext &context, Binder &binder, OperatorExtensionInfo *info,
                             SQLStatement &statement) {
	// Check if we have a stored statement from plan_function
	auto bind_state = context.registered_state->Get<StataDoBindState>("stata_do_bind");
	if (!bind_state || !bind_state->statement) {
		// Not our statement — return empty to let other extensions try
		return BoundStatement();
	}

	// Bind the stored SQL statement
	auto sql_binder = Binder::CreateBinder(context, &binder);
	auto result = sql_binder->Bind(*bind_state->statement);

	// Clear the bind state
	context.registered_state->Remove("stata_do_bind");

	return result;
}

//===--------------------------------------------------------------------===//
// Extension Loading
//===--------------------------------------------------------------------===//

static void LoadInternal(ExtensionLoader &loader) {
	auto &instance = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(instance);

	// Shared state for all parser paths
	auto shared_state = make_shared_ptr<StataDoStateInfo>();

	// Register parser extension (parse_function for most commands, parser_override for SQL-conflicting ones)
	ParserExtension parser_ext;
	parser_ext.parse_function = stata_do_parse;
	parser_ext.plan_function = stata_do_plan;
	parser_ext.parser_override = stata_do_parser_override;
	parser_ext.parser_info = shared_state;
	ParserExtension::Register(config, parser_ext);

	// Enable parser override in fallback mode (override runs first, falls back to standard parser)
	config.SetOptionByName("allow_parser_override_extension", Value("fallback"));

	// Register operator extension for the bind redirect
	auto operator_ext = make_shared_ptr<StataDoOperatorExtension>();
	OperatorExtension::Register(config, operator_ext);
}

void StataDoExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string StataDoExtension::Name() {
	return "stata_do";
}

std::string StataDoExtension::Version() const {
#ifdef EXT_VERSION_STATA_DO
	return EXT_VERSION_STATA_DO;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(stata_do, loader) {
	duckdb::LoadInternal(loader);
}
}
