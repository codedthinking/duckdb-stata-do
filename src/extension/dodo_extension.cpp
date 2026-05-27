#define DUCKDB_EXTENSION_MAIN

#include "dodo_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

// Global pointer to shared state, used by the live_view option callback
static DodoStateInfo *g_dodo_state = nullptr;

//===--------------------------------------------------------------------===//
// Build SQL for DuckDB-interactive features (history table, live view)
//===--------------------------------------------------------------------===//

static string BuildHistorySQL(const DodoStateInfo &state) {
	if (!state.core.materialized) {
		return "";
	}
	string sql = "CREATE OR REPLACE TABLE dodo._history AS SELECT * FROM (VALUES ";
	bool first = true;
	for (idx_t i = 0; i < state.core.cte_commands.size(); i++) {
		if (!first) {
			sql += ", ";
		}
		string escaped_cmd = state.core.cte_commands[i];
		size_t pos = 0;
		while ((pos = escaped_cmd.find('\'', pos)) != string::npos) {
			escaped_cmd.replace(pos, 1, "''");
			pos += 2;
		}
		sql += "(" + to_string(i) + ", '" + escaped_cmd + "', false)";
		first = false;
	}
	for (idx_t i = 0; i < state.core.redo_stack.size(); i++) {
		if (!first) {
			sql += ", ";
		}
		string escaped_cmd = state.core.redo_stack[state.core.redo_stack.size() - 1 - i].first;
		size_t pos = 0;
		while ((pos = escaped_cmd.find('\'', pos)) != string::npos) {
			escaped_cmd.replace(pos, 1, "''");
			pos += 2;
		}
		int step_id = static_cast<int>(state.core.cte_commands.size()) + static_cast<int>(i);
		sql += "(" + to_string(step_id) + ", '" + escaped_cmd + "', true)";
		first = false;
	}
	if (first) {
		return "CREATE OR REPLACE TABLE dodo._history (step_id INTEGER, command VARCHAR, undone BOOLEAN)";
	}
	sql += ") AS t(step_id, command, undone)";
	return sql;
}

static string BuildLiveViewSQL(const DodoStateInfo &state) {
	if (!state.live_view_enabled || !state.core.HasData()) {
		return "";
	}
	return "CREATE OR REPLACE VIEW _dodo_data AS (" +
	       state.core.BuildQuery("SELECT * FROM " + state.core.LatestStep()) + ")";
}

//===--------------------------------------------------------------------===//
// parse_function: detect commands (called when standard parser fails)
//===--------------------------------------------------------------------===//
static ParserExtensionParseResult dodo_parse(ParserExtensionInfo *info, const string &query) {
	std::string command;
	if (!dodo::IsDodoCommand(query, command)) {
		return ParserExtensionParseResult();
	}
	auto parse_data = make_uniq<DodoParseData>(query);
	return ParserExtensionParseResult(std::move(parse_data));
}

