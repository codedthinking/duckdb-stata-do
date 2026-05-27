#pragma once

#include "dodo_core.hpp"
#include "duckdb.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/planner/operator_extension.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// DodoStateInfo — wraps dodo::DodoState with DuckDB extension interfaces
//===--------------------------------------------------------------------===//
struct DodoStateInfo : public ParserExtensionInfo {
	dodo::DodoState core;

	//! Live view: CREATE OR REPLACE VIEW _dodo_data after each transformation
	bool live_view_enabled = false;

	// Convenience forwarders for the extension glue
	bool HasData() const { return core.HasData(); }
	std::string LatestStep() const { return core.LatestStep(); }
};

//===--------------------------------------------------------------------===//
// Parse Data
//===--------------------------------------------------------------------===//
struct DodoParseData : public ParserExtensionParseData {
	string raw_query;

	explicit DodoParseData(string query) : raw_query(std::move(query)) {
	}

	unique_ptr<ParserExtensionParseData> Copy() const override {
		return make_uniq<DodoParseData>(raw_query);
	}

	string ToString() const override {
		return raw_query;
	}
};

//===--------------------------------------------------------------------===//
// Bind state — passes the generated SQL statement between plan and bind
//===--------------------------------------------------------------------===//
class DodoBindState : public ClientContextState {
public:
	explicit DodoBindState(unique_ptr<SQLStatement> stmt)
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
BoundStatement dodo_bind(ClientContext &context, Binder &binder, OperatorExtensionInfo *info,
                         SQLStatement &statement);

class DodoOperatorExtension : public OperatorExtension {
public:
	DodoOperatorExtension() {
		Bind = dodo_bind;
	}

	std::string GetName() override {
		return "dodo";
	}

	unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &deserializer) override {
		throw InternalException("dodo operator should not be serialized");
	}
};

//===--------------------------------------------------------------------===//
// Extension class
//===--------------------------------------------------------------------===//
class DodoExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
