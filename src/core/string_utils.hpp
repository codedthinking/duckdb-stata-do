#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace dodo {

using idx_t = std::size_t;

//===--------------------------------------------------------------------===//
// DodoException — replaces DuckDB's ParserException
//===--------------------------------------------------------------------===//
class DodoException : public std::runtime_error {
public:
	explicit DodoException(const std::string &msg) : std::runtime_error(msg) {
	}
};

//===--------------------------------------------------------------------===//
// String utilities — drop-in replacements for DuckDB's StringUtil
//===--------------------------------------------------------------------===//
namespace str {

inline void Trim(std::string &s) {
	auto start = s.find_first_not_of(" \t\n\r\f\v");
	if (start == std::string::npos) {
		s.clear();
		return;
	}
	auto end = s.find_last_not_of(" \t\n\r\f\v");
	s = s.substr(start, end - start + 1);
}

inline std::string Lower(const std::string &s) {
	std::string result = s;
	std::transform(result.begin(), result.end(), result.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return result;
}

inline bool StartsWith(const std::string &s, const std::string &prefix) {
	if (prefix.size() > s.size()) {
		return false;
	}
	return s.compare(0, prefix.size(), prefix) == 0;
}

inline bool EndsWith(const std::string &s, const std::string &suffix) {
	if (suffix.size() > s.size()) {
		return false;
	}
	return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline std::vector<std::string> Split(const std::string &s, char delim) {
	std::vector<std::string> result;
	std::string current;
	for (char c : s) {
		if (c == delim) {
			result.push_back(current);
			current.clear();
		} else {
			current += c;
		}
	}
	result.push_back(current);
	return result;
}

// SQL keywords that require quoting when used as identifiers
inline bool IsSQLKeyword(const std::string &s) {
	static const std::unordered_set<std::string> KEYWORDS = {
	    "abort",     "action",    "add",       "after",     "all",       "alter",     "always",
	    "analyze",   "and",       "as",        "asc",       "attach",    "autoincrement",
	    "before",    "begin",     "between",   "by",        "cascade",   "case",      "cast",
	    "check",     "collate",   "column",    "commit",    "conflict",  "constraint","create",
	    "cross",     "current",   "current_date", "current_time", "current_timestamp",
	    "database",  "default",   "deferrable","deferred",  "delete",    "desc",      "detach",
	    "distinct",  "do",        "drop",      "each",      "else",      "end",       "escape",
	    "except",    "exclude",   "exclusive", "exists",    "explain",   "fail",      "filter",
	    "first",     "following", "for",       "foreign",   "from",      "full",      "glob",
	    "group",     "groups",    "having",    "if",        "ignore",    "immediate", "in",
	    "index",     "indexed",   "initially", "inner",     "insert",    "instead",   "intersect",
	    "into",      "is",        "isnull",    "join",      "key",       "last",      "left",
	    "like",      "limit",     "match",     "materialized", "natural","no",        "not",
	    "nothing",   "notnull",   "null",      "nulls",     "of",        "offset",    "on",
	    "or",        "order",     "outer",     "over",      "partition", "plan",      "pragma",
	    "preceding", "primary",   "query",     "raise",     "range",     "recursive", "references",
	    "regexp",    "reindex",   "release",   "rename",    "replace",   "restrict",  "returning",
	    "right",     "rollback",  "row",       "rows",      "savepoint", "select",    "set",
	    "table",     "temp",      "temporary", "then",      "ties",      "to",        "transaction",
	    "trigger",   "unbounded", "union",     "unique",    "update",    "using",     "vacuum",
	    "values",    "view",      "virtual",   "when",      "where",     "window",    "with",
	    "without",
	};
	return KEYWORDS.count(Lower(s)) > 0;
}

inline bool NeedsQuoting(const std::string &s) {
	if (s.empty()) {
		return true;
	}
	if (IsSQLKeyword(s)) {
		return true;
	}
	// Must start with letter or underscore
	if (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_') {
		return true;
	}
	// Must contain only alphanumeric or underscore
	for (char c : s) {
		if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
			return true;
		}
	}
	return false;
}

inline std::string QuoteIdent(const std::string &s) {
	if (NeedsQuoting(s)) {
		std::string escaped = s;
		std::size_t pos = 0;
		while ((pos = escaped.find('"', pos)) != std::string::npos) {
			escaped.insert(pos, 1, '"');
			pos += 2;
		}
		return "\"" + escaped + "\"";
	}
	return s;
}

} // namespace str
} // namespace dodo
