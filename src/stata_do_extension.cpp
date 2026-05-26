#define DUCKDB_EXTENSION_MAIN

#include "stata_do_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/binder.hpp"

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
                                              "mvencode",  "reshape"};

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

static string TranslateExpression(const string &expr) {
	string result = expr;
	// log() -> LN()
	std::regex log_re("\\blog\\s*\\(");
	result = std::regex_replace(result, log_re, "LN(");
	// missing(x) -> (x IS NULL)
	std::regex missing_re("\\bmissing\\s*\\(([^)]+)\\)");
	result = std::regex_replace(result, missing_re, "($1 IS NULL)");
	// & -> AND (standalone, not && which is already valid)
	// Simple approach: replace & with AND when surrounded by spaces or at word boundaries
	// DuckDB actually handles & as bitwise AND, but Stata uses it as logical AND
	// For now, leave & as-is since DuckDB handles it in boolean context
	// | -> OR: same consideration
	// ! -> NOT: leave as-is, DuckDB doesn't support ! as NOT in SQL
	// Actually, let's do simple string replacements for the operators
	// We'll handle this more carefully later
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

	if (!state.HasData()) {
		throw ParserException("No dataset in memory. Use 'use' to load data first.");
	}

	string prev = state.LatestStep();

	// --- Transformation commands ---
	if (cmd.command == "keep") {
		if (cmd.arguments.empty() && !cmd.condition.empty()) {
			state.AddStep("SELECT * FROM " + prev + " WHERE " + TranslateExpression(cmd.condition));
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
			state.AddStep("SELECT * FROM " + prev + " WHERE NOT (" + TranslateExpression(cmd.condition) + ")");
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
		return state.BuildQuery("SELECT * FROM (SELECT *, ROW_NUMBER() OVER () AS _rn, COUNT(*) OVER () AS _total FROM " +
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
// parse_function: detect Stata commands
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

	// Register parser extension
	ParserExtension parser_ext;
	parser_ext.parse_function = stata_do_parse;
	parser_ext.plan_function = stata_do_plan;
	parser_ext.parser_info = make_shared_ptr<StataDoStateInfo>();
	ParserExtension::Register(config, parser_ext);

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
