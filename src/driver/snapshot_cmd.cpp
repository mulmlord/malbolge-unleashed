#include "driver/snapshot_cmd.hpp"

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <vector>

#include "core/tables.hpp"
#include "core/vm.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm snapshot [--max-steps N] [--max-cells N] [--] <file>\n"
    "       malm snapshot --check [--max-cells N] [--] <file>\n"
    "       (writes or checks <file>.snap)\n";

int usage_error(std::ostream& err, std::string_view msg) {
    if (!msg.empty()) {
        err << "malm snapshot: " << msg << '\n';
    }
    err << kUsage;
    return 2;
}

// The instruction the VM executes next ("Out", ...); run_pure stopped before it.
std::string_view next_op(const core::Vm& vm) {
    const core::Value cell = vm.memory().read(vm.c());
    return core::op_name(core::decode(vm.c().mod94(), static_cast<std::uint32_t>(cell.raw())));
}

int write_command(std::string_view path, const LoadedProgram& prog,
                  std::optional<std::uint64_t> max_steps, std::ostream& out, std::ostream& err) {
    core::Vm vm(prog.cells);
    const core::RunResult r = vm.run_pure(max_steps);
    if (r.kind == core::RunResult::Kind::Fault) {
        err << "malm snapshot: the program faults before its first I/O ("
            << core::format_fault(*r.fault) << "); no snapshot written\n";
        return 1;
    }
    const std::vector<std::uint8_t> bytes =
        container::write_snapshot(vm, {*prog.file_hash, prog.cells});
    const std::string snap = snapshot_path(path);
    if (!write_output(snap, bytes, out, err)) {
        return 1;
    }
    out << snap << ": " << vm.steps() << " steps ("
        << (r.kind == core::RunResult::Kind::BeforeIo
                ? "up to the first " + std::string(next_op(vm))
                : std::string("--max-steps"))
        << "), " << vm.memory().page_count() << " pages, " << bytes.size() << " bytes\n";
    return 0;
}

// Runs the program to the snapshot's step without it and compares the states.
int check_command(std::string_view path, const LoadedProgram& prog, std::ostream& out,
                  std::ostream& err) {
    const std::string snap = snapshot_path(path);
    std::vector<std::uint8_t> bytes;
    if (!read_file(snap, bytes, err)) {
        return 1;
    }
    const auto s = container::Snapshot::read(bytes, {*prog.file_hash, prog.cells});
    if (!s) {
        err << "malm snapshot: invalid snapshot '" << snap << "': " << s.error() << '\n';
        return 1;
    }
    core::Vm vm(prog.cells);
    const core::RunResult r = vm.run_pure(s->steps());
    std::string diff;
    if (r.kind == core::RunResult::Kind::Fault) {
        diff = "the program faults first (" + core::format_fault(*r.fault) + ")";
    } else if (vm.steps() < s->steps()) {
        diff = "the program does I/O first, at step " + std::to_string(vm.steps() + 1);
    } else {
        diff = s->compare(vm, prog.cells);
    }
    if (!diff.empty()) {
        err << "malm snapshot: '" << snap << "' does not match the program at step " << s->steps()
            << ": " << diff << '\n';
        return 1;
    }
    out << snap << ": matches the program at step " << s->steps() << '\n';
    return 0;
}

} // namespace

std::string snapshot_path(std::string_view path) { return std::string(path) + ".snap"; }

bool has_snapshot(std::string_view path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(snapshot_path(path), ec);
}

std::optional<container::Snapshot> find_snapshot(std::string_view path, const LoadedProgram& prog,
                                                 std::ostream& err) {
    if (!prog.file_hash || !has_snapshot(path)) {
        return std::nullopt;
    }
    const std::string snap = snapshot_path(path);
    const auto ignore = [&](std::string_view why) {
        err << "malm: note: ignoring snapshot '" << snap << "': " << why << '\n';
        return std::nullopt;
    };
    if (prog.classic) {
        return ignore("snapshots are for unleashed-mode programs");
    }
    std::vector<std::uint8_t> bytes;
    if (!read_file(snap, bytes, err)) {
        return ignore("cannot read it");
    }
    auto s = container::Snapshot::read(bytes, {*prog.file_hash, prog.cells});
    if (!s) {
        return ignore(s.error());
    }
    return std::move(*s);
}

int snapshot_command(std::span<const std::string_view> args, std::istream&, std::ostream& out,
                     std::ostream& err) {
    std::string_view path;
    bool check = false;
    bool options_done = false;
    std::optional<std::uint64_t> max_steps;
    std::size_t max_cells = core::kDefaultMaxCells;
    bool max_cells_given = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (!options_done && arg == "--") {
            options_done = true;
        } else if (!options_done && arg == "--check") {
            check = true;
        } else if (!options_done && arg == "--max-steps") {
            max_steps = i + 1 < args.size() ? parse_count(args[i + 1]) : std::nullopt;
            if (!max_steps) {
                return usage_error(err, "--max-steps needs a positive integer");
            }
            ++i;
        } else if (!options_done && arg == "--max-cells") {
            const auto v = i + 1 < args.size() ? parse_max_cells(args[i + 1]) : std::nullopt;
            if (!v) {
                return usage_error(err, "--max-cells needs a positive integer");
            }
            ++i;
            max_cells = *v;
            max_cells_given = true;
        } else if ((!options_done && arg.starts_with('-')) || !path.empty()) {
            return usage_error(err, "unexpected argument '" + std::string(arg) + "'");
        } else {
            path = arg;
        }
    }
    if (path.empty()) {
        return usage_error(err, "missing input file");
    }
    if (check && max_steps) {
        return usage_error(err, "--max-steps cannot be used with --check");
    }
    if (max_cells_given) {
        if (const std::string msg = max_cells_limit_error(false, max_cells); !msg.empty()) {
            return usage_error(err, msg);
        }
    }
    LoadedProgram prog;
    if (const int rc = load_program(path, false, err, prog, max_cells, true); rc != 0) {
        return rc;
    }
    if (prog.classic) {
        err << "malm snapshot: '" << path
            << "' is a classic-mode program; snapshots are for unleashed mode\n";
        return 1;
    }
    return check ? check_command(path, prog, out, err)
                 : write_command(path, prog, max_steps, out, err);
}

} // namespace malm
