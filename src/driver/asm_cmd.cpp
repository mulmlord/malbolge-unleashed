#include "driver/asm_cmd.hpp"

#include <ostream>
#include <string>

#include "asm/check.hpp"
#include "asm/parser.hpp"
#include "asm/printer.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm asm (--check | --print) [--] <file.mua>\n"
    "       malm asm [--text] [--map <file>] -o <out> [--] <file.mua>   (not implemented yet)\n"
    "  --check   parse and run the static checks; diagnostics on stderr, exit 0 or 1\n"
    "  --print   after the checks, print the program in canonical form on stdout\n";

int usage_error(std::ostream& err, std::string_view msg) {
    if (!msg.empty()) {
        err << "malm asm: " << msg << '\n';
    }
    err << kUsage;
    return 2;
}

} // namespace

int asm_command(std::span<const std::string_view> args, std::istream&, std::ostream& out,
                std::ostream& err) {
    std::string_view input;
    bool check = false;
    bool print = false;
    bool plain = false; // an option of the (not yet available) real assembly
    bool options_done = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (!options_done && arg == "--") {
            options_done = true;
        } else if (!options_done && arg == "--check") {
            check = true;
        } else if (!options_done && arg == "--print") {
            print = true;
        } else if (!options_done && arg == "--text") {
            plain = true;
        } else if (!options_done && (arg == "-o" || arg == "--map")) {
            if (i + 1 == args.size()) {
                return usage_error(err, "option '" + std::string(arg) + "' needs a value");
            }
            ++i;
            plain = true;
        } else if ((!options_done && arg.starts_with('-') && arg != "-") || !input.empty()) {
            return usage_error(err, "unexpected argument '" + std::string(arg) + "'");
        } else {
            input = arg;
        }
    }
    if (input.empty()) {
        return usage_error(err, "missing input file");
    }
    if (!check && !print) {
        err << "malm asm: assembly not implemented yet (#14-#16); use --check or --print\n";
        return 2;
    }
    if (plain) {
        return usage_error(err, "-o, --text and --map cannot be combined with --check or --print");
    }

    assembler::FsLoader loader;
    assembler::Diagnostics diags;
    const assembler::Program program = assembler::analyze(std::string(input), loader, diags);
    if (diags.has_errors()) {
        err << assembler::format_diagnostics(diags, program.files);
        return 1;
    }
    if (print) {
        out << assembler::print_program(program);
        out.flush();
        if (!out) {
            err << "malm: E_IO: cannot write to stdout\n";
            return 1;
        }
    }
    return 0;
}

} // namespace malm
