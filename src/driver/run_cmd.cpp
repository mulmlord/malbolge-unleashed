#include "driver/run_cmd.hpp"

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "core/classic.hpp"
#include "core/loader.hpp"
#include "core/vm.hpp"
#include "driver/program_io.hpp"
#include "driver/stream_io.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm run [--classic] [--exit-a] [--max-cells N] [--] <file>\n";

template <class VmT>
int run_vm(std::span<const std::uint8_t> cells, std::istream& in, std::ostream& out,
           std::ostream& err, bool exit_a) {
    VmT vm(cells);
    StreamIo io(in, out);
    const core::RunResult r = vm.run(io);
    if (r.kind == core::RunResult::Kind::Fault) {
        err << "malm: " << core::format_fault(*r.fault) << '\n';
    }
    return core::exit_status(r, vm, exit_a);
}

} // namespace

int run_loaded(const LoadedProgram& prog, bool exit_a, std::istream& in, std::ostream& out,
               std::ostream& err) {
    exit_a = exit_a || prog.exit_a;
    return prog.classic ? run_vm<core::ClassicVm>(prog.cells, in, out, err, exit_a)
                        : run_vm<core::Vm>(prog.cells, in, out, err, exit_a);
}

int run_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                std::ostream& err) {
    std::string_view path;
    bool exit_a = false;
    bool classic = false;
    bool options_done = false;
    std::size_t max_cells = core::kDefaultMaxCells;
    bool max_cells_given = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (!options_done && arg == "--") {
            options_done = true;
        } else if (!options_done && arg == "--exit-a") {
            exit_a = true;
        } else if (!options_done && arg == "--classic") {
            classic = true;
        } else if (!options_done && arg == "--max-cells") {
            const auto v = i + 1 < args.size() ? parse_max_cells(args[i + 1]) : std::nullopt;
            if (!v) {
                err << "malm run: --max-cells needs a positive integer\n" << kUsage;
                return 2;
            }
            ++i;
            max_cells = *v;
            max_cells_given = true;
        } else if ((!options_done && arg.starts_with('-')) || !path.empty()) {
            err << "malm run: unexpected argument '" << arg << "'\n" << kUsage;
            return 2;
        } else {
            path = arg;
        }
    }
    if (path.empty()) {
        err << kUsage;
        return 2;
    }

    if (max_cells_given) {
        if (const std::string msg = max_cells_limit_error(classic, max_cells); !msg.empty()) {
            err << "malm run: " << msg << '\n' << kUsage;
            return 2;
        }
    }
    LoadedProgram prog;
    if (const int rc = load_program(path, classic, err, prog, max_cells); rc != 0) {
        return rc;
    }
    return run_loaded(prog, exit_a, in, out, err);
}

} // namespace malm
