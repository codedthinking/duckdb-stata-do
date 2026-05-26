#pragma once

#include "duckdb.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/planner/operator_extension.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// CTE Chain State — stored in ParserExtensionInfo (per database)
//===--------------------------------------------------------------------===//
struct StataDoStateInfo : public ParserExtensionInfo {
	vector<string> cte_steps;
	int step_counter = 0;
	string current_source;

	string LatestStep() const {
		return cte_steps.empty() ? "" : "_s" + to_string(step_counter - 1);
	}

	bool HasData() const {
		return !cte_steps.empty();
	}

	void AddStep(const string &inner_sql) {
		string step_name = "_s" + to_string(step_counter);
		cte_steps.push_back(step_name + " AS (" + inner_sql + ")");
		step_counter++;
	}

	string BuildCTEPrefix() const {
		if (cte_steps.empty()) {
			return "";
		}
		string result = "WITH ";
		for (idx_t i = 0; i < cte_steps.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += cte_steps[i];
		}
		result += " ";
		return result;
	}

	string BuildQuery(const string &final_select) const {
		return BuildCTEPrefix() + final_select;
	}

	void Clear() {
		cte_steps.clear();
		step_counter = 0;
		current_source.clear();
	}
};

//===--------------------------------------------------------------------===//
// Parse Data
//===--------------------------------------------------------------------===//
struct StataDoParseData : public ParserExtensionParseData {
	string raw_query;

	explicit StataDoParseData(string query) : raw_query(std::move(query)) {
	}

	unique_ptr<ParserExtensionParseData> Copy() const override {
		return make_uniq<StataDoParseData>(raw_query);
	}

	string ToString() const override {
		return raw_query;
	}
};

//===--------------------------------------------------------------------===//
// Bind state — passes the generated SQL statement between plan and bind
//===--------------------------------------------------------------------===//
class StataDoBindState : public ClientContextState {
public:
	explicit StataDoBindState(unique_ptr<SQLStatement> stmt)
	    : statement(std::move(stmt)) {
	}

	void QueryEnd() override {
		statement.reset();
	}

	unique_ptr<SQLStatement> statement;
};

//===--------------------------------------------------------------------===//
// Operator Extension
//===--------------------------------------------------------------------===//
BoundStatement stata_do_bind(ClientContext &context, Binder &binder, OperatorExtensionInfo *info,
                             SQLStatement &statement);

class StataDoOperatorExtension : public OperatorExtension {
public:
	StataDoOperatorExtension() {
		Bind = stata_do_bind;
	}

	std::string GetName() override {
		return "stata_do";
	}

	unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &deserializer) override {
		throw InternalException("stata_do operator should not be serialized");
	}
};

//===--------------------------------------------------------------------===//
// Extension class
//===--------------------------------------------------------------------===//
class StataDoExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
