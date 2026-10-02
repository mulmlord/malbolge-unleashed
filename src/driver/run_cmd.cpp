#include "driver/run_cmd.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "core/classic.hpp"
#include "core/loader.hpp"
#include "core/vm.hpp"
#include "driver/debug_observer.hpp"
#include "driver/program_io.hpp"
#include "driver/stream_io.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm run [--classic] [--exit-a] [--max-cells N] [--stats]\n"
    "                [--trace [--trace-from N] [--trace-to M]] [--] <file>\n";

// Touched pages of the sparse memory; classic mode has a flat memory and reports none.
template <class VmT> std::optional<std::size_t> page_count(const VmT& vm) {
    if constexpr (requires { vm.memory().page_count(); }) {
        return vm.memory().page_count();
    } else {
        return std::nullopt;
    }
}

// A positive decimal integer fitting 64 bits (steps are 1-based).
std::optional<std::uint64_t> parse_step(std::string_view text) {
    std::uint64_t v = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() || v == 0) {
        return std::nullopt;
    }
    return v;
}

template <class VmT>
int run_vm(std::span<const std::uint8_t> cells, std::istream& in, std::ostream& out,
           std::ostream& err, bool exit_a, const DebugOptions& dbg) {
    VmT vm(cells);
    StreamIo io(in, out);
    if (!dbg.trace && !dbg.stats) {
        const core::RunResult r = vm.run(io);
        if (r.kind == core::RunResult::Kind::Fault) {
            err << "malm: " << core::format_fault(*r.fault) << '\n';
        }
        return core::exit_status(r, vm, exit_a);
    }
    DebugObserver obs(dbg, err);
    const auto t0 = std::chrono::steady_clock::now();
    const core::RunResult r = vm.run(io, obs);
    const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - t0;
    obs.flush_trace(); // before the diagnostic, so the two never interleave
    if (r.kind == core::RunResult::Kind::Fault) {
        err << "malm: " << core::format_fault(*r.fault) << '\n';
    }
    if (dbg.stats) {
        obs.print_stats(r.kind == core::RunResult::Kind::Halted,
                        r.fault ? core::error_name(r.fault->error) : std::string_view{}, vm.steps(),
                        vm.n(), page_count(vm), wall.count());
    }
    return core::exit_status(r, vm, exit_a);
}

} // namespace

int run_loaded(const LoadedProgram& prog, bool exit_a, std::istream& in, std::ostream& out,
               std::ostream& err, const DebugOptions& dbg) {
    exit_a = exit_a || prog.exit_a;
    return prog.classic ? run_vm<core::ClassicVm>(prog.cells, in, out, err, exit_a, dbg)
                        : run_vm<core::Vm>(prog.cells, in, out, err, exit_a, dbg);
}

int run_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                std::ostream& err) {
    std::string_view path;
    bool exit_a = false;
    bool classic = false;
    bool options_done = false;
    std::size_t max_cells = core::kDefaultMaxCells;
    bool max_cells_given = false;
    DebugOptions dbg;
    bool window_given = false;
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
        } else if (!options_done && arg == "--stats") {
            dbg.stats = true;
        } else if (!options_done && arg == "--trace") {
            dbg.trace = true;
        } else if (!options_done && (arg == "--trace-from" || arg == "--trace-to")) {
            const auto v = i + 1 < args.size() ? parse_step(args[i + 1]) : std::nullopt;
            if (!v) {
                err << "malm run: " << arg << " needs a positive integer\n" << kUsage;
                return 2;
            }
            ++i;
            (arg == "--trace-from" ? dbg.trace_from : dbg.trace_to) = *v;
            window_given = true;
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

    if (window_given && !dbg.trace) {
        err << "malm run: --trace-from and --trace-to need --trace\n" << kUsage;
        return 2;
    }
    if (dbg.trace_from > dbg.trace_to) {
        err << "malm run: --trace-from is greater than --trace-to\n" << kUsage;
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
    return run_loaded(prog, exit_a, in, out, err, dbg);
}

} // namespace malm
