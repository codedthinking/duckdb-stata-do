#include "dodo_core.hpp"

#include <fstream>
#include <regex>

namespace dodo {

using std::string;
using std::vector;
using std::unordered_map;
using std::unordered_set;
using std::pair;
using std::to_string;

// Helper: Trim that returns a new string
static string Trim(const string &s) {
	string result = s;
	str::Trim(result);
	return result;
}

// Helper: quote an identifier if it's a SQL keyword or needs quoting
static string QuoteIdent(const string &s) {
	return str::QuoteIdent(s);
}



//===--------------------------------------------------------------------===//
// Command Tokenizer
//===--------------------------------------------------------------------===//

const vector<string> DODO_COMMANDS = {"use",       "list",       "clear",   "keep",    "drop",
                                              "generate",  "replace",    "rename",  "sort",    "order",
                                              "egen",      "collapse",   "count",   "describe", "summarize",
                                              "tabulate",  "head",       "tail",    "save",    "append",
                                              "mvencode",  "reshape",    "do",      "label",   "codebook",
                                              "duplicates", "expand",    "export",  "import",
                                              "merge",     "tempfile",  "preserve", "restore",
                                              "xtset",     "tsset",    "bysort",  "by",
                                              "undo",      "redo",     "history"};

// Command classification for do-file execution
// Transformation: modifies the CTE chain state
// Terminal: materializes the chain, returns the data frame
// Side-effect: returns a derived result (count, stats, freq table, file write)
bool IsTransformationCommand(const string &command) {
	static const vector<string> TRANSFORMATION = {"use", "clear", "do", "keep", "drop", "generate", "replace",
	                                              "rename", "sort", "order", "egen", "collapse", "mvencode",
	                                              "reshape", "append", "label", "duplicates", "expand", "import",
	                                              "merge", "tempfile", "preserve", "restore",
	                                              "xtset", "tsset", "bysort", "by",
	                                              "undo", "redo"};
	for (auto &cmd : TRANSFORMATION) {
		if (command == cmd) {
			return true;
		}
	}
	return false;
}

// Side-effect commands that produce SQL beyond state updates — must be executed in do-files
bool IsSideEffectCommand(const string &command) {
	return command == "export" || command == "save";
}



bool IsDodoCommand(const string &query, string &command_out) {
	string trimmed = Trim(query);
	if (!trimmed.empty() && trimmed.back() == ';') {
		trimmed.pop_back();
		trimmed = Trim(trimmed);
	}
	string lower = str::Lower(trimmed);
	for (auto &cmd : DODO_COMMANDS) {
		if (str::StartsWith(lower, cmd)) {
			if (lower.size() == cmd.size() || lower[cmd.size()] == ' ' || lower[cmd.size()] == ',') {
				command_out = cmd;
				return true;
			}
		}
	}
	return false;
}



DodoCommand TokenizeCommand(const string &query) {
	DodoCommand result;
	string trimmed = Trim(query);
	if (!trimmed.empty() && trimmed.back() == ';') {
		trimmed.pop_back();
		trimmed = Trim(trimmed);
	}

	string lower = str::Lower(trimmed);
	for (auto &cmd : DODO_COMMANDS) {
		if (str::StartsWith(lower, cmd) &&
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
	string lower_bc = str::Lower(before_comma);
	idx_t if_pos = string::npos;
	idx_t cond_start = string::npos;

	// Check if it starts with "if "
	if (str::StartsWith(lower_bc, "if ")) {
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

string ExtractQuotedString(const string &s) {
	string trimmed = Trim(s);
	if (trimmed.size() >= 2) {
		if ((trimmed.front() == '"' && trimmed.back() == '"') ||
		    (trimmed.front() == '\'' && trimmed.back() == '\'')) {
			return trimmed.substr(1, trimmed.size() - 2);
		}
	}
	return trimmed;
}

string FileReadFunction(const string &filename) {
	string lower = str::Lower(filename);
	if (str::EndsWith(lower, ".csv")) {
		return "read_csv('" + filename + "')";
	} else if (str::EndsWith(lower, ".parquet")) {
		return "read_parquet('" + filename + "')";
	} else if (str::EndsWith(lower, ".dta")) {
		return "st_read('" + filename + "')";
	} else if (str::EndsWith(lower, ".json")) {
		return "read_json('" + filename + "')";
	}
	return filename;
}

// Parse by(var1, var2) from options string. Returns comma-separated var list or empty.
string ParseByOption(const string &options) {
	string lower = str::Lower(options);
	// Find by(...)
	idx_t pos = lower.find("by(");
	if (pos == string::npos) {
		return "";
	}
	idx_t start = pos + 3;
	idx_t end = options.find(')', start);
	if (end == string::npos) {
		throw DodoException("Unmatched parenthesis in by() option");
	}
	string by_content = Trim(options.substr(start, end - start));
	// by() content may be space-separated or comma-separated; normalize to comma-separated
	// Quote each variable name to handle SQL keyword conflicts
	if (by_content.find(',') == string::npos) {
		auto vars = str::Split(by_content, ' ');
		string result;
		for (idx_t i = 0; i < vars.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += QuoteIdent(Trim(vars[i]));
		}
		return result;
	}
	// Comma-separated: quote each part
	auto vars = str::Split(by_content, ',');
	string result;
	for (idx_t i = 0; i < vars.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += QuoteIdent(Trim(vars[i]));
	}
	return result;
}

// Map aggregate function names to SQL equivalents
string TranslateAggFunction(const string &func_name) {
	string lower = str::Lower(func_name);
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
bool ParseFunctionCall(const string &expr, string &func_name, string &arg) {
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
bool ExpressionUsesWindowFunctions(const string &expr) {
	std::regex window_re("\\b(_[nN]\\b|[LFD]\\d*\\.)");
	return std::regex_search(expr, window_re);
}

// Translate a cond(test, a, b) call to CASE WHEN test THEN a ELSE b END
// Handles nested parentheses in arguments
string TranslateCond(const string &args) {
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
		throw DodoException("cond() requires exactly 3 arguments: cond(test, true_val, false_val)");
	}
	return "CASE WHEN " + parts[0] + " THEN " + parts[1] + " ELSE " + parts[2] + " END";
}

// Translate inrange(x, lo, hi) to (x BETWEEN lo AND hi)
string TranslateInrange(const string &args) {
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
		throw DodoException("inrange() requires exactly 3 arguments: inrange(x, lo, hi)");
	}
	return "(" + parts[0] + " BETWEEN " + parts[1] + " AND " + parts[2] + ")";
}

// Translate inlist(x, a, b, c, ...) to (x IN (a, b, c, ...))
string TranslateInlist(const string &args) {
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
		throw DodoException("inlist() requires at least 2 arguments: inlist(x, val1, ...)");
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
idx_t FindFunctionArgs(const string &s, idx_t open_paren, string &args_out) {
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

string TranslateExpression(const string &expr, const string &by_cols,
                                  const string &panel_var, const string &time_var,
                                  const string &bysort_order) {
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

	// Convert double-quoted strings to single-quoted (do-file syntax uses " for strings, SQL uses ')
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

	// var[_n-1] -> LAG(var, 1) OVER (...) — MUST be before _n expansion
	// var[_n+1] -> LEAD(var, 1) OVER (...)
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
	// sum(x) with by_cols -> SUM(x) OVER (PARTITION BY ... ORDER BY ... ROWS UNBOUNDED PRECEDING)
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
string ProcessCommand(const DodoCommand &cmd, DodoState &state) {
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
			throw DodoException("'bysort'/'by' requires a colon: bysort vars: command");
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
		auto gvars = str::Split(group_vars_str, ' ');
		string partition_cols;
		for (idx_t i = 0; i < gvars.size(); i++) {
			if (i > 0) {
				partition_cols += ", ";
			}
			partition_cols += QuoteIdent(Trim(gvars[i]));
		}
		string order_cols;
		if (!sort_vars_str.empty()) {
			auto svars = str::Split(sort_vars_str, ' ');
			for (idx_t i = 0; i < svars.size(); i++) {
				if (i > 0) {
					order_cols += ", ";
				}
				order_cols += QuoteIdent(Trim(svars[i]));
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
		// Drop previous materialized table if switching datasets
		string pre_cleanup;
		if (state.materialized) {
			pre_cleanup = "DROP TABLE IF EXISTS dodo._current; ";
		}
		string saved_cmd = state.pending_command;
		state.Clear();
		state.pending_command = saved_cmd;
		string source = ExtractQuotedString(cmd.arguments);
		string read_expr = FileReadFunction(source);
		string lower_opts = str::Lower(cmd.options);
		bool lazy = (lower_opts.find("lazy") != string::npos);
		bool is_file = (read_expr != source);

		string result_sql;
		if (!lazy && is_file) {
			// Materialize: create table, reference it in CTE chain
			result_sql = "CREATE SCHEMA IF NOT EXISTS dodo; ";
			result_sql += "CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM " + read_expr + "; ";
			state.AddStep("SELECT * FROM dodo._current");
			state.materialized = true;
		} else {
			// Lazy mode or existing table: reference directly
			state.AddStep("SELECT * FROM " + read_expr);
		}
		state.current_source = cmd.arguments;
		result_sql += "SELECT 'OK' AS status";
		return pre_cleanup + result_sql;
	}

	if (cmd.command == "clear") {
		// Generate cleanup SQL for tracked tables, then clear state
		string cleanup = state.BuildCleanupSQL();
		if (state.live_view_enabled) {
			cleanup += "DROP VIEW IF EXISTS _dodo_data; ";
		}
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
		auto names = str::Split(cmd.arguments, ' ');
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
			throw DodoException("'preserve' called while already preserved. Use 'restore' first.");
		}
		state.preserve_checkpoint = static_cast<int>(state.cte_steps.size());
		state.preserve_step_counter = state.step_counter;
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "restore") {
		if (state.preserve_checkpoint < 0) {
			throw DodoException("'restore' called without a prior 'preserve'.");
		}
		// Truncate CTE chain back to the checkpoint
		state.cte_steps.resize(state.preserve_checkpoint);
		state.cte_commands.resize(state.preserve_checkpoint);
		state.step_counter = state.preserve_step_counter;
		state.preserve_checkpoint = -1;
		state.preserve_step_counter = -1;
		state.redo_stack.clear();
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "undo") {
		if (state.cte_steps.size() <= 1) {
			throw DodoException("Nothing to undo.");
		}
		int n = 1;
		if (!cmd.arguments.empty()) {
			n = std::stoi(Trim(cmd.arguments));
		}
		int max_undo = static_cast<int>(state.cte_steps.size()) - 1;
		if (n > max_undo) {
			n = max_undo;
		}
		for (int i = 0; i < n; i++) {
			string cte = state.cte_steps.back();
			string command_text = state.cte_commands.back();
			state.redo_stack.push_back({command_text, cte});
			state.cte_steps.pop_back();
			state.cte_commands.pop_back();
			state.step_counter--;
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "redo") {
		if (state.redo_stack.empty()) {
			throw DodoException("Nothing to redo.");
		}
		int n = 1;
		if (!cmd.arguments.empty()) {
			n = std::stoi(Trim(cmd.arguments));
		}
		int max_redo = static_cast<int>(state.redo_stack.size());
		if (n > max_redo) {
			n = max_redo;
		}
		for (int i = 0; i < n; i++) {
			auto &entry = state.redo_stack.back();
			state.cte_commands.push_back(entry.first);
			state.cte_steps.push_back(entry.second);
			state.step_counter++;
			state.redo_stack.pop_back();
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "xtset" || cmd.command == "tsset") {
		// xtset panelvar timevar  OR  tsset timevar  OR  tsset panelvar timevar
		auto vars = str::Split(cmd.arguments, ' ');
		if (vars.empty()) {
			throw DodoException("'" + cmd.command + "' requires at least a time variable");
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
		string filename = ExtractQuotedString(cmd.arguments);
		auto side_effect_sql = ProcessDoFile(filename, state);
		string result;
		for (auto &sql : side_effect_sql) {
			result += sql + "; ";
		}
		result += "SELECT 'OK' AS status";
		return result;
	}

	if (cmd.command == "label") {
		// label variable varname "label text"
		// label define labelname val1 "text1" val2 "text2" ...
		// label values varname labelname
		string lower_args = str::Lower(cmd.arguments);

		if (str::StartsWith(lower_args, "variable ")) {
			// label variable varname "label text"
			string rest = Trim(cmd.arguments.substr(9));
			// First token is the variable name
			idx_t space = rest.find(' ');
			if (space == string::npos) {
				throw DodoException("'label variable' requires: label variable varname \"label text\"");
			}
			string varname = Trim(rest.substr(0, space));
			string label_text = ExtractQuotedString(Trim(rest.substr(space + 1)));
			state.variable_labels[varname] = label_text;
			return "SELECT 'OK' AS status";
		}

		if (str::StartsWith(lower_args, "define ")) {
			// label define labelname val1 "text1" val2 "text2" ...
			string rest = Trim(cmd.arguments.substr(7));
			// First token is the label name
			idx_t space = rest.find(' ');
			if (space == string::npos) {
				throw DodoException("'label define' requires: label define labelname value \"text\" ...");
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
						throw DodoException("Unmatched quote in label define");
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

		if (str::StartsWith(lower_args, "values ")) {
			// label values varname labelname
			string rest = Trim(cmd.arguments.substr(7));
			auto parts = str::Split(rest, ' ');
			if (parts.size() != 2) {
				throw DodoException("'label values' requires: label values varname labelname");
			}
			string varname = Trim(parts[0]);
			string label_name = Trim(parts[1]);
			// Verify the label definition exists
			if (state.value_label_defs.find(label_name) == state.value_label_defs.end()) {
				throw DodoException("Value label '" + label_name + "' not defined. Use 'label define' first.");
			}
			state.column_labels[varname] = label_name;
			return "SELECT 'OK' AS status";
		}

		if (str::StartsWith(lower_args, "list")) {
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

		throw DodoException("Unknown label subcommand. Use: label variable, label define, label values, or label list");
	}

	if (cmd.command == "import") {
		// import delimited "file", clear — same as use for CSV
		string lower_args = str::Lower(cmd.arguments);
		if (!str::StartsWith(lower_args, "delimited ")) {
			throw DodoException("'import' only supports 'import delimited'");
		}
		string rest = Trim(cmd.arguments.substr(10));
		string filename = ExtractQuotedString(rest);
		string lower_opts = str::Lower(cmd.options);
		bool lazy = (lower_opts.find("lazy") != string::npos);

		// Drop previous materialized table if switching datasets
		string pre_cleanup;
		if (state.materialized) {
			pre_cleanup = "DROP TABLE IF EXISTS dodo._current; ";
		}
		string saved_cmd = state.pending_command;
		state.Clear();
		state.pending_command = saved_cmd;

		string result_sql;
		if (!lazy) {
			result_sql = "CREATE SCHEMA IF NOT EXISTS dodo; ";
			result_sql += "CREATE OR REPLACE TABLE dodo._current AS SELECT * FROM read_csv('" + filename + "'); ";
			state.AddStep("SELECT * FROM dodo._current");
			state.materialized = true;
		} else {
			state.AddStep("SELECT * FROM read_csv('" + filename + "')");
		}
		state.current_source = filename;
		result_sql += "SELECT 'OK' AS status";
		return pre_cleanup + result_sql;
	}

	if (cmd.command == "history") {
		// Terminal: show command history
		if (state.materialized) {
			return "SELECT * FROM dodo._history ORDER BY step_id";
		}
		// In-memory fallback: build VALUES table
		string sql = "SELECT * FROM (VALUES ";
		bool first = true;
		for (idx_t i = 0; i < state.cte_commands.size(); i++) {
			if (!first) {
				sql += ", ";
			}
			string escaped = state.cte_commands[i];
			size_t pos = 0;
			while ((pos = escaped.find('\'', pos)) != string::npos) {
				escaped.replace(pos, 1, "''");
				pos += 2;
			}
			sql += "(" + to_string(i) + ", '" + escaped + "', false)";
			first = false;
		}
		for (idx_t i = 0; i < state.redo_stack.size(); i++) {
			if (!first) {
				sql += ", ";
			}
			string escaped = state.redo_stack[state.redo_stack.size() - 1 - i].first;
			size_t pos = 0;
			while ((pos = escaped.find('\'', pos)) != string::npos) {
				escaped.replace(pos, 1, "''");
				pos += 2;
			}
			int step_id = static_cast<int>(state.cte_commands.size()) + static_cast<int>(i);
			sql += "(" + to_string(step_id) + ", '" + escaped + "', true)";
			first = false;
		}
		if (first) {
			return "SELECT 'No commands in history' AS message";
		}
		sql += ") AS t(step_id, command, undone) ORDER BY step_id";
		return sql;
	}

	if (!state.HasData()) {
		throw DodoException("No dataset in memory. Use 'use' to load data first.");
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
			auto vars = str::Split(cmd.arguments, ' ');
			string col_list;
			for (idx_t i = 0; i < vars.size(); i++) {
				if (i > 0) {
					col_list += ", ";
				}
				col_list += QuoteIdent(Trim(vars[i]));
			}
			state.AddStep("SELECT " + col_list + " FROM " + prev);
		} else if (!cmd.arguments.empty() && !cmd.condition.empty()) {
			auto vars = str::Split(cmd.arguments, ' ');
			string col_list;
			for (idx_t i = 0; i < vars.size(); i++) {
				if (i > 0) {
					col_list += ", ";
				}
				col_list += QuoteIdent(Trim(vars[i]));
			}
			state.AddStep("SELECT " + col_list + " FROM " + prev + " WHERE " +
			              TrExpr(cmd.condition));
		} else {
			throw DodoException("Invalid 'keep' syntax");
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
			auto vars = str::Split(cmd.arguments, ' ');
			string exclude_list;
			for (idx_t i = 0; i < vars.size(); i++) {
				if (i > 0) {
					exclude_list += ", ";
				}
				exclude_list += QuoteIdent(Trim(vars[i]));
			}
			state.AddStep("SELECT * EXCLUDE (" + exclude_list + ") FROM " + prev);
		} else {
			throw DodoException("Invalid 'drop' syntax. Use 'drop var1 var2' or 'drop if condition'.");
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "rename") {
		auto parts = str::Split(cmd.arguments, ' ');
		if (parts.size() != 2) {
			throw DodoException("'rename' requires exactly two arguments: rename oldname newname");
		}
		string old_name = QuoteIdent(Trim(parts[0]));
		string new_name = QuoteIdent(Trim(parts[1]));
		state.AddStep("SELECT * EXCLUDE (" + old_name + "), " + old_name + " AS " + new_name + " FROM " + prev);
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "generate") {
		idx_t eq_pos = cmd.arguments.find('=');
		if (eq_pos == string::npos) {
			throw DodoException("'generate' requires an assignment: generate varname = expression");
		}
		string var_name = QuoteIdent(Trim(cmd.arguments.substr(0, eq_pos)));
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
			throw DodoException("'replace' requires an assignment: replace varname = expression");
		}
		string var_name = QuoteIdent(Trim(cmd.arguments.substr(0, eq_pos)));
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
		if (str::Lower(cmd.options) == "desc") {
			order = "DESC";
		}
		auto vars = str::Split(cmd.arguments, ' ');
		string order_clause;
		for (idx_t i = 0; i < vars.size(); i++) {
			if (i > 0) {
				order_clause += ", ";
			}
			order_clause += QuoteIdent(Trim(vars[i])) + " " + order;
		}
		state.AddStep("SELECT * FROM " + prev + " ORDER BY " + order_clause);
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "egen") {
		// egen y = func(x) [if cond], by(g)
		// Translates to window function: SELECT *, FUNC(x) OVER (PARTITION BY g) AS y FROM _prev
		idx_t eq_pos = cmd.arguments.find('=');
		if (eq_pos == string::npos) {
			throw DodoException("'egen' requires an assignment: egen varname = function(arg)");
		}
		string var_name = QuoteIdent(Trim(cmd.arguments.substr(0, eq_pos)));
		string rhs = Trim(cmd.arguments.substr(eq_pos + 1));

		string func_name, func_arg;
		if (!ParseFunctionCall(rhs, func_name, func_arg)) {
			throw DodoException("'egen' requires a function call on the right side: egen y = mean(x)");
		}

		string sql_func = TranslateAggFunction(func_name);
		string by_cols = ParseByOption(cmd.options);
		string partition = by_cols.empty() ? "" : "PARTITION BY " + by_cols;

		string window_expr = sql_func + "(" + QuoteIdent(func_arg) + ") OVER (" + partition + ")";

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
		// Syntax: collapse (mean) wage = income (sum) hours = workhrs, by(group)
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
					throw DodoException("Unmatched parenthesis in collapse");
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
					select_exprs += sql_func + "(" + QuoteIdent(var) + ") AS " + QuoteIdent(var);
					first_expr = false;
				}
				continue;
			}

			string var_name = QuoteIdent(Trim(remaining.substr(0, eq_pos)));
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
					throw DodoException("Unmatched parenthesis in collapse expression");
				}
				source_expr = Trim(remaining.substr(0, close_paren + 1));
				remaining = Trim(remaining.substr(close_paren + 1));

				ParseFunctionCall(source_expr, func_name_used, func_arg_used);
				string sql_func = TranslateAggFunction(func_name_used);
				if (!first_expr) {
					select_exprs += ", ";
				}
				select_exprs += sql_func + "(" + QuoteIdent(func_arg_used) + ") AS " + var_name;
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
				select_exprs += sql_func + "(" + QuoteIdent(source_expr) + ") AS " + var_name;
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
		auto vars = str::Split(cmd.arguments, ' ');
		if (vars.empty()) {
			throw DodoException("'order' requires at least one variable name");
		}
		string col_list;
		for (idx_t i = 0; i < vars.size(); i++) {
			if (i > 0) {
				col_list += ", ";
			}
			col_list += QuoteIdent(Trim(vars[i]));
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
		string lower_args = str::Lower(args);

		// Parse merge type: 1:1, m:1, 1:m
		string merge_type;
		if (str::StartsWith(lower_args, "1:1 ")) {
			merge_type = "1:1";
			args = Trim(args.substr(4));
		} else if (str::StartsWith(lower_args, "m:1 ")) {
			merge_type = "m:1";
			args = Trim(args.substr(4));
		} else if (str::StartsWith(lower_args, "1:m ")) {
			merge_type = "1:m";
			args = Trim(args.substr(4));
		} else if (str::StartsWith(lower_args, "m:m ")) {
			merge_type = "m:m";
			args = Trim(args.substr(4));
		} else {
			throw DodoException("'merge' requires a type: merge 1:1, m:1, 1:m, or m:m");
		}

		// Find "using" keyword to split key vars from filename
		string lower_rest = str::Lower(args);
		idx_t using_pos = lower_rest.find(" using ");
		if (using_pos == string::npos) {
			throw DodoException("'merge' requires 'using': merge " + merge_type + " varlist using filename");
		}
		string key_vars_str = Trim(args.substr(0, using_pos));
		string filename_str = Trim(args.substr(using_pos + 7));
		string filename = ExtractQuotedString(filename_str);
		string using_read = FileReadFunction(filename);

		// Parse key variables (space-separated -> comma-separated)
		auto key_vars = str::Split(key_vars_str, ' ');
		string key_cols;
		for (idx_t i = 0; i < key_vars.size(); i++) {
			if (i > 0) {
				key_cols += ", ";
			}
			key_cols += QuoteIdent(Trim(key_vars[i]));
		}

		// Parse options: keep(), keepusing(), nogenerate
		string keep_filter;   // empty = keep all
		string keepusing_cols; // empty = keep all using vars
		bool nogen = false;

		if (!cmd.options.empty()) {
			string opts = cmd.options;
			string lower_opts = str::Lower(opts);

			// Parse keep()
			idx_t keep_pos = lower_opts.find("keep(");
			if (keep_pos != string::npos) {
				idx_t start = keep_pos + 5;
				idx_t end = opts.find(')', start);
				if (end != string::npos) {
					string keep_str = str::Lower(Trim(opts.substr(start, end - start)));
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
					auto ku_vars = str::Split(ku_str, ' ');
					for (idx_t i = 0; i < ku_vars.size(); i++) {
						if (i > 0) {
							keepusing_cols += ", ";
						}
						keepusing_cols += QuoteIdent(Trim(ku_vars[i]));
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
			using_clause += QuoteIdent(Trim(key_vars[i]));
		}

		// Determine JOIN type based on keep() option for optimization
		string join_type;
		if (keep_filter.find("_merge = 1") == string::npos &&
		    keep_filter.find("_merge = 2") == string::npos && !keep_filter.empty()) {
			// keep(match) only -> INNER JOIN
			join_type = "INNER JOIN";
		} else if (keep_filter.find("_merge = 2") == string::npos && !keep_filter.empty()) {
			// keep(master) or keep(master match) -> LEFT JOIN
			join_type = "LEFT JOIN";
		} else {
			// Default: keep all -> FULL OUTER JOIN
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
		string lower_args = str::Lower(cmd.arguments);
		if (!str::StartsWith(lower_args, "drop")) {
			throw DodoException("'duplicates' only supports 'duplicates drop [varlist]'");
		}
		string rest = Trim(cmd.arguments.substr(4));
		if (rest.empty()) {
			// duplicates drop — deduplicate on all columns
			state.AddStep("SELECT DISTINCT * FROM " + prev);
		} else {
			// duplicates drop var1 var2 — keep first row per group of varlist
			auto vars = str::Split(rest, ' ');
			string col_list;
			for (idx_t i = 0; i < vars.size(); i++) {
				if (i > 0) {
					col_list += ", ";
				}
				col_list += QuoteIdent(Trim(vars[i]));
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
			string lower_opts = str::Lower(cmd.options);
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
			              "CASE WHEN _expand_idx = 1 THEN 0 ELSE 1 END AS " + QuoteIdent(gen_var) +
			              " FROM (SELECT t.*, g.generate_series AS _expand_idx FROM " + prev +
			              " t, LATERAL GENERATE_SERIES(1, " + n_expr + ") g)");
		}
		return "SELECT 'OK' AS status";
	}

	// --- Side-effect commands ---
	if (cmd.command == "export") {
		// export delimited using "file", replace
		string lower_args = str::Lower(cmd.arguments);
		if (!str::StartsWith(lower_args, "delimited ")) {
			throw DodoException("'export' only supports 'export delimited'");
		}
		string rest = Trim(cmd.arguments.substr(10));
		// Strip optional "using" keyword
		string lower_rest = str::Lower(rest);
		if (str::StartsWith(lower_rest, "using ")) {
			rest = Trim(rest.substr(6));
		}
		string filename = ExtractQuotedString(rest);
		return "COPY (" + state.BuildQuery("SELECT * FROM " + prev) + ") TO '" + filename + "' (FORMAT CSV, HEADER)";
	}

	// --- Terminal commands ---
	if (cmd.command == "list") {
		string cols = "*";
		if (!cmd.arguments.empty()) {
			auto vars = str::Split(cmd.arguments, ' ');
			cols = "";
			for (idx_t i = 0; i < vars.size(); i++) {
				if (i > 0) {
					cols += ", ";
				}
				cols += QuoteIdent(Trim(vars[i]));
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
			throw DodoException("'summarize' requires at least one variable name");
		}
		string var = QuoteIdent(Trim(cmd.arguments));
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
		auto vars = str::Split(cmd.arguments, ' ');
		if (vars.empty()) {
			throw DodoException("'tabulate' requires at least one variable name");
		}
		string group_cols;
		for (idx_t i = 0; i < vars.size(); i++) {
			if (i > 0) {
				group_cols += ", ";
			}
			group_cols += QuoteIdent(Trim(vars[i]));
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
		string lower_opts = str::Lower(cmd.options);
		bool save_as_table = (lower_opts.find("table") != string::npos ||
		                      lower_opts.find("memory") != string::npos);

		// Check if target is a registered tempfile
		bool is_tempfile = (state.tempfile_names.find(target) != state.tempfile_names.end());

		if (save_as_table || is_tempfile) {
			// Save as in-memory table
			string table_name;
			string full_query = state.BuildQuery("SELECT * FROM " + prev);

			if (is_tempfile) {
				// Tempfile -> _tempfiles schema (schema created by tempfile command)
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
		string lower_fn = str::Lower(target);
		string format_clause;
		if (str::EndsWith(lower_fn, ".csv")) {
			format_clause = " (FORMAT CSV, HEADER)";
		} else if (str::EndsWith(lower_fn, ".parquet")) {
			format_clause = " (FORMAT PARQUET)";
		}
		return "COPY (" + state.BuildQuery("SELECT * FROM " + prev) + ") TO '" + target + "'" + format_clause;
	}

	if (cmd.command == "append") {
		// append using "file.csv"
		string args = cmd.arguments;
		string lower_args = str::Lower(args);
		// Strip optional "using" keyword
		if (str::StartsWith(lower_args, "using ")) {
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
		string lower_opts = str::Lower(cmd.options);
		idx_t mv_pos = lower_opts.find("mv(");
		if (mv_pos != string::npos) {
			idx_t start = mv_pos + 3;
			idx_t end = cmd.options.find(')', start);
			if (end != string::npos) {
				mv_value = Trim(cmd.options.substr(start, end - start));
			}
		}

		auto vars = str::Split(cmd.arguments, ' ');
		// Check for _all
		bool all_vars = false;
		for (auto &v : vars) {
			if (str::Lower(Trim(v)) == "_all") {
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
			throw DodoException("'mvencode _all' is not yet supported. List column names explicitly.");
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
			string qvar = QuoteIdent(var);
			replace_list += "COALESCE(" + qvar + ", " + mv_value + ") AS " + qvar;
		}
		state.AddStep("SELECT * REPLACE (" + replace_list + ") FROM " + prev);
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "reshape") {
		// reshape long varlist, i(id_vars) j(time_var)
		// reshape wide varlist, i(id_vars) j(time_var)
		string lower_args = str::Lower(cmd.arguments);
		bool is_long = str::StartsWith(lower_args, "long ");
		bool is_wide = str::StartsWith(lower_args, "wide ");

		if (!is_long && !is_wide) {
			throw DodoException("'reshape' requires 'long' or 'wide': reshape long/wide varlist, i(ids) j(timevar)");
		}

		string varlist_str = Trim(cmd.arguments.substr(5)); // skip "long " or "wide "
		auto value_vars = str::Split(varlist_str, ' ');

		// Parse i() and j() from options
		string i_vars, j_var;
		string opts = cmd.options;
		string lower_opt = str::Lower(opts);

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
			throw DodoException("'reshape' requires j(varname) option");
		}

		string qj = QuoteIdent(j_var);

		if (is_long) {
			// UNPIVOT: wide -> long
			// do-file convention: "reshape long revenue, i(id) j(year)" means
			// unpivot columns matching revenue_* pattern, strip the prefix from j values
			// Use COLUMNS('stub_.*') regex to match, then strip prefix from j var
			if (value_vars.size() == 1) {
				string stub = Trim(value_vars[0]);
				string qstub = QuoteIdent(stub);
				state.AddStep(
				    "SELECT * REPLACE (REPLACE(" + qj + ", '" + stub + "_', '') AS " + qj +
				    ") FROM (UNPIVOT " + prev + " ON COLUMNS('" + stub + "_.*') INTO NAME " +
				    qj + " VALUE " + qstub + ")");
			} else {
				// Multiple value vars: each stub becomes a value column
				// UNPIVOT ON COLUMNS('stub1_.*'), COLUMNS('stub2_.*') INTO NAME j VALUE stub1, stub2
				string on_clause;
				string value_names;
				for (idx_t i = 0; i < value_vars.size(); i++) {
					string stub = Trim(value_vars[i]);
					if (i > 0) {
						on_clause += ", ";
						value_names += ", ";
					}
					on_clause += "COLUMNS('" + stub + "_.*')";
					value_names += QuoteIdent(stub);
				}
				// Strip all prefixes from j var
				string replace_expr = "REGEXP_REPLACE(" + qj + ", '^[^_]+_', '') AS " + qj;
				state.AddStep(
				    "SELECT * REPLACE (" + replace_expr +
				    ") FROM (UNPIVOT " + prev + " ON " + on_clause + " INTO NAME " +
				    qj + " VALUE " + value_names + ")");
			}
		} else {
			// PIVOT: long -> wide
			// DuckDB PIVOT internally expands to multiple statements, so it can't live
			// inside a CTE. We create a temp table from the current chain, PIVOT it,
			// then restart the chain from the temp table.
			if (i_vars.empty()) {
				throw DodoException("'reshape wide' requires i(id_vars) option");
			}
			if (value_vars.size() != 1) {
				throw DodoException("'reshape wide' currently supports one value variable");
			}
			string value_var = Trim(value_vars[0]);
			auto i_var_list = str::Split(i_vars, ' ');
			string i_cols;
			for (idx_t i = 0; i < i_var_list.size(); i++) {
				if (i > 0) {
					i_cols += ", ";
				}
				i_cols += QuoteIdent(Trim(i_var_list[i]));
			}
			// PIVOT generates MULTI statements that can't live in CTEs.
			// Strategy: create temp table via PIVOT, restart chain from it.
			string subquery = "(" + state.BuildQuery("SELECT * FROM " + prev) + ")";
			state.Clear();
			// __PIVOT__ marker tells plan_function/parser_override to handle specially
			return "__PIVOT__:CREATE OR REPLACE TEMP TABLE _dodo_pivot AS (PIVOT " +
			       subquery + " ON " + qj + " USING FIRST(" + QuoteIdent(value_var) +
			       ") GROUP BY " + i_cols + ");" +
			       "||STATE||_dodo_pivot";
		}
		return "SELECT 'OK' AS status";
	}

	throw DodoException("Unimplemented command: " + cmd.command);
}

//===--------------------------------------------------------------------===//
// ProcessDoFile: read and execute a .do file, returning side-effect SQL
//===--------------------------------------------------------------------===//
vector<string> ProcessDoFile(const string &filename, DodoState &state) {
	std::ifstream file(filename);
	if (!file.is_open()) {
		throw DodoException("Cannot open file: " + filename);
	}

	string line;
	bool in_block_comment = false;
	string continued_line;
	vector<string> side_effect_sql;

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

		// Strip * line-start comments (do-file convention)
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

		// Check if this is a command we know
		string sub_command;
		if (!IsDodoCommand(trimmed, sub_command)) {
			// Not a known command — skip
			continue;
		}

		// In do-file execution, run transformation and side-effect commands
		// Terminal (list, head, tail, describe) and query commands (count,
		// summarize, tabulate) are skipped — user runs them interactively after
		if (!IsTransformationCommand(sub_command) && !IsSideEffectCommand(sub_command)) {
			continue;
		}

		auto sub_cmd = TokenizeCommand(trimmed);
		state.pending_command = trimmed;
		string sql = ProcessCommand(sub_cmd, state);

		// In do-file context, use/import cannot materialize (table creation
		// SQL can't execute mid-script). Rewrite to lazy if needed.
		if ((sub_command == "use" || sub_command == "import") && state.materialized) {
			// Undo materialization: replace dodo._current reference with direct file read
			string source = ExtractQuotedString(sub_cmd.arguments);
			string read_expr = FileReadFunction(source);
			if (sub_command == "import") {
				string rest = Trim(sub_cmd.arguments.substr(10));
				read_expr = "read_csv('" + ExtractQuotedString(rest) + "')";
			}
			state.cte_steps.clear();
			state.step_counter = 0;
			state.AddStep("SELECT * FROM " + read_expr);
			state.materialized = false;
		}

		// Side-effect commands (export, save) return SQL that must be executed
		if (IsSideEffectCommand(sub_command)) {
			side_effect_sql.push_back(sql);
		}
	}

	return side_effect_sql;
}

} // namespace dodo
