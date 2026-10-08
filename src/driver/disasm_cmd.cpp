#include "driver/disasm_cmd.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <ostream>
#include <string>
#include <vector>

#include "asm/disasm.hpp"
#include "asm/label_map.hpp"
#include "driver/program_io.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm disasm [--map <file>] [--raw] [--classic] [--max-cells N] [-o <out>] [--] <file>\n"
    "  --map <file>  labels and blocks from `malm asm --map`\n"
    "  --raw         list the program text cell by cell, even for a program from malm asm\n"
    "  -o <out>      write the disassembly to <out> (default and -o -: stdout)\n";

int usage_error(std::ostream& err, std::string_view msg) {
    if (!msg.empty()) {
        err << "malm disasm: " << msg << '\n';
    }
    err << kUsage;
    return 2;
}

} // namespace

int disasm_command(std::span<const std::string_view> args, std::istream&, std::ostream& out,
                   std::ostream& err) {
    std::string_view input;
    std::string_view output = "-";
    std::string_view map;
    bool have_output = false;
    bool have_map = false;
    bool raw = false;
    bool classic = false;
    bool options_done = false;
    std::size_t max_cells = core::kDefaultMaxCells;
    bool max_cells_given = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (!options_done && arg == "--") {
            options_done = true;
        } else if (!options_done && arg == "--raw") {
            raw = true;
        } else if (!options_done && arg == "--classic") {
            classic = true;
        } else if (!options_done && arg == "--max-cells") {
            const auto v = i + 1 < args.size() ? parse_max_cells(args[i + 1]) : std::nullopt;
            if (!v) {
                return usage_error(err, "--max-cells needs a positive integer");
            }
            ++i;
            max_cells = *v;
            max_cells_given = true;
        } else if (!options_done && (arg == "-o" || arg == "--map")) {
            if (i + 1 == args.size()) {
                return usage_error(err, "option '" + std::string(arg) + "' needs a value");
            }
            bool& have = arg == "-o" ? have_output : have_map;
            if (have) {
                return usage_error(err, std::string(arg) + " given twice");
            }
            have = true;
            (arg == "-o" ? output : map) = args[++i];
        } else if ((!options_done && arg.starts_with('-') && arg != "-") || !input.empty()) {
            return usage_error(err, "unexpected argument '" + std::string(arg) + "'");
        } else {
            input = arg;
        }
    }
    if (input.empty()) {
        return usage_error(err, "missing input file");
    }
    if (have_map && (raw || classic)) {
        return usage_error(err, "--map cannot be combined with --raw or --classic");
    }
    if (same_file(output, input) || (have_map && same_file(output, map))) {
        err << "malm disasm: refusing to overwrite the input file '"
            << (same_file(output, input) ? input : map) << "'\n";
        return 1;
    }
    if (max_cells_given) {
        if (const std::string msg = max_cells_limit_error(classic, max_cells); !msg.empty()) {
            return usage_error(err, msg);
        }
    }

    LoadedProgram prog;
    if (const int rc = load_program(input, classic, err, prog, max_cells); rc != 0) {
        return rc;
    }
    std::vector<assembler::MapEntry> entries;
    if (have_map) {
        std::vector<std::uint8_t> bytes;
        if (!read_file(map, bytes, err)) {
            return 1;
        }
        auto parsed =
            assembler::parse_label_map({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        if (!parsed) {
            err << "malm disasm: " << map << ": " << parsed.error() << '\n';
            return 1;
        }
        entries = std::move(*parsed);
    }
    assembler::DisasmOptions options;
    options.name = std::string(input);
    options.map = have_map ? &entries : nullptr;
    options.raw = raw;
    options.classic = prog.classic;
    const auto d = assembler::disassemble(prog.cells, options);
    if (!d) {
        err << "malm disasm: " << d.error() << '\n';
        return 1;
    }
    if (!d->warning.empty()) {
        err << "malm disasm: warning: " << d->warning << '\n';
    }

    if (output == "-") {
        out << d->text;
        out.flush();
        if (!out) {
            err << "malm: E_IO: cannot write to stdout\n";
            return 1;
        }
        return 0;
    }
    std::ofstream file{std::string(output), std::ios::binary | std::ios::trunc};
    if (file) {
        file << d->text;
        file.close();
    }
    if (!file) {
        err << "malm: cannot write '" << output << "': " << std::strerror(errno) << '\n';
        return 1;
    }
    return 0;
}

} // namespace malm