//===--------------------------------------------------------------------===//
// parser_override: handles commands that conflict with SQL keywords
//===--------------------------------------------------------------------===//
static ParserOverrideResult dodo_parser_override(ParserExtensionInfo *info, const string &query,
                                                     ParserOptions &options) {
	auto &state = dynamic_cast<DodoStateInfo &>(*info);

	// Split the full query into individual statements by ';'
	auto statements_str = StringUtil::Split(query, ';');

	bool has_dodo_commands = false;
	bool has_conflict_commands = false;
	for (auto &s : statements_str) {
		string trimmed = StringUtil::Lower(s);
		StringUtil::Trim(trimmed);
		if (trimmed.empty()) {
			continue;
		}

		if (StringUtil::StartsWith(trimmed, "use ") && (trimmed.find('"') != string::npos || trimmed.find('\'') != string::npos)) {
			has_conflict_commands = true;
		}
		if (StringUtil::StartsWith(trimmed, "import ")) {
			has_conflict_commands = true;
		}
		if (StringUtil::StartsWith(trimmed, "describe") || StringUtil::StartsWith(trimmed, "summarize")) {
			has_conflict_commands = true;
		}
		if (StringUtil::StartsWith(trimmed, "reshape") && trimmed.find("wide") != string::npos) {
			has_conflict_commands = true;
		}

		std::string cmd_name;
		// Need original (non-lowered) string for IsDodoCommand
		string orig = s;
		StringUtil::Trim(orig);
		if (dodo::IsDodoCommand(orig, cmd_name)) {
			has_dodo_commands = true;
		}
	}

	bool any_needs_override = has_conflict_commands && (has_dodo_commands || state.HasData());
	if (!any_needs_override && (state.live_view_enabled || state.core.materialized) && has_dodo_commands) {
		any_needs_override = true;
	}

	if (!any_needs_override) {
		return ParserOverrideResult();
	}

	try {
		vector<unique_ptr<SQLStatement>> all_statements;

		for (auto &s : statements_str) {
			string trimmed = s;
			StringUtil::Trim(trimmed);
			if (trimmed.empty()) {
				continue;
			}

			std::string cmd_name;
			if (dodo::IsDodoCommand(trimmed, cmd_name)) {
				auto cmd = dodo::TokenizeCommand(trimmed);
				state.core.pending_command = trimmed;
				string sql = dodo::ProcessCommand(cmd, state.core);

				// Handle __PIVOT__ marker
				if (StringUtil::StartsWith(sql, "__PIVOT__:")) {
					string rest = sql.substr(10);
					idx_t state_pos = rest.find("||STATE||");
					string pivot_sql = rest.substr(0, state_pos);
					string table_name = rest.substr(state_pos + 9);

					Parser parser;
					parser.ParseQuery(pivot_sql);
					state.core.AddStep("SELECT * FROM " + table_name);
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

				// Live view: inject view creation for UI data panel
				string view_sql = BuildLiveViewSQL(state);
				if (!view_sql.empty()) {
					Parser view_parser;
					view_parser.ParseQuery(view_sql);
					for (auto &stmt : view_parser.statements) {
						all_statements.push_back(std::move(stmt));
					}
				}

				// History table: inject rebuild for UI visibility
				string history_sql = BuildHistorySQL(state);
				if (!history_sql.empty()) {
					Parser hist_parser;
					hist_parser.ParseQuery(history_sql);
					for (auto &stmt : hist_parser.statements) {
						all_statements.push_back(std::move(stmt));
					}
				}
			} else {
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
static ParserExtensionPlanResult dodo_plan(ParserExtensionInfo *info, ClientContext &context,
                                               unique_ptr<ParserExtensionParseData> parse_data) {
	auto &dodo_data = dynamic_cast<DodoParseData &>(*parse_data);
	auto &state = dynamic_cast<DodoStateInfo &>(*info);

	auto cmd = dodo::TokenizeCommand(dodo_data.raw_query);
	state.core.pending_command = dodo_data.raw_query;
	string sql = dodo::ProcessCommand(cmd, state.core);

	Parser parser;
	try {
		parser.ParseQuery(sql);
	} catch (std::exception &ex) {
		throw BinderException("dodo: failed to parse generated SQL: %s\nSQL: %s", ex.what(), sql);
	}
	if (parser.statements.empty()) {
		throw BinderException("dodo: generated SQL produced no statements");
	}

	auto bind_state = make_shared_ptr<DodoBindState>(std::move(parser.statements[0]));
	context.registered_state->Remove("dodo_bind");
	context.registered_state->Insert("dodo_bind", bind_state);

	throw BinderException("dodo redirect to operator bind");
}

//===--------------------------------------------------------------------===//
// OperatorExtension::Bind — picks up stored statement and binds it
//===--------------------------------------------------------------------===//
BoundStatement dodo_bind(ClientContext &context, Binder &binder, OperatorExtensionInfo *info,
                             SQLStatement &statement) {
	auto bind_state = context.registered_state->Get<DodoBindState>("dodo_bind");
	if (!bind_state || !bind_state->statement) {
		return BoundStatement();
	}

	auto sql_binder = Binder::CreateBinder(context, &binder);
	auto result = sql_binder->Bind(*bind_state->statement);

	context.registered_state->Remove("dodo_bind");

	return result;
}

//===--------------------------------------------------------------------===//
// Extension Loading
//===--------------------------------------------------------------------===//

static void LoadInternal(ExtensionLoader &loader) {
	auto &instance = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(instance);

	auto shared_state = make_shared_ptr<DodoStateInfo>();
	g_dodo_state = shared_state.get();

	ParserExtension parser_ext;
	parser_ext.parse_function = dodo_parse;
	parser_ext.plan_function = dodo_plan;
	parser_ext.parser_override = dodo_parser_override;
	parser_ext.parser_info = shared_state;
	ParserExtension::Register(config, parser_ext);

	config.SetOptionByName("allow_parser_override_extension", Value("fallback"));

	auto operator_ext = make_shared_ptr<DodoOperatorExtension>();
	OperatorExtension::Register(config, operator_ext);

	config.AddExtensionOption(
	    "dodo_live_view",
	    "Create/replace _dodo_data view after each transformation (for DuckDB UI)",
	    LogicalType::BOOLEAN,
	    Value::BOOLEAN(false),
	    [](ClientContext &context, SetScope scope, Value &parameter) {
	        if (g_dodo_state) {
	            g_dodo_state->live_view_enabled = parameter.GetValue<bool>();
	            g_dodo_state->core.live_view_enabled = parameter.GetValue<bool>();
	        }
	    });
}

void DodoExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string DodoExtension::Name() {
	return "dodo";
}

std::string DodoExtension::Version() const {
#ifdef EXT_VERSION_DODO
	return EXT_VERSION_DODO;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(dodo, loader) {
	duckdb::LoadInternal(loader);
}
}
