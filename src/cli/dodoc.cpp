#include "dodo_core.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static const char *DODOC_VERSION = "0.2.0";

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
	          << "  -v, --version        Show version\n"
	          << "  -h, --help           Show this help message\n";
}

struct CliOptions {
	std::string input_file;  // empty = stdin
	std::string output_file; // empty = stdout
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
		} else if (arg == "-v" || arg == "--version") {
			std::cout << "dodoc " << DODOC_VERSION << "\n";
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

// Process lines from an input stream using shared ProcessLines engine
static void process_stream(std::istream &input, dodo::DodoState &state, std::vector<std::string> &side_effect_sql,
                           const CliOptions &opts) {
	dodo::LineReader reader = [&](std::string &out) -> bool {
		return !!std::getline(input, out);
	};
	auto results = dodo::ProcessLines(reader, state, /*skip_terminal=*/!opts.terminal);
	for (auto &sql : results) {
		side_effect_sql.push_back(sql);
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
