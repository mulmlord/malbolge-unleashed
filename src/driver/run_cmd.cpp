#include "driver/run_cmd.hpp"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "core/classic.hpp"
#include "core/loader.hpp"
#include "core/vm.hpp"
#include "driver/debug_observer.hpp"
#include "driver/program_io.hpp"
#include "driver/snapshot_cmd.hpp"
#include "driver/stream_io.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm run [--classic] [--exit-a] [--max-cells N] [--no-snapshot] [--stats]\n"
    "                [--trace [--trace-from N] [--trace-to M]] [--] <file>\n";

// Touched pages of the sparse memory; classic mode has a flat memory and reports none.
template <class VmT> std::optional<std::size_t> page_count(const VmT& vm) {
    if constexpr (requires { vm.memory().page_count(); }) {
        return vm.memory().page_count();
    } else {
        return std::nullopt;
    }
}

template <class VmT>
int run_vm(std::span<const std::uint8_t> cells, std::istream& in, std::ostream& out,
           std::ostream& err, bool exit_a, const DebugOptions& dbg,
           const container::Snapshot* snapshot) {
    VmT vm(cells);
    if constexpr (std::same_as<VmT, core::Vm>) {
        if (snapshot != nullptr) {
            snapshot->restore(vm, cells);
        }
    }
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
               std::ostream& err, const DebugOptions& dbg, const container::Snapshot* snapshot) {
    exit_a = exit_a || prog.exit_a;
    return prog.classic ? run_vm<core::ClassicVm>(prog.cells, in, out, err, exit_a, dbg, nullptr)
                        : run_vm<core::Vm>(prog.cells, in, out, err, exit_a, dbg, snapshot);
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
    bool no_snapshot = false;
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
        } else if (!options_done && arg == "--no-snapshot") {
            no_snapshot = true;
        } else if (!options_done && arg == "--stats") {
            dbg.stats = true;
        } else if (!options_done && arg == "--trace") {
            dbg.trace = true;
        } else if (!options_done && (arg == "--trace-from" || arg == "--trace-to")) {
            const auto v = i + 1 < args.size() ? parse_count(args[i + 1]) : std::nullopt;
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
    // The trace and the statistics observe every step, so they run without a snapshot.
    const bool snapshot = !no_snapshot && !dbg.trace && !dbg.stats && has_snapshot(path);
    LoadedProgram prog;
    if (const int rc = load_program(path, classic, err, prog, max_cells, snapshot); rc != 0) {
        return rc;
    }
    const std::optional<container::Snapshot> snap =
        snapshot ? find_snapshot(path, prog, err) : std::nullopt;
    return run_loaded(prog, exit_a, in, out, err, dbg, snap ? &*snap : nullptr);
}

} // namespace malm
