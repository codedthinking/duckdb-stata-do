#include "dodo_core.hpp"
#include "dta_reader.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>

namespace dodo {

using std::pair;
using std::string;
using std::to_string;
using std::unordered_map;
using std::unordered_set;
using std::vector;

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
// SQL Formatting
//===--------------------------------------------------------------------===//

// Format a SQL string with line breaks at major clause keywords.
// Only breaks at top-level (paren_depth 0), preserving subquery structure.
static string FormatInnerSQL(const string &sql, const string &indent) {
	if (sql.empty()) {
		return sql;
	}

	string result;
	idx_t len = sql.size();
	idx_t i = 0;
	int paren_depth = 0;
	bool in_quote = false;
	bool first_clause = true;

	// Clause keywords that trigger new lines (longest match first)
	static const vector<string> CLAUSE_KW = {
	    "UNION ALL BY NAME ", "UNION ALL ",
	    "SELECT DISTINCT ",   "SELECT ",
	    "FULL OUTER JOIN ",   "LEFT JOIN ",
	    "INNER JOIN ",        "FROM ",
	    "WHERE ",             "GROUP BY ",
	    "ORDER BY ",          "HAVING ",
	    "LIMIT ",             "USING "};

	while (i < len) {
		char c = sql[i];

		// Track single quotes (handle escaped '' too)
		if (c == '\'') {
			if (!in_quote) {
				in_quote = true;
			} else if (i + 1 < len && sql[i + 1] == '\'') {
				result += c;
				i++;
			} else {
				in_quote = false;
			}
			result += c;
			i++;
			continue;
		}
		if (in_quote) {
			result += c;
			i++;
			continue;
		}

		// Track parens
		if (c == '(') {
			paren_depth++;
			result += c;
			i++;
			continue;
		}
		if (c == ')') {
			paren_depth--;
			result += c;
			i++;
			continue;
		}

		// Only match keywords at paren depth 0 and word boundary
		if (paren_depth == 0 && (i == 0 || sql[i - 1] == ' ' || sql[i - 1] == '\n')) {
			bool matched = false;
			for (auto &kw : CLAUSE_KW) {
				if (i + kw.size() <= len) {
					bool kw_match = true;
					for (idx_t j = 0; j < kw.size(); j++) {
						if (toupper(sql[i + j]) != kw[j]) {
							kw_match = false;
							break;
						}
					}
					if (kw_match) {
						if (!first_clause) {
							// Trim trailing whitespace
							while (!result.empty() && result.back() == ' ') {
								result.pop_back();
							}
							result += "\n" + indent;
						}
						result += kw;
						i += kw.size();
						first_clause = false;
						matched = true;
						break;
					}
				}
			}
			if (matched) {
				continue;
			}
		}

		result += c;
		i++;
	}

	return result;
}

// DodoState::BuildCTEPrefix — format-aware CTE assembly
string DodoState::BuildCTEPrefix() const {
	if (cte_steps.empty()) {
		return "";
	}
	if (format_sql) {
		string result = "WITH\n";
		for (idx_t i = 0; i < cte_steps.size(); i++) {
			if (i > 0) {
				result += ",\n";
			}
			if (sql_comments && i < cte_commands.size() && !cte_commands[i].empty()) {
				result += "  -- [source] " + cte_commands[i] + "\n";
			}
			result += "  _s" + to_string(i) + " AS (\n";
			result += "    " + FormatInnerSQL(cte_steps[i], "    ") + "\n";
			result += "  )";
		}
		result += "\n";
		return result;
	}
	// Unformatted (compact)
	string result = "WITH ";
	for (idx_t i = 0; i < cte_steps.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += "_s" + to_string(i) + " AS (" + cte_steps[i] + ")";
	}
	result += " ";
	return result;
}

// DodoState::BuildQuery — CTE prefix + formatted final SELECT
string DodoState::BuildQuery(const string &final_select) const {
	if (format_sql) {
		return BuildCTEPrefix() + FormatInnerSQL(final_select, "");
	}
	return BuildCTEPrefix() + final_select;
}

//===--------------------------------------------------------------------===//
// Simple Expression Evaluator (for local x = expr, scalar x = expr)
//===--------------------------------------------------------------------===//

// Tokenizer for simple expressions
struct ExprToken {
	enum Type { NUMBER, OP, LPAREN, RPAREN, FUNC, END };
	Type type;
	double value;
	char op;
	string func_name;
};

static vector<ExprToken> TokenizeExpr(const string &expr) {
	vector<ExprToken> tokens;
	idx_t i = 0;
	while (i < expr.size()) {
		char c = expr[i];
		if (c == ' ' || c == '\t') {
			i++;
			continue;
		}
		if (c == '(') {
			tokens.push_back({ExprToken::LPAREN, 0, '(', ""});
			i++;
		} else if (c == ')') {
			tokens.push_back({ExprToken::RPAREN, 0, ')', ""});
			i++;
		} else if (c == '+' || c == '*' || c == '/' || c == '%') {
			tokens.push_back({ExprToken::OP, 0, c, ""});
			i++;
		} else if (c == '-') {
			// Unary minus: at start, after operator, or after left paren
			bool is_unary = tokens.empty() || tokens.back().type == ExprToken::OP ||
			                tokens.back().type == ExprToken::LPAREN;
			if (is_unary) {
				// Parse as part of number
				idx_t start = i;
				i++;
				while (i < expr.size() && (isdigit(expr[i]) || expr[i] == '.')) {
					i++;
				}
				if (i == start + 1) {
					// Just a minus sign followed by non-digit — treat as unary op
					tokens.push_back({ExprToken::OP, 0, 'n', ""}); // 'n' = negate
					continue;
				}
				tokens.push_back({ExprToken::NUMBER, std::stod(expr.substr(start, i - start)), 0, ""});
			} else {
				tokens.push_back({ExprToken::OP, 0, '-', ""});
				i++;
			}
		} else if (isdigit(c) || c == '.') {
			idx_t start = i;
			while (i < expr.size() && (isdigit(expr[i]) || expr[i] == '.' || expr[i] == 'e' || expr[i] == 'E')) {
				i++;
			}
			tokens.push_back({ExprToken::NUMBER, std::stod(expr.substr(start, i - start)), 0, ""});
		} else if (isalpha(c) || c == '_') {
			idx_t start = i;
			while (i < expr.size() && (isalnum(expr[i]) || expr[i] == '_')) {
				i++;
			}
			string name = expr.substr(start, i - start);
			// Check if followed by ( — then it's a function
			if (i < expr.size() && expr[i] == '(') {
				tokens.push_back({ExprToken::FUNC, 0, 0, name});
			} else {
				throw DodoException("Unknown identifier in expression: " + name);
			}
		} else {
			throw DodoException("Unexpected character in expression: " + string(1, c));
		}
	}
	tokens.push_back({ExprToken::END, 0, 0, ""});
	return tokens;
}

// Recursive descent parser for simple arithmetic
static idx_t expr_pos;
static double ParseExprAddSub(const vector<ExprToken> &tokens);

static double ParseExprAtom(const vector<ExprToken> &tokens) {
	auto &tok = tokens[expr_pos];
	if (tok.type == ExprToken::NUMBER) {
		expr_pos++;
		return tok.value;
	}
	if (tok.type == ExprToken::OP && tok.op == 'n') {
		// Unary negate
		expr_pos++;
		return -ParseExprAtom(tokens);
	}
	if (tok.type == ExprToken::FUNC) {
		string fname = str::Lower(tok.func_name);
		expr_pos++; // skip func name
		if (tokens[expr_pos].type != ExprToken::LPAREN) {
			throw DodoException("Expected '(' after function " + fname);
		}
		expr_pos++; // skip (
		double arg = ParseExprAddSub(tokens);
		if (tokens[expr_pos].type != ExprToken::RPAREN) {
			throw DodoException("Expected ')' after function argument");
		}
		expr_pos++; // skip )
		if (fname == "int" || fname == "floor") {
			return std::floor(arg);
		} else if (fname == "ceil") {
			return std::ceil(arg);
		} else if (fname == "round") {
			return std::round(arg);
		} else if (fname == "abs") {
			return std::abs(arg);
		} else if (fname == "sqrt") {
			return std::sqrt(arg);
		} else if (fname == "ln" || fname == "log") {
			return std::log(arg);
		} else if (fname == "exp") {
			return std::exp(arg);
		}
		throw DodoException("Unknown function in expression: " + fname);
	}
	if (tok.type == ExprToken::LPAREN) {
		expr_pos++; // skip (
		double val = ParseExprAddSub(tokens);
		if (tokens[expr_pos].type != ExprToken::RPAREN) {
			throw DodoException("Mismatched parentheses in expression");
		}
		expr_pos++; // skip )
		return val;
	}
	throw DodoException("Unexpected token in expression");
}

static double ParseExprMulDiv(const vector<ExprToken> &tokens) {
	double left = ParseExprAtom(tokens);
	while (tokens[expr_pos].type == ExprToken::OP &&
	       (tokens[expr_pos].op == '*' || tokens[expr_pos].op == '/' || tokens[expr_pos].op == '%')) {
		char op = tokens[expr_pos].op;
		expr_pos++;
		double right = ParseExprAtom(tokens);
		if (op == '*') {
			left *= right;
		} else if (op == '/') {
			if (right == 0) {
				throw DodoException("Division by zero in expression");
			}
			left /= right;
		} else {
			left = std::fmod(left, right);
		}
	}
	return left;
}

static double ParseExprAddSub(const vector<ExprToken> &tokens) {
	double left = ParseExprMulDiv(tokens);
	while (tokens[expr_pos].type == ExprToken::OP &&
	       (tokens[expr_pos].op == '+' || tokens[expr_pos].op == '-')) {
		char op = tokens[expr_pos].op;
		expr_pos++;
		double right = ParseExprMulDiv(tokens);
		if (op == '+') {
			left += right;
		} else {
			left -= right;
		}
	}
	return left;
}

double EvaluateSimpleExpr(const string &expr) {
	auto tokens = TokenizeExpr(Trim(expr));
	expr_pos = 0;
	double result = ParseExprAddSub(tokens);
	if (tokens[expr_pos].type != ExprToken::END) {
		throw DodoException("Unexpected trailing content in expression: " + expr);
	}
	return result;
}

// Format a double: use integer format if it's a whole number
static string FormatNumber(double val) {
	if (val == std::floor(val) && std::abs(val) < 1e15) {
		return to_string(static_cast<long long>(val));
	}
	// Use enough precision
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.12g", val);
	return string(buf);
}

//===--------------------------------------------------------------------===//
// Macro Function Evaluator
//===--------------------------------------------------------------------===//

string EvaluateMacroFunction(const string &func, const DodoState &state) {
	string trimmed = Trim(func);

	// :word count string
	if (str::StartsWith(str::Lower(trimmed), "word count ")) {
		string s = Trim(trimmed.substr(11));
		if (s.empty()) {
			return "0";
		}
		int count = 0;
		bool in_word = false;
		bool in_quotes = false;
		for (idx_t i = 0; i < s.size(); i++) {
			if (s[i] == '"') {
				if (!in_word) {
					in_word = true;
					count++;
				}
				in_quotes = !in_quotes;
			} else if (s[i] == ' ' && !in_quotes) {
				in_word = false;
			} else if (!in_word) {
				in_word = true;
				count++;
			}
		}
		return to_string(count);
	}

	// :word # of string
	if (str::StartsWith(str::Lower(trimmed), "word ")) {
		string rest = trimmed.substr(5);
		// Parse: # of string
		idx_t of_pos = str::Lower(rest).find(" of ");
		if (of_pos != string::npos) {
			int n = 0;
			try {
				n = std::stoi(Trim(rest.substr(0, of_pos)));
			} catch (...) {
				throw DodoException("Invalid word index in macro function: " + func);
			}
			string s = Trim(rest.substr(of_pos + 4));
			// Split into tokens (respecting quotes)
			vector<string> words;
			string current;
			bool in_quotes = false;
			for (idx_t i = 0; i < s.size(); i++) {
				if (s[i] == '"') {
					in_quotes = !in_quotes;
				} else if (s[i] == ' ' && !in_quotes) {
					if (!current.empty()) {
						words.push_back(current);
						current.clear();
					}
				} else {
					current += s[i];
				}
			}
			if (!current.empty()) {
				words.push_back(current);
			}
			if (n >= 1 && n <= static_cast<int>(words.size())) {
				return words[n - 1];
			}
			return "";
		}
	}

	// :variable label varname
	if (str::StartsWith(str::Lower(trimmed), "variable label ")) {
		string varname = Trim(trimmed.substr(15));
		auto it = state.variable_labels.find(varname);
		if (it != state.variable_labels.end()) {
			return it->second;
		}
		return "";
	}

	// :value label varname
	if (str::StartsWith(str::Lower(trimmed), "value label ")) {
		string varname = Trim(trimmed.substr(12));
		auto it = state.column_labels.find(varname);
		if (it != state.column_labels.end()) {
			return it->second;
		}
		return "";
	}

	// :label labelname # [#_2]
	if (str::StartsWith(str::Lower(trimmed), "label ")) {
		string rest = Trim(trimmed.substr(6));
		auto parts = str::Split(rest, ' ');
		if (parts.size() >= 2) {
			string label_name = parts[0];
			int value = 0;
			try {
				value = std::stoi(parts[1]);
			} catch (...) {
				return "";
			}
			auto it = state.value_label_defs.find(label_name);
			if (it != state.value_label_defs.end()) {
				auto vit = it->second.find(value);
				if (vit != it->second.end()) {
					string result = vit->second;
					// Optional max length
					if (parts.size() >= 3) {
						int maxlen = 0;
						try {
							maxlen = std::stoi(parts[2]);
						} catch (...) {
							maxlen = 0;
						}
						if (maxlen > 0 && static_cast<int>(result.size()) > maxlen) {
							result = result.substr(0, maxlen);
						}
					}
					return result;
				}
			}
			// Return the number itself if no label found
			return parts[1];
		}
	}

	// :type varname — we don't have runtime type info at compile time, return ""
	if (str::StartsWith(str::Lower(trimmed), "type ")) {
		return "";
	}

	// :display fmt expr — format a number
	if (str::StartsWith(str::Lower(trimmed), "display ")) {
		string rest = Trim(trimmed.substr(8));
		// Try to evaluate as expression
		try {
			double val = EvaluateSimpleExpr(rest);
			return FormatNumber(val);
		} catch (...) {
			return rest;
		}
	}

	throw DodoException("Unknown macro function: :" + func);
}

//===--------------------------------------------------------------------===//
// Macro Expansion
//===--------------------------------------------------------------------===//

string ExpandMacros(const string &text, const DodoState &state) {
	string result = text;
	int depth = 0;
	const int MAX_DEPTH = 50;

	while (depth < MAX_DEPTH) {
		string prev = result;
		string expanded;
		idx_t i = 0;

		while (i < result.size()) {
			// Local macro: `name'
			if (result[i] == '`') {
				// Find the matching closing quote, respecting nested backtick-quote pairs
				// For `...', we need to find the ' that balances the nesting
				idx_t end = string::npos;
				int bt_depth = 1; // We've seen one opening backtick
				for (idx_t j = i + 1; j < result.size(); j++) {
					if (result[j] == '`') {
						bt_depth++;
					} else if (result[j] == '\'') {
						bt_depth--;
						if (bt_depth == 0) {
							end = j;
							break;
						}
					}
				}

				if (end != string::npos) {
					string content = result.substr(i + 1, end - i - 1);

					// If content contains nested backtick patterns, only expand
					// the innermost ones this iteration (skip this pattern)
					if (content.find('`') != string::npos) {
						expanded += result[i];
						i++;
						continue;
					}

					// `=expr' — inline expression evaluation
					if (!content.empty() && content[0] == '=') {
						string expr = content.substr(1);
						try {
							double val = EvaluateSimpleExpr(expr);
							expanded += FormatNumber(val);
						} catch (...) {
							expanded += expr;
						}
						i = end + 1;
						continue;
					}

					// `:macro_fcn' — macro function
					if (!content.empty() && content[0] == ':') {
						string func = content.substr(1);
						try {
							expanded += EvaluateMacroFunction(func, state);
						} catch (...) {
							expanded += "";
						}
						i = end + 1;
						continue;
					}

					// Regular local macro: `name'
					bool valid = true;
					for (auto c : content) {
						if (!isalnum(c) && c != '_') {
							valid = false;
							break;
						}
					}
					if (valid) {
						auto it = state.local_macros.find(content);
						if (it != state.local_macros.end()) {
							expanded += it->second;
						}
						// If not found, expand to empty string (Stata behavior)
						i = end + 1;
						continue;
					}
				}
				expanded += result[i];
				i++;
				continue;
			}

			// Global macro: $name or ${name}
			if (result[i] == '$') {
				if (i + 1 < result.size() && result[i + 1] == '{') {
					// ${name}
					idx_t end = result.find('}', i + 2);
					if (end != string::npos) {
						string name = result.substr(i + 2, end - i - 2);
						auto it = state.global_macros.find(name);
						if (it != state.global_macros.end()) {
							expanded += it->second;
						}
						i = end + 1;
						continue;
					}
				} else if (i + 1 < result.size() && (isalpha(result[i + 1]) || result[i + 1] == '_')) {
					// $name
					idx_t start = i + 1;
					idx_t j = start;
					while (j < result.size() && (isalnum(result[j]) || result[j] == '_')) {
						j++;
					}
					string name = result.substr(start, j - start);
					auto it = state.global_macros.find(name);
					if (it != state.global_macros.end()) {
						expanded += it->second;
					}
					i = j;
					continue;
				}
				expanded += result[i];
				i++;
				continue;
			}

			expanded += result[i];
			i++;
		}

		result = expanded;

		// Bare scalar name substitution: replace standalone identifiers
		// that match scalar names with their values
		for (auto &[sname, sval] : state.scalars) {
			idx_t spos = 0;
			while ((spos = result.find(sname, spos)) != string::npos) {
				bool start_ok = (spos == 0 || (!isalnum(result[spos - 1]) && result[spos - 1] != '_'));
				bool end_ok = (spos + sname.size() >= result.size() ||
				               (!isalnum(result[spos + sname.size()]) && result[spos + sname.size()] != '_'));
				if (start_ok && end_ok) {
					result.replace(spos, sname.size(), sval);
					spos += sval.size();
				} else {
					spos += sname.size();
				}
			}
		}

		if (result == prev) {
			break; // No more expansions
		}
		depth++;
	}

	return result;
}

//===--------------------------------------------------------------------===//
// Numlist Parser
//===--------------------------------------------------------------------===//

vector<string> ParseNumlist(const string &spec) {
	vector<string> result;
	string trimmed = Trim(spec);

	// Split by spaces (each token can be a number, a/b, or a(d)b)
	auto tokens = str::Split(trimmed, ' ');
	for (auto &token : tokens) {
		string t = Trim(token);
		if (t.empty()) {
			continue;
		}

		// Check for a(d)b pattern
		idx_t paren_open = t.find('(');
		idx_t paren_close = t.find(')');
		if (paren_open != string::npos && paren_close != string::npos && paren_close > paren_open) {
			double start = std::stod(t.substr(0, paren_open));
			double step = std::stod(t.substr(paren_open + 1, paren_close - paren_open - 1));
			double end = std::stod(t.substr(paren_close + 1));
			if (step == 0) {
				throw DodoException("Step size cannot be zero in numlist: " + t);
			}
			if (step > 0) {
				for (double v = start; v <= end + 1e-10; v += step) {
					result.push_back(FormatNumber(v));
				}
			} else {
				for (double v = start; v >= end - 1e-10; v += step) {
					result.push_back(FormatNumber(v));
				}
			}
			continue;
		}

		// Check for a/b pattern (step = 1)
		idx_t slash_pos = t.find('/');
		if (slash_pos != string::npos && slash_pos > 0 && slash_pos < t.size() - 1) {
			int start = std::stoi(t.substr(0, slash_pos));
			int end = std::stoi(t.substr(slash_pos + 1));
			int step = (start <= end) ? 1 : -1;
			for (int v = start; (step > 0 ? v <= end : v >= end); v += step) {
				result.push_back(to_string(v));
			}
			continue;
		}

		// Plain number
		result.push_back(t);
	}

	return result;
}

//===--------------------------------------------------------------------===//
// Command Tokenizer
//===--------------------------------------------------------------------===//

const vector<string> DODO_COMMANDS = {
    "use",       "list",       "clear",    "keep",     "drop",       "generate",   "replace",
    "rename",    "sort",       "order",    "egen",     "collapse",   "count",      "describe",
    "summarize", "tabulate",   "head",     "tail",     "save",       "append",     "mvencode",
    "reshape",   "do",         "label",    "codebook", "duplicates", "expand",     "export",
    "import",    "merge",      "tempfile", "preserve", "restore",    "xtset",      "tsset",
    "bysort",    "by",         "undo",     "redo",     "history",    "show",       "local",
    "global",    "scalar",     "macro",    "display",  "foreach",    "forvalues",  "tempvar",
    "tempname"};

// Command classification for do-file execution
// Transformation: modifies the CTE chain state
// Terminal: materializes the chain, returns the data frame
// Side-effect: returns a derived result (count, stats, freq table, file write)
bool IsTransformationCommand(const string &command) {
	static const vector<string> TRANSFORMATION = {
	    "use",      "clear",    "do",        "keep",      "drop",   "generate", "replace",    "rename",
	    "sort",     "order",    "egen",      "collapse",  "mvencode","reshape",  "append",     "label",
	    "duplicates","expand",  "import",    "merge",     "tempfile","preserve", "restore",    "xtset",
	    "tsset",    "bysort",   "by",        "undo",      "redo",   "local",    "global",     "scalar",
	    "macro",    "foreach",  "forvalues", "tempvar",   "tempname"};
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
		if ((trimmed.front() == '"' && trimmed.back() == '"') || (trimmed.front() == '\'' && trimmed.back() == '\'')) {
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
		return "read_dta('" + filename + "')";
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
	if (lower == "mean")
		return "AVG";
	if (lower == "sd")
		return "STDDEV";
	if (lower == "count")
		return "COUNT";
	if (lower == "sum")
		return "SUM";
	if (lower == "min")
		return "MIN";
	if (lower == "max")
		return "MAX";
	if (lower == "median")
		return "MEDIAN";
	if (lower == "first" || lower == "firstnm")
		return "FIRST";
	if (lower == "last" || lower == "lastnm")
		return "LAST";
	return func_name; // pass through unknown functions
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

string TranslateExpression(const string &expr, const string &by_cols, const string &panel_var, const string &time_var,
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
			string lag_expr = "CASE WHEN (" + time_var + " - LAG(" + time_var + ", " + to_string(n) + ") " + over +
			                  ") = " + to_string(n) + " THEN LAG(" + var + ", " + to_string(n) + ") " + over +
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
			string lead_expr = "CASE WHEN (LEAD(" + time_var + ", " + to_string(n) + ") " + over + " - " + time_var +
			                   ") = " + to_string(n) + " THEN LEAD(" + var + ", " + to_string(n) + ") " + over +
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
		    {"sum", "SUM"}, {"mean", "AVG"}, {"count", "COUNT"}, {"min", "MIN"}, {"max", "MAX"}};
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
				string replacement =
				    sql_fn + "(" + args + ") OVER (" + partition + order_clause + "ROWS UNBOUNDED PRECEDING)";
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

		// Extract variable labels from .dta files
		if (str::EndsWith(str::Lower(source), ".dta")) {
			try {
				dta::DtaReader reader(source);
				for (auto &col : reader.Columns()) {
					if (!col.label.empty()) {
						state.variable_labels[col.name] = col.label;
					}
				}
			} catch (...) {
				// Ignore errors — labels are best-effort
			}
		}

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

	if (cmd.command == "show") {
		// show sql — return the formatted SQL as text
		string lower_args = str::Lower(Trim(cmd.arguments));
		if (lower_args != "sql" && lower_args != "query") {
			throw DodoException("'show' supports: show sql");
		}
		if (!state.HasData()) {
			throw DodoException("No dataset in memory. Use 'use' to load data first.");
		}
		string prev_step = state.LatestStep();
		// Temporarily force formatting for show sql output
		bool saved_format = state.format_sql;
		bool saved_comments = state.sql_comments;
		state.format_sql = true;
		state.sql_comments = true;
		string full_sql = state.BuildQuery("SELECT * FROM " + prev_step);
		state.format_sql = saved_format;
		state.sql_comments = saved_comments;
		// Escape single quotes for SQL string literal
		string escaped = full_sql;
		size_t pos = 0;
		while ((pos = escaped.find('\'', pos)) != string::npos) {
			escaped.replace(pos, 1, "''");
			pos += 2;
		}
		return "SELECT '" + escaped + "' AS sql";
	}

	//===--------------------------------------------------------------------===//
	// Macro / Scalar / Temp commands (do not require data)
	//===--------------------------------------------------------------------===//

	if (cmd.command == "local") {
		string args = cmd.arguments;
		if (!cmd.condition.empty()) {
			args += " if " + cmd.condition;
		}
		if (!cmd.options.empty()) {
			args += ", " + cmd.options;
		}
		args = Trim(args);

		if (args.empty()) {
			throw DodoException("'local' requires a macro name");
		}

		// local ++name / local --name
		if (str::StartsWith(args, "++") || str::StartsWith(args, "--")) {
			bool increment = args[0] == '+';
			string name = Trim(args.substr(2));
			auto it = state.local_macros.find(name);
			double val = 0;
			if (it != state.local_macros.end()) {
				try {
					val = std::stod(it->second);
				} catch (...) {
					throw DodoException("Cannot increment/decrement non-numeric macro: " + name);
				}
			}
			val += increment ? 1 : -1;
			state.local_macros[name] = FormatNumber(val);
			return "SELECT 'OK' AS status";
		}

		// Split: name [= expr | "string" | value...]
		idx_t space = args.find(' ');
		idx_t eq = args.find('=');

		if (space == string::npos && eq == string::npos) {
			// local name — set to empty
			state.local_macros[args] = "";
			return "SELECT 'OK' AS status";
		}

		string name;
		string value;

		if (eq != string::npos && (space == string::npos || eq < space || (eq == space + 1))) {
			// local name = expr (or local name= expr)
			name = Trim(args.substr(0, eq));
			// Remove trailing space from name if "name ="
			name = Trim(name);
			value = Trim(args.substr(eq + 1));
			// Check if it's a quoted string
			if ((value.front() == '"' && value.back() == '"') ||
			    (value.size() >= 4 && value.substr(0, 2) == "`\"" && value.substr(value.size() - 2) == "\"'")) {
				state.local_macros[name] = ExtractQuotedString(value);
			} else {
				// Evaluate as numeric expression
				try {
					double result = EvaluateSimpleExpr(value);
					state.local_macros[name] = FormatNumber(result);
				} catch (...) {
					// If evaluation fails, store as literal text
					state.local_macros[name] = value;
				}
			}
		} else {
			// local name value... (literal text assignment)
			name = Trim(args.substr(0, space));
			value = Trim(args.substr(space + 1));
			// Strip surrounding quotes if present
			if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
				value = value.substr(1, value.size() - 2);
			} else if (value.size() >= 4 && value.substr(0, 2) == "`\"" && value.substr(value.size() - 2) == "\"'") {
				value = value.substr(2, value.size() - 4);
			}
			state.local_macros[name] = value;
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "global") {
		string args = cmd.arguments;
		if (!cmd.condition.empty()) {
			args += " if " + cmd.condition;
		}
		if (!cmd.options.empty()) {
			args += ", " + cmd.options;
		}
		args = Trim(args);

		if (args.empty()) {
			throw DodoException("'global' requires a macro name");
		}

		// Split: name [= expr | "string" | value...]
		idx_t space = args.find(' ');
		idx_t eq = args.find('=');

		if (space == string::npos && eq == string::npos) {
			state.global_macros[args] = "";
			return "SELECT 'OK' AS status";
		}

		string name;
		string value;

		if (eq != string::npos && (space == string::npos || eq < space || (eq == space + 1))) {
			name = Trim(args.substr(0, eq));
			name = Trim(name);
			value = Trim(args.substr(eq + 1));
			if ((value.front() == '"' && value.back() == '"') ||
			    (value.size() >= 4 && value.substr(0, 2) == "`\"" && value.substr(value.size() - 2) == "\"'")) {
				state.global_macros[name] = ExtractQuotedString(value);
			} else {
				try {
					double result = EvaluateSimpleExpr(value);
					state.global_macros[name] = FormatNumber(result);
				} catch (...) {
					state.global_macros[name] = value;
				}
			}
		} else {
			name = Trim(args.substr(0, space));
			value = Trim(args.substr(space + 1));
			if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
				value = value.substr(1, value.size() - 2);
			} else if (value.size() >= 4 && value.substr(0, 2) == "`\"" && value.substr(value.size() - 2) == "\"'") {
				value = value.substr(2, value.size() - 4);
			}
			state.global_macros[name] = value;
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "macro") {
		string lower_args = str::Lower(Trim(cmd.arguments));
		if (str::StartsWith(lower_args, "drop ")) {
			string rest = Trim(cmd.arguments.substr(5));
			if (rest == "_all") {
				state.local_macros.clear();
				state.global_macros.clear();
			} else {
				auto names = str::Split(rest, ' ');
				for (auto &n : names) {
					string name = Trim(n);
					state.local_macros.erase(name);
					state.global_macros.erase(name);
				}
			}
			return "SELECT 'OK' AS status";
		}
		throw DodoException("'macro' supports: macro drop name [name ...] | macro drop _all");
	}

	if (cmd.command == "scalar") {
		string args = cmd.arguments;
		if (!cmd.condition.empty()) {
			args += " if " + cmd.condition;
		}
		if (!cmd.options.empty()) {
			args += ", " + cmd.options;
		}
		args = Trim(args);
		string lower_args = str::Lower(args);

		// scalar list [names | _all]
		if (str::StartsWith(lower_args, "list") || str::StartsWith(lower_args, "dir")) {
			string sql = "SELECT * FROM (VALUES ";
			bool has_rows = false;
			for (auto &[sn, sv] : state.scalars) {
				if (has_rows) {
					sql += ", ";
				}
				string escaped_val = sv;
				size_t qpos = 0;
				while ((qpos = escaped_val.find('\'', qpos)) != string::npos) {
					escaped_val.replace(qpos, 1, "''");
					qpos += 2;
				}
				sql += "('" + sn + "', '" + escaped_val + "')";
				has_rows = true;
			}
			if (!has_rows) {
				sql += "('(none)', '')";
			}
			sql += ") AS t(name, value) ORDER BY name";
			return sql;
		}

		// scalar drop names | _all
		if (str::StartsWith(lower_args, "drop ")) {
			string rest = Trim(args.substr(5));
			if (rest == "_all") {
				for (auto &[sn, sv] : state.scalars) {
					state.local_macros.erase(sn);
				}
				state.scalars.clear();
			} else {
				auto names = str::Split(rest, ' ');
				for (auto &n : names) {
					string sn = Trim(n);
					state.scalars.erase(sn);
					state.local_macros.erase(sn);
				}
			}
			return "SELECT 'OK' AS status";
		}

		// scalar [define] name = expr
		if (str::StartsWith(lower_args, "define ")) {
			args = Trim(args.substr(7));
		}

		idx_t eq_pos = args.find('=');
		if (eq_pos == string::npos) {
			throw DodoException("'scalar' requires: scalar name = expression");
		}
		string name = Trim(args.substr(0, eq_pos));
		string expr = Trim(args.substr(eq_pos + 1));

		string value;
		// Check for string scalar
		if (expr.size() >= 2 && expr.front() == '"' && expr.back() == '"') {
			value = expr.substr(1, expr.size() - 2);
		} else {
			// Replace known scalar names with their values in the expression
			string expanded_expr = expr;
			for (auto &[sname, sval] : state.scalars) {
				idx_t spos = 0;
				while ((spos = expanded_expr.find(sname, spos)) != string::npos) {
					bool start_ok = (spos == 0 || (!isalnum(expanded_expr[spos - 1]) && expanded_expr[spos - 1] != '_'));
					bool end_ok = (spos + sname.size() >= expanded_expr.size() ||
					               (!isalnum(expanded_expr[spos + sname.size()]) && expanded_expr[spos + sname.size()] != '_'));
					if (start_ok && end_ok) {
						expanded_expr.replace(spos, sname.size(), sval);
						spos += sval.size();
					} else {
						spos += sname.size();
					}
				}
			}
			try {
				double result = EvaluateSimpleExpr(expanded_expr);
				value = FormatNumber(result);
			} catch (...) {
				value = expr;
			}
		}

		state.scalars[name] = value;
		state.local_macros[name] = value;  // scalars are also accessible as locals
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "tempvar") {
		string args = Trim(cmd.arguments);
		if (args.empty()) {
			throw DodoException("'tempvar' requires at least one name");
		}
		auto names = str::Split(args, ' ');
		for (auto &n : names) {
			string lclname = Trim(n);
			string tmpname = "__dodo_tmp_" + to_string(state.temp_counter++);
			state.local_macros[lclname] = tmpname;
			state.tempvar_columns.push_back(tmpname);
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "tempname") {
		string args = Trim(cmd.arguments);
		if (args.empty()) {
			throw DodoException("'tempname' requires at least one name");
		}
		auto names = str::Split(args, ' ');
		for (auto &n : names) {
			string lclname = Trim(n);
			string tmpname = "__dodo_tmp_" + to_string(state.temp_counter++);
			state.local_macros[lclname] = tmpname;
			state.tempname_names.push_back(tmpname);
		}
		return "SELECT 'OK' AS status";
	}

	if (cmd.command == "display") {
		string args = cmd.arguments;
		if (!cmd.options.empty()) {
			args += ", " + cmd.options;
		}
		args = Trim(args);
		// Strip quotes
		if (args.size() >= 2 && args.front() == '"' && args.back() == '"') {
			args = args.substr(1, args.size() - 2);
		}
		// Escape for SQL
		string escaped = args;
		size_t pos = 0;
		while ((pos = escaped.find('\'', pos)) != string::npos) {
			escaped.replace(pos, 1, "''");
			pos += 2;
		}
		return "SELECT '" + escaped + "' AS display";
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
			state.AddStep("SELECT " + col_list + " FROM " + prev + " WHERE " + TrExpr(cmd.condition));
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
			state.AddStep("SELECT *, CASE WHEN " + sql_cond + " THEN " + sql_expr + " ELSE NULL END AS " + var_name +
			              " FROM " + prev);
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
			state.AddStep("SELECT *, CASE WHEN " + sql_cond + " THEN " + window_expr + " ELSE NULL END AS " + var_name +
			              " FROM " + prev);
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
		string current_func = "mean"; // default aggregate function
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
			bool has_assignment = (eq_pos != string::npos && (first_paren == string::npos || eq_pos < first_paren));

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
			sql += " ORDER BY " + by_cols;
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
		string keep_filter;    // empty = keep all
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
		if (keep_filter.find("_merge = 1") == string::npos && keep_filter.find("_merge = 2") == string::npos &&
		    !keep_filter.empty()) {
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
		string join_sql = "SELECT * EXCLUDE (_m_tag, _u_tag), " + merge_expr + " FROM (" + master_sql +
		                  ") AS _master " + join_type + " (" + using_sql + ") AS _using USING (" + using_clause + ")";

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
			              "SELECT *, ROW_NUMBER() OVER (PARTITION BY " +
			              col_list + ") AS _dedup_rn FROM " + prev + ") WHERE _dedup_rn = 1");
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
			              "SELECT t.*, g.generate_series AS _expand_idx FROM " +
			              prev + " t, LATERAL GENERATE_SERIES(1, " + n_expr + ") g)");
		} else {
			// generate(newvar) — newvar is 0 for original row, 1 for copies
			state.AddStep("SELECT * EXCLUDE (_expand_idx), "
			              "CASE WHEN _expand_idx = 1 THEN 0 ELSE 1 END AS " +
			              QuoteIdent(gen_var) + " FROM (SELECT t.*, g.generate_series AS _expand_idx FROM " + prev +
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
		return state.BuildQuery("SELECT * EXCLUDE (_rn, _total) FROM ("
		                        "SELECT *, ROW_NUMBER() OVER () AS _rn, COUNT(*) OVER () AS _total FROM " +
		                        prev + ") sub WHERE _rn > _total - " + n);
	}

	if (cmd.command == "describe" || cmd.command == "codebook") {
		// codebook is an alias for describe that doesn't conflict with SQL
		// Build a query that shows column metadata including labels
		// Base: column_name, column_type from DESCRIBE
		// Enhanced: add variable_label and value_label columns from state
		string base_sql =
		    "SELECT column_name, column_type FROM (DESCRIBE " + state.BuildQuery("SELECT * FROM " + prev) + ")";

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

		return "SELECT column_name, column_type, " + var_label_expr + " AS variable_label, " + val_label_expr +
		       " AS value_label FROM (DESCRIBE " + state.BuildQuery("SELECT * FROM " + prev) + ")";
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
		             "COUNT(" +
		             var +
		             ") AS N, "
		             "AVG(" +
		             var +
		             ") AS mean, "
		             "STDDEV(" +
		             var +
		             ") AS sd, "
		             "MIN(" +
		             var +
		             ") AS min, "
		             "PERCENTILE_CONT(0.25) WITHIN GROUP (ORDER BY " +
		             var +
		             ") AS p25, "
		             "PERCENTILE_CONT(0.50) WITHIN GROUP (ORDER BY " +
		             var +
		             ") AS p50, "
		             "PERCENTILE_CONT(0.75) WITHIN GROUP (ORDER BY " +
		             var +
		             ") AS p75, "
		             "MAX(" +
		             var +
		             ") AS max "
		             "FROM " +
		             prev + where_clause;
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
		bool save_as_table = (lower_opts.find("table") != string::npos || lower_opts.find("memory") != string::npos);

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
		} else if (str::EndsWith(lower_fn, ".dta")) {
			format_clause = " (FORMAT DTA)";
		}

		string pre_save_sql;
		// For .dta: apply variable labels as COMMENT ON COLUMN on materialized table
		if (str::EndsWith(lower_fn, ".dta") && state.materialized && !state.variable_labels.empty()) {
			for (auto &[col, label] : state.variable_labels) {
				string escaped_label = label;
				size_t pos = 0;
				while ((pos = escaped_label.find('\'', pos)) != string::npos) {
					escaped_label.replace(pos, 1, "''");
					pos += 2;
				}
				pre_save_sql += "COMMENT ON COLUMN dodo._current." + col + " IS '" + escaped_label + "'; ";
			}
		}

		return pre_save_sql + "COPY (" + state.BuildQuery("SELECT * FROM " + prev) + ") TO '" + target + "'" + format_clause;
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
		string mv_value = "0"; // default
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
				state.AddStep("SELECT * REPLACE (REPLACE(" + qj + ", '" + stub + "_', '') AS " + qj +
				              ") FROM (UNPIVOT " + prev + " ON COLUMNS('" + stub + "_.*') INTO NAME " + qj + " VALUE " +
				              qstub + ")");
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
				state.AddStep("SELECT * REPLACE (" + replace_expr + ") FROM (UNPIVOT " + prev + " ON " + on_clause +
				              " INTO NAME " + qj + " VALUE " + value_names + ")");
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
			return "__PIVOT__:CREATE OR REPLACE TEMP TABLE _dodo_pivot AS (PIVOT " + subquery + " ON " + qj +
			       " USING FIRST(" + QuoteIdent(value_var) + ") GROUP BY " + i_cols + ");" + "||STATE||_dodo_pivot";
		}
		return "SELECT 'OK' AS status";
	}

	throw DodoException("Unimplemented command: " + cmd.command);
}

//===--------------------------------------------------------------------===//
// Brace Block Accumulation (for foreach/forvalues)
//===--------------------------------------------------------------------===//

static vector<string> AccumulateBraceBlock(LineReader reader) {
	vector<string> body;
	int depth = 1;
	string line;
	while (reader(line)) {
		string trimmed = Trim(line);

		// Strip comments
		if (!trimmed.empty() && trimmed[0] == '*') {
			continue;
		}
		idx_t comment_pos = trimmed.find("//");
		if (comment_pos != string::npos) {
			if (comment_pos + 2 < trimmed.size() && trimmed[comment_pos + 2] == '/') {
				// /// continuation inside block — not typical but handle gracefully
				trimmed = Trim(trimmed.substr(0, comment_pos));
			} else {
				trimmed = Trim(trimmed.substr(0, comment_pos));
			}
		}

		if (trimmed.empty()) {
			continue;
		}

		// Track brace depth
		for (auto c : trimmed) {
			if (c == '{') {
				depth++;
			} else if (c == '}') {
				depth--;
			}
		}

		if (depth <= 0) {
			// The closing } line — don't add it to body
			// But if there's content before }, add it
			idx_t close_pos = trimmed.find('}');
			if (close_pos > 0) {
				string before = Trim(trimmed.substr(0, close_pos));
				if (!before.empty()) {
					body.push_back(before);
				}
			}
			break;
		}

		body.push_back(trimmed);
	}

	if (depth > 0) {
		throw DodoException("Unterminated brace block in foreach/forvalues");
	}

	return body;
}

//===--------------------------------------------------------------------===//
// Loop Parsing Helpers
//===--------------------------------------------------------------------===//

// Split a string into tokens respecting quotes
static vector<string> SplitTokens(const string &s) {
	vector<string> tokens;
	string current;
	bool in_quotes = false;
	for (idx_t i = 0; i < s.size(); i++) {
		if (s[i] == '"') {
			in_quotes = !in_quotes;
		} else if (s[i] == ' ' && !in_quotes) {
			if (!current.empty()) {
				tokens.push_back(current);
				current.clear();
			}
		} else {
			current += s[i];
		}
	}
	if (!current.empty()) {
		tokens.push_back(current);
	}
	return tokens;
}

// Parse foreach header and return (loop_var, values)
static pair<string, vector<string>> ParseForeachHeader(const string &header, const DodoState &state) {
	// Formats:
	//   foreach lname in list {
	//   foreach lname of local macname {
	//   foreach lname of global macname {
	//   foreach lname of numlist spec {
	string rest = Trim(header);

	// Strip trailing {
	if (!rest.empty() && rest.back() == '{') {
		rest = Trim(rest.substr(0, rest.size() - 1));
	}

	// Strip "foreach "
	if (str::StartsWith(str::Lower(rest), "foreach ")) {
		rest = Trim(rest.substr(8));
	}

	// First token is the loop variable name
	idx_t space = rest.find(' ');
	if (space == string::npos) {
		throw DodoException("foreach: missing list specification");
	}
	string lname = Trim(rest.substr(0, space));
	rest = Trim(rest.substr(space + 1));

	string lower_rest = str::Lower(rest);

	// foreach lname in list
	if (str::StartsWith(lower_rest, "in ")) {
		string list_str = Trim(rest.substr(3));
		return {lname, SplitTokens(list_str)};
	}

	// foreach lname of local/global/numlist
	if (str::StartsWith(lower_rest, "of ")) {
		rest = Trim(rest.substr(3));
		lower_rest = str::Lower(rest);

		if (str::StartsWith(lower_rest, "local ")) {
			string macname = Trim(rest.substr(6));
			auto it = state.local_macros.find(macname);
			if (it != state.local_macros.end()) {
				return {lname, SplitTokens(it->second)};
			}
			return {lname, {}}; // empty macro = zero iterations
		}
		if (str::StartsWith(lower_rest, "global ")) {
			string macname = Trim(rest.substr(7));
			auto it = state.global_macros.find(macname);
			if (it != state.global_macros.end()) {
				return {lname, SplitTokens(it->second)};
			}
			return {lname, {}};
		}
		if (str::StartsWith(lower_rest, "numlist ")) {
			string spec = Trim(rest.substr(8));
			return {lname, ParseNumlist(spec)};
		}

		throw DodoException("foreach: expected 'in', 'of local', 'of global', or 'of numlist'");
	}

	throw DodoException("foreach: expected 'in' or 'of' after variable name");
}

// Parse forvalues header: forvalues lname = range {
static pair<string, vector<string>> ParseForvaluesHeader(const string &header) {
	string rest = Trim(header);

	// Strip trailing {
	if (!rest.empty() && rest.back() == '{') {
		rest = Trim(rest.substr(0, rest.size() - 1));
	}

	// Strip "forvalues "
	if (str::StartsWith(str::Lower(rest), "forvalues ")) {
		rest = Trim(rest.substr(10));
	}

	// Parse: lname = range
	idx_t eq_pos = rest.find('=');
	if (eq_pos == string::npos) {
		throw DodoException("forvalues: expected '=' in range specification");
	}

	string lname = Trim(rest.substr(0, eq_pos));
	string range = Trim(rest.substr(eq_pos + 1));

	return {lname, ParseNumlist(range)};
}

//===--------------------------------------------------------------------===//
// ProcessLines: shared line-processing engine
//===--------------------------------------------------------------------===//

vector<string> ProcessLines(LineReader reader, DodoState &state, bool skip_terminal) {
	string line;
	bool in_block_comment = false;
	string continued_line;
	vector<string> side_effect_sql;

	// Process a single command line (used by main loop and loop iterations)
	auto process_command = [&](const string &trimmed) {
		string sub_command;
		if (!IsDodoCommand(trimmed, sub_command)) {
			return;
		}

		if (skip_terminal && !IsTransformationCommand(sub_command) && !IsSideEffectCommand(sub_command)) {
			return;
		}

		auto sub_cmd = TokenizeCommand(trimmed);
		state.pending_command = trimmed;
		string sql = ProcessCommand(sub_cmd, state);

		// In do-file context, use/import cannot materialize
		if ((sub_command == "use" || sub_command == "import") && state.materialized) {
			string source = ExtractQuotedString(sub_cmd.arguments);
			string read_expr = FileReadFunction(source);
			if (sub_command == "import") {
				string imp_rest = Trim(sub_cmd.arguments.substr(10));
				read_expr = "read_csv('" + ExtractQuotedString(imp_rest) + "')";
			}
			state.cte_steps.clear();
			state.step_counter = 0;
			state.AddStep("SELECT * FROM " + read_expr);
			state.materialized = false;
		}

		if (IsSideEffectCommand(sub_command)) {
			side_effect_sql.push_back(sql);
		}
	};

	// Execute a loop body with the given variable name bound to each value
	std::function<void(const string &, const vector<string> &, const vector<string> &)> execute_loop;
	execute_loop = [&](const string &lname, const vector<string> &values, const vector<string> &body) {
		for (auto &val : values) {
			state.local_macros[lname] = val;
			for (auto &body_line : body) {
				// Expand macros in body line
				string expanded = ExpandMacros(body_line, state);
				if (expanded.empty()) {
					continue;
				}

				string lower = str::Lower(expanded);

				// Nested foreach
				if (str::StartsWith(lower, "foreach ")) {
					auto [inner_lname, inner_values] = ParseForeachHeader(expanded, state);
					// Check if body is on same line (single-line loop)
					idx_t brace = expanded.find('{');
					idx_t close = expanded.find('}');
					vector<string> inner_body;
					if (brace != string::npos && close != string::npos && close > brace) {
						// Single-line: foreach x in a b { cmd }
						string inline_body = Trim(expanded.substr(brace + 1, close - brace - 1));
						if (!inline_body.empty()) {
							inner_body.push_back(inline_body);
						}
					}
					// Note: nested multi-line loops within a loop body are already accumulated
					// because the outer body was accumulated from the stream. Inner braces
					// within the body lines need special handling — for now, we support
					// single-line nested loops and multi-line via pre-accumulated body.
					execute_loop(inner_lname, inner_values, inner_body);
					continue;
				}

				// Nested forvalues
				if (str::StartsWith(lower, "forvalues ")) {
					auto [inner_lname, inner_values] = ParseForvaluesHeader(expanded);
					idx_t brace = expanded.find('{');
					idx_t close = expanded.find('}');
					vector<string> inner_body;
					if (brace != string::npos && close != string::npos && close > brace) {
						string inline_body = Trim(expanded.substr(brace + 1, close - brace - 1));
						if (!inline_body.empty()) {
							inner_body.push_back(inline_body);
						}
					}
					execute_loop(inner_lname, inner_values, inner_body);
					continue;
				}

				process_command(expanded);
			}
		}
	};

	while (reader(line)) {
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
				trimmed = Trim(trimmed.substr(0, block_start) + trimmed.substr(block_end + 2));
			} else {
				trimmed = Trim(trimmed.substr(0, block_start));
				in_block_comment = true;
			}
		}

