// VM benchmark (issue #37, docs/benchmarks.md): runs one program in process on in-memory stdin
// and times the plain step loop (`Vm::run` without an observer, as `malm run` runs it). Building
// the VM (loading the program into memory) is timed separately. Short programs are run again until
// the step loop has taken --min-time seconds in total; the fastest run counts.
//
// Usage: vm_bench [--classic] [--min-time S] <program> [<stdin file>]
// Prints one line: steps, seconds of the fastest run, steps/s, load seconds, output bytes.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/vm.hpp"
#include "driver/program_io.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// stdin from a buffer; output is counted and dropped.
struct MemoryIo final : malm::core::Io {
    std::span<const std::uint8_t> in;
    std::size_t pos = 0;
    std::uint64_t written = 0;

    int read_byte() override { return pos < in.size() ? in[pos++] : malm::core::kEof; }
    bool write_byte(std::uint8_t) override {
        ++written;
        return true;
    }
    bool flush() override { return true; }
};

struct Sample {
    std::uint64_t steps;
    double run_s;
    double load_s;
    std::uint64_t out;
    bool halted;
};

template <class VmT>
Sample run_once(std::span<const std::uint8_t> cells, std::span<const std::uint8_t> input) {
    const auto t0 = Clock::now();
    VmT vm(cells);
    const auto t1 = Clock::now();
    MemoryIo io;
    io.in = input;
    const malm::core::RunResult r = vm.run(io);
    const auto t2 = Clock::now();
    return {vm.steps(), std::chrono::duration<double>(t2 - t1).count(),
            std::chrono::duration<double>(t1 - t0).count(), io.written,
            r.kind == malm::core::RunResult::Kind::Halted};
}

int usage() {
    std::cerr << "usage: vm_bench [--classic] [--min-time S] <program> [<stdin file>]\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    bool classic = false;
    double min_time = 1.0;
    std::vector<std::string_view> files;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--classic") {
            classic = true;
        } else if (arg == "--min-time" && i + 1 < argc) {
            min_time = std::strtod(argv[++i], nullptr);
        } else if (arg.starts_with('-') && arg != "-") {
            return usage();
        } else {
            files.push_back(arg);
        }
    }
    if (files.empty() || files.size() > 2) {
        return usage();
    }

    malm::LoadedProgram prog;
    if (const int rc = malm::load_program(files[0], classic, std::cerr, prog); rc != 0) {
        return rc;
    }
    std::vector<std::uint8_t> input;
    if (files.size() == 2 && !malm::read_file(files[1], input, std::cerr)) {
        return 1;
    }

    Sample best{};
    double total = 0;
    do {
        const Sample s = prog.classic ? run_once<malm::core::ClassicVm>(prog.cells, input)
                                      : run_once<malm::core::Vm>(prog.cells, input);
        if (!s.halted) {
            std::cerr << "vm_bench: the program did not halt\n";
            return 1;
        }
        if (best.steps == 0 || s.run_s < best.run_s) {
            best = s;
        }
        total += s.run_s;
    } while (total < min_time);

    std::printf("%llu %.6f %.0f %.6f %llu\n", static_cast<unsigned long long>(best.steps),
                best.run_s, static_cast<double>(best.steps) / best.run_s, best.load_s,
                static_cast<unsigned long long>(best.out));
    return 0;
}
