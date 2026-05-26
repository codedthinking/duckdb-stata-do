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

static const vector<string> STATA_COMMANDS = {"use",       "list",      "clear",   "keep",    "drop",
                                              "generate",  "replace",   "rename",  "sort",    "order",
                                              "egen",      "collapse",  "count",   "describe", "summarize",
                                              "tabulate",  "head",      "tail",    "save",    "append",
                                              "mvencode",  "reshape",  "do"};

// Command classification for do-file execution
// Transformation: modifies the CTE chain state
// Terminal: materializes the chain, returns the data frame
// Side-effect: returns a derived result (count, stats, freq table, file write)
static bool IsTransformationCommand(const string &command) {
	static const vector<string> TRANSFORMATION = {"use", "clear", "do", "keep", "drop", "generate", "replace",
	                                              "rename", "sort", "order", "egen", "collapse", "mvencode",
	                                              "reshape", "append"};
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
	if (lower == "first") return "FIRST";
	if (lower == "last") return "LAST";
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

// Check if expression contains _n or _N (which expand to window functions)
static bool ExpressionUsesRowVars(const string &expr) {
	std::regex row_var_re("\\b_[nN]\\b");
	return std::regex_search(expr, row_var_re);
}

static string TranslateExpression(const string &expr, const string &by_cols = "") {
	string result = expr;
	// log() -> LN()
	std::regex log_re("\\blog\\s*\\(");
	result = std::regex_replace(result, log_re, "LN(");
	// missing(x) -> (x IS NULL)
	std::regex missing_re("\\bmissing\\s*\\(([^)]+)\\)");
	result = std::regex_replace(result, missing_re, "($1 IS NULL)");
	// _n -> ROW_NUMBER() OVER (...)
	string partition = by_cols.empty() ? "" : "PARTITION BY " + by_cols + " ";
	std::regex n_re("\\b_n\\b");
	result = std::regex_replace(result, n_re, "ROW_NUMBER() OVER (" + partition + ")");
	// _N -> COUNT(*) OVER (...)
	std::regex N_re("\\b_N\\b");
	result = std::regex_replace(result, N_re, "COUNT(*) OVER (" + partition + ")");
	return result;
}

//===--------------------------------------------------------------------===//
// Process command: update state, return SQL
//===--------------------------------------------------------------------===//
static string ProcessCommand(const StataCommand &cmd, StataDoStateInfo &state) {
	if (cmd.command == "use") {
		state.Clear();
		string source = ExtractQuotedString(cmd.arguments);
		string read_expr = FileReadFunction(source);
		state.AddStep("SELECT * FROM " + read_expr);
		state.current_source = cmd.arguments;
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "clear") {
		state.Clear();
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

	if (!state.HasData()) {
		throw ParserException("No dataset in memory. Use 'use' to load data first.");
	}

	string prev = state.LatestStep();

	// --- Transformation commands ---
	if (cmd.command == "keep") {
		if (cmd.arguments.empty() && !cmd.condition.empty()) {
			string cond = TranslateExpression(cmd.condition);
			if (ExpressionUsesRowVars(cmd.condition)) {
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
			              TranslateExpression(cmd.condition));
		} else {
			throw ParserException("Invalid 'keep' syntax");
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "drop") {
		if (cmd.arguments.empty() && !cmd.condition.empty()) {
			string cond = TranslateExpression(cmd.condition);
			if (ExpressionUsesRowVars(cmd.condition)) {
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
		string sql_expr = TranslateExpression(expr);

		if (!cmd.condition.empty()) {
			string sql_cond = TranslateExpression(cmd.condition);
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
		string sql_expr = TranslateExpression(expr);

		if (!cmd.condition.empty()) {
			string sql_cond = TranslateExpression(cmd.condition);
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
			string sql_cond = TranslateExpression(cmd.condition, by_cols);
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
			sql += " WHERE " + TranslateExpression(cmd.condition, by_cols);
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
			sql += " WHERE " + TranslateExpression(cmd.condition);
		}
		return state.BuildQuery(sql);
	}

	if (cmd.command == "count") {
		string sql = "SELECT COUNT(*) AS n FROM " + prev;
		if (!cmd.condition.empty()) {
			sql += " WHERE " + TranslateExpression(cmd.condition);
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

	if (cmd.command == "describe") {
		return "SELECT column_name, column_type FROM (DESCRIBE " + state.BuildQuery("SELECT * FROM " + prev) + ")";
	}

	if (cmd.command == "summarize") {
		if (cmd.arguments.empty()) {
			throw ParserException("'summarize' requires at least one variable name");
		}
		string var = Trim(cmd.arguments);
		string where_clause;
		if (!cmd.condition.empty()) {
			where_clause = " WHERE " + TranslateExpression(cmd.condition);
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
			where_clause = " WHERE " + TranslateExpression(cmd.condition);
		}
		return state.BuildQuery("SELECT " + group_cols + ", COUNT(*) AS freq FROM " + prev + where_clause +
		                        " GROUP BY " + group_cols + " ORDER BY " + group_cols);
	}

	if (cmd.command == "save") {
		string filename = ExtractQuotedString(cmd.arguments);
		return "COPY (" + state.BuildQuery("SELECT * FROM " + prev) + ") TO '" + filename + "'";
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

//===--------------------------------------------------------------------===//
// parser_override: handles commands that conflict with SQL keywords
// (describe, summarize) — called BEFORE the standard parser
//===--------------------------------------------------------------------===//
static ParserOverrideResult stata_do_parser_override(ParserExtensionInfo *info, const string &query,
                                                     ParserOptions &options) {
	string trimmed = Trim(query);
	if (!trimmed.empty() && trimmed.back() == ';') {
		trimmed.pop_back();
		trimmed = Trim(trimmed);
	}
	string lower = StringUtil::Lower(trimmed);

	// Only intercept describe/summarize when we have data loaded
	auto &state = dynamic_cast<StataDoStateInfo &>(*info);
	if (!state.HasData()) {
		return ParserOverrideResult();
	}

	bool is_ours = false;
	if (StringUtil::StartsWith(lower, "describe") &&
	    (lower.size() == 8 || lower[8] == ' ' || lower[8] == ';')) {
		is_ours = true;
	}
	if (StringUtil::StartsWith(lower, "summarize") &&
	    (lower.size() == 9 || lower[9] == ' ' || lower[9] == ';')) {
		is_ours = true;
	}

	if (!is_ours) {
		return ParserOverrideResult();
	}

	try {
		auto cmd = TokenizeCommand(query);
		string sql = ProcessCommand(cmd, state);

		Parser parser;
		parser.ParseQuery(sql);
		return ParserOverrideResult(std::move(parser.statements));
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