		// Strip // line comments
		idx_t comment_pos = trimmed.find("//");
		if (comment_pos != string::npos) {
			if (comment_pos + 2 < trimmed.size() && trimmed[comment_pos + 2] == '/') {
				continued_line += Trim(trimmed.substr(0, comment_pos)) + " ";
				continue;
			}
			trimmed = Trim(trimmed.substr(0, comment_pos));
		}

		// Strip * line-start comments
		if (!trimmed.empty() && trimmed[0] == '*') {
			continue;
		}

		// Handle line continuation
		if (!continued_line.empty()) {
			trimmed = continued_line + trimmed;
			continued_line.clear();
		}

		if (trimmed.empty()) {
			continue;
		}

		// Strip trailing semicolons
		if (!trimmed.empty() && trimmed.back() == ';') {
			trimmed.pop_back();
			trimmed = Trim(trimmed);
		}
		if (trimmed.empty()) {
			continue;
		}

		// Expand macros before command recognition
		trimmed = ExpandMacros(trimmed, state);
		if (trimmed.empty()) {
			continue;
		}

		// Check for foreach/forvalues loops
		string lower = str::Lower(trimmed);
		if (str::StartsWith(lower, "foreach ")) {
			auto [lname, values] = ParseForeachHeader(trimmed, state);
			// Check for single-line loop: foreach x in a b { cmd }
			idx_t brace = trimmed.find('{');
			idx_t close = trimmed.rfind('}');
			vector<string> body;
			if (brace != string::npos && close != string::npos && close > brace + 1) {
				// Single-line: body is between { and }
				string inline_body = Trim(trimmed.substr(brace + 1, close - brace - 1));
				if (!inline_body.empty()) {
					body.push_back(inline_body);
				}
			} else {
				// Multi-line: accumulate until }
				body = AccumulateBraceBlock(reader);
			}
			execute_loop(lname, values, body);
			continue;
		}
		if (str::StartsWith(lower, "forvalues ")) {
			auto [lname, values] = ParseForvaluesHeader(trimmed);
			idx_t brace = trimmed.find('{');
			idx_t close = trimmed.rfind('}');
			vector<string> body;
			if (brace != string::npos && close != string::npos && close > brace + 1) {
				string inline_body = Trim(trimmed.substr(brace + 1, close - brace - 1));
				if (!inline_body.empty()) {
					body.push_back(inline_body);
				}
			} else {
				body = AccumulateBraceBlock(reader);
			}
			execute_loop(lname, values, body);
			continue;
		}

		process_command(trimmed);
	}

	return side_effect_sql;
}

//===--------------------------------------------------------------------===//
// ProcessDoFile: thin wrapper around ProcessLines
//===--------------------------------------------------------------------===//
vector<string> ProcessDoFile(const string &filename, DodoState &state) {
	std::ifstream file(filename);
	if (!file.is_open()) {
		throw DodoException("Cannot open file: " + filename);
	}
	string line;
	LineReader reader = [&](string &out) -> bool {
		if (std::getline(file, out)) {
			return true;
		}
		return false;
	};
	return ProcessLines(reader, state, /*skip_terminal=*/true);
}

} // namespace dodo
