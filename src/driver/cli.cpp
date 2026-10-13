#include "driver/cli.hpp"

#include <algorithm>
#include <array>
#include <istream>
#include <ostream>

#include "driver/asm_cmd.hpp"
#include "driver/build_cmd.hpp"
#include "driver/disasm_cmd.hpp"
#include "driver/export_cmd.hpp"
#include "driver/hla_cmd.hpp"
#include "driver/pack_cmd.hpp"
#include "driver/run_cmd.hpp"
#include "driver/rvaot_cmd.hpp"
#include "driver/rvemu_cmd.hpp"
#include "driver/rvref_cmd.hpp"
#include "version.hpp"

namespace malm {
namespace {

using Handler = int (*)(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                        std::ostream& err);

struct Command {
    std::string_view name;
    std::string_view description;
    Handler handler;     // nullptr: not implemented yet
    bool hidden = false; // a debugging tool, left out of --help
};

// Single table of subcommands; later tickets plug in handlers here.
constexpr auto kCommands = std::to_array<Command>({
    {"run", "Run a .malm or Malbolge text program", run_command},
    {"asm", "Assemble a low-level .mua file into a .malm", asm_command},
    {"hla", "Compile a high-level .mhl file to .malm", hla_command},
    {"disasm", "Disassemble a .malm or Malbolge text file", disasm_command},
    {"build", "Build a C program or an RV32IM ELF into a .malm or an executable", build_command},
    {"pack", "Pack a .malm into a native executable", pack_command},
    {"export", "Convert between .malm and plain Malbolge text (--text | --malm)", export_command},
    {"rvref", "Run an RV32IM ELF on the C++ reference emulator", rvref_command, true},
    {"rvemu", "Write the HLA program of rv-emu, the RV32IM emulator", rvemu_command, true},
    {"rvaot", "Write the HLA program that rv-aot translates an RV32IM ELF into", rvaot_command,
     true},
});

void print_usage(std::ostream& os) {
    os << "usage: malm <command> [args...]\n"
          "       malm --help | --version\n\n"
          "commands:\n";
    std::size_t width = 0;
    for (const Command& c : kCommands) {
        width = std::max(width, c.hidden ? 0 : c.name.size());
    }
    for (const Command& c : kCommands) {
        if (c.hidden) {
            continue;
        }
        os << "  " << c.name;
        for (std::size_t i = c.name.size(); i < width + 2; ++i) {
            os << ' ';
        }
        os << c.description << '\n';
    }
}

} // namespace

int run_cli(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
            std::ostream& err) {
    if (args.empty()) {
        print_usage(err);
        return 2;
    }
    const std::string_view cmd = args.front();
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        print_usage(out);
        return 0;
    }
    if (cmd == "--version") {
        out << "malm " << MALM_VERSION << '\n';
        return 0;
    }
    for (const Command& c : kCommands) {
        if (c.name == cmd) {
            if (c.handler == nullptr) {
                err << "malm " << cmd << ": not implemented\n";
                return 2;
            }
            return c.handler(args.subspan(1), in, out, err);
        }
    }
    err << "malm: unknown command '" << cmd << "'\n"
        << "Run 'malm --help' for a list of commands.\n";
    return 2;
}

} // namespace malm
