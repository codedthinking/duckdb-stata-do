#pragma once

#include "string_utils.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dodo {

//===--------------------------------------------------------------------===//
// DodoCommand — parsed .do command representation
//===--------------------------------------------------------------------===//
struct DodoCommand {
	std::string command;
	std::string arguments;
	std::string condition;
	std::string options;
	std::string bysort_partition;  // PARTITION BY vars, comma-separated
	std::string bysort_order;     // ORDER BY vars, comma-separated
};

//===--------------------------------------------------------------------===//
// DodoState — CTE chain state for .do file to SQL translation
//===--------------------------------------------------------------------===//
struct DodoState {
	std::vector<std::string> cte_steps;
	std::vector<std::string> cte_commands;  //! Original command text for each step
	int step_counter = 0;
	std::string current_source;
	std::string pending_command;  //! Set before ProcessCommand, consumed by AddStep

	//! Redo stack: (command_text, cte_sql) pairs popped by undo
	std::vector<std::pair<std::string, std::string>> redo_stack;

	//! Variable labels: column_name -> label text
	std::unordered_map<std::string, std::string> variable_labels;
	//! Value label definitions: label_name -> {value -> text}
	std::unordered_map<std::string, std::unordered_map<int, std::string>> value_label_defs;
	//! Column-to-value-label mapping: column_name -> label_name
	std::unordered_map<std::string, std::string> column_labels;

	//! Tempfile names (registered via tempfile command)
	std::unordered_set<std::string> tempfile_names;
	//! Whether _tempfiles schema has been created
	bool tempfiles_schema_created = false;
	//! Tempfile tables created (for garbage collection on clear)
	std::vector<std::string> tempfile_tables;

	//! Panel structure (set by xtset/tsset)
	std::string panel_var;  // empty if pure time-series
	std::string time_var;   // empty if not set

	//! Whether dodo._current table exists (materialized use)
	bool materialized = false;

	//! Whether live view is enabled (set by extension, used by clear)
	bool live_view_enabled = false;

	//! Preserve checkpoint: index into cte_steps (-1 = no active preserve)
	int preserve_checkpoint = -1;
	int preserve_step_counter = -1;

	std::string LatestStep() const {
		return cte_steps.empty() ? "" : "_s" + std::to_string(step_counter - 1);
	}

	bool HasData() const {
		return !cte_steps.empty();
	}

	void AddStep(const std::string &inner_sql) {
		std::string step_name = "_s" + std::to_string(step_counter);
		cte_steps.push_back(step_name + " AS (" + inner_sql + ")");
		cte_commands.push_back(pending_command);
		step_counter++;
		// New step invalidates redo history
		redo_stack.clear();
	}

	std::string BuildCTEPrefix() const {
		if (cte_steps.empty()) {
			return "";
		}
		std::string result = "WITH ";
		for (idx_t i = 0; i < cte_steps.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += cte_steps[i];
		}
		result += " ";
		return result;
	}

	std::string BuildQuery(const std::string &final_select) const {
		return BuildCTEPrefix() + final_select;
	}

	//! Get SQL to drop tempfile tables, materialized table, and schemas
	std::string BuildCleanupSQL() const {
		std::string sql;
		for (auto &tbl : tempfile_tables) {
			sql += "DROP TABLE IF EXISTS " + tbl + "; ";
		}
		if (tempfiles_schema_created) {
			sql += "DROP SCHEMA IF EXISTS _tempfiles CASCADE; ";
		}
		if (materialized) {
			sql += "DROP TABLE IF EXISTS dodo._history; ";
			sql += "DROP TABLE IF EXISTS dodo._current; ";
			sql += "DROP SCHEMA IF EXISTS dodo; ";
		}
		return sql;
	}

	void Clear() {
		cte_steps.clear();
		cte_commands.clear();
		redo_stack.clear();
		step_counter = 0;
		current_source.clear();
		pending_command.clear();
		variable_labels.clear();
		value_label_defs.clear();
		column_labels.clear();
		tempfile_names.clear();
		preserve_checkpoint = -1;
		preserve_step_counter = -1;
		materialized = false;
	}

	void ClearAll() {
		Clear();
		tempfile_tables.clear();
		tempfiles_schema_created = false;
	}
};

//===--------------------------------------------------------------------===//
// Free function declarations
//===--------------------------------------------------------------------===//

//! List of recognized dodo commands
extern const std::vector<std::string> DODO_COMMANDS;

//! Check if a query string is a dodo command; if so, set command_out
bool IsDodoCommand(const std::string &query, std::string &command_out);

//! Returns true if the command transforms data (adds CTE step)
bool IsTransformationCommand(const std::string &command);

//! Returns true if the command has side effects (e.g., save, export)
bool IsSideEffectCommand(const std::string &command);

//! Parse a raw .do command string into structured DodoCommand
DodoCommand TokenizeCommand(const std::string &query);

//! Translate a Stata expression to SQL
std::string TranslateExpression(const std::string &expr, const std::string &by_cols = "",
                                const std::string &panel_var = "", const std::string &time_var = "",
                                const std::string &bysort_order = "");

//! Process a single parsed command, returning SQL (or empty string)
std::string ProcessCommand(const DodoCommand &cmd, DodoState &state);

//! Process an entire .do file, returning a vector of SQL statements
std::vector<std::string> ProcessDoFile(const std::string &filename, DodoState &state);

//! Extract a quoted string (strip surrounding quotes)
std::string ExtractQuotedString(const std::string &s);

//! Return the appropriate DuckDB read function for a file extension
std::string FileReadFunction(const std::string &filename);

} // namespace dodo
