#include "dodo_core.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static void print_usage(const char *prog) {
	std::cerr << "Usage: " << prog << " [OPTIONS] [INPUT_FILE]\n"
	          << "\n"
	          << "Compile .do files to SQL.\n"
	          << "\n"
	          << "Arguments:\n"
	          << "  INPUT_FILE           .do file to compile (default: stdin)\n"
	          << "\n"
	          << "Options:\n"
	          << "  -o, --output FILE    Write SQL to FILE (default: stdout)\n"
	          << "  --annotate           Emit original .do command as SQL comment\n"
	          << "  --terminal           Also emit SQL for terminal commands\n"
	          << "  -h, --help           Show this help message\n";
}

struct CliOptions {
	std::string input_file;   // empty = stdin
	std::string output_file;  // empty = stdout
	bool annotate = false;
	bool terminal = false;
};

static CliOptions parse_args(int argc, char *argv[]) {
	CliOptions opts;
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			print_usage(argv[0]);
			std::exit(0);
		} else if (arg == "-o" || arg == "--output") {
			if (i + 1 >= argc) {
				std::cerr << "Error: " << arg << " requires a filename\n";
				std::exit(1);
			}
			opts.output_file = argv[++i];
		} else if (arg == "--annotate") {
			opts.annotate = true;
		} else if (arg == "--terminal") {
			opts.terminal = true;
		} else if (arg[0] == '-') {
			std::cerr << "Unknown option: " << arg << "\n";
			print_usage(argv[0]);
			std::exit(1);
		} else {
			opts.input_file = arg;
		}
	}
	return opts;
}

// Process lines from an input stream (file or stdin)
static void process_stream(std::istream &input, dodo::DodoState &state,
                           std::vector<std::string> &side_effect_sql,
                           const CliOptions &opts) {
	std::string line;
	bool in_block_comment = false;
	std::string continued_line;

	while (std::getline(input, line)) {
		std::string trimmed = line;
		dodo::str::Trim(trimmed);

		// Handle block comments /* ... */
		if (in_block_comment) {
			auto end_pos = trimmed.find("*/");
			if (end_pos != std::string::npos) {
				in_block_comment = false;
				trimmed = trimmed.substr(end_pos + 2);
				dodo::str::Trim(trimmed);
			} else {
				continue;
			}
		}

		auto block_start = trimmed.find("/*");
		if (block_start != std::string::npos) {
			auto block_end = trimmed.find("*/", block_start + 2);
			if (block_end != std::string::npos) {
				trimmed = trimmed.substr(0, block_start) + trimmed.substr(block_end + 2);
				dodo::str::Trim(trimmed);
			} else {
				trimmed = trimmed.substr(0, block_start);
				dodo::str::Trim(trimmed);
				in_block_comment = true;
			}
		}

		// Strip // line comments
		auto comment_pos = trimmed.find("//");
		if (comment_pos != std::string::npos) {
			if (comment_pos + 2 < trimmed.size() && trimmed[comment_pos + 2] == '/') {
				// Line continuation ///
				auto before = trimmed.substr(0, comment_pos);
				dodo::str::Trim(before);
				continued_line += before + " ";
				continue;
			}
			trimmed = trimmed.substr(0, comment_pos);
			dodo::str::Trim(trimmed);
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
			dodo::str::Trim(trimmed);
		}
		if (trimmed.empty()) {
			continue;
		}

		std::string sub_command;
		if (!dodo::IsDodoCommand(trimmed, sub_command)) {
			continue;
		}

		// Skip terminal commands unless --terminal is set
		if (!dodo::IsTransformationCommand(sub_command) && !dodo::IsSideEffectCommand(sub_command)) {
			if (!opts.terminal) {
				continue;
			}
		}

		auto sub_cmd = dodo::TokenizeCommand(trimmed);
		state.pending_command = trimmed;
		std::string sql = dodo::ProcessCommand(sub_cmd, state);

		// Force lazy mode for use/import (no CREATE TABLE in CLI context)
		if ((sub_command == "use" || sub_command == "import") && state.materialized) {
			std::string source = dodo::ExtractQuotedString(sub_cmd.arguments);
			std::string read_expr = dodo::FileReadFunction(source);
			if (sub_command == "import") {
				auto rest = sub_cmd.arguments.substr(10);
				dodo::str::Trim(rest);
				read_expr = "read_csv('" + dodo::ExtractQuotedString(rest) + "')";
			}
			state.cte_steps.clear();
			state.cte_commands.clear();
			state.step_counter = 0;
			state.AddStep("SELECT * FROM " + read_expr);
			state.materialized = false;
		}

		// Collect side-effect SQL (save, export)
		if (dodo::IsSideEffectCommand(sub_command)) {
			side_effect_sql.push_back(sql);
		}

		// Terminal commands: emit their SQL directly
		if (opts.terminal && !dodo::IsTransformationCommand(sub_command) && !dodo::IsSideEffectCommand(sub_command)) {
			// Skip "SELECT 'OK' AS status" acknowledgments
			if (sql.find("SELECT 'OK' AS status") == std::string::npos) {
				side_effect_sql.push_back(sql);
			}
		}
	}
}

int main(int argc, char *argv[]) {
	auto opts = parse_args(argc, argv);

	dodo::DodoState state;
	std::vector<std::string> side_effect_sql;

	try {
		if (opts.input_file.empty()) {
			// Read from stdin
			process_stream(std::cin, state, side_effect_sql, opts);
		} else {
			std::ifstream input(opts.input_file);
			if (!input.is_open()) {
				std::cerr << "Error: cannot open file: " << opts.input_file << "\n";
				return 1;
			}
			process_stream(input, state, side_effect_sql, opts);
		}
	} catch (const dodo::DodoException &ex) {
		std::cerr << "Error: " << ex.what() << "\n";
		return 1;
	}

	// Open output
	std::ostream *out = &std::cout;
	std::ofstream out_file;
	if (!opts.output_file.empty()) {
		out_file.open(opts.output_file);
		if (!out_file.is_open()) {
			std::cerr << "Error: cannot open output file: " << opts.output_file << "\n";
			return 1;
		}
		out = &out_file;
	}

	// Emit the final CTE chain query if there is data
	if (state.HasData()) {
		if (opts.annotate) {
			for (size_t i = 0; i < state.cte_commands.size(); i++) {
				*out << "-- " << state.cte_commands[i] << "\n";
			}
		}
		*out << state.BuildQuery("SELECT * FROM " + state.LatestStep()) << ";\n";
	}

	// Emit side-effect SQL (save, export, terminal commands)
	for (auto &sql : side_effect_sql) {
		// Skip "SELECT 'OK' AS status" lines
		if (sql.find("SELECT 'OK' AS status") != std::string::npos) {
			continue;
		}
		*out << sql << ";\n";
	}

	return 0;
}
