#include "driver/cli.hpp"

#include <algorithm>
#include <array>
#include <istream>
#include <ostream>

#include "driver/run_cmd.hpp"
#include "version.hpp"

namespace malm {
namespace {

using Handler = int (*)(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                        std::ostream& err);

struct Command {
    std::string_view name;
    std::string_view description;
    Handler handler; // nullptr: not implemented yet
};

// Single table of subcommands; later tickets plug in handlers here.
constexpr auto kCommands = std::to_array<Command>({
    {"run", "Run a .malm program", run_command},
    {"asm", "Assemble a low-level .mua file to .malm", nullptr},
    {"hla", "Compile a high-level .mhl file to .malm", nullptr},
    {"disasm", "Disassemble a .malm or Malbolge text file", nullptr},
    {"build", "Build a C/C++/Rust/Zig source to .malm or an executable", nullptr},
    {"pack", "Pack a .malm into a native executable", nullptr},
    {"export", "Export a .malm as plain Malbolge text", nullptr},
});

void print_usage(std::ostream& os) {
    os << "usage: malm <command> [args...]\n"
          "       malm --help | --version\n\n"
          "commands:\n";
    std::size_t width = 0;
    for (const Command& c : kCommands) {
        width = std::max(width, c.name.size());
    }
    for (const Command& c : kCommands) {
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
