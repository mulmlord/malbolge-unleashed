// The observer behind `malm run --trace` and `--stats` (docs/cli.md): a buffered trace writer and
// the run statistics.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>

#include "core/vm.hpp"

namespace malm {

struct DebugOptions {
    bool trace = false;
    bool stats = false;
    // Inclusive 1-based step window of the trace.
    std::uint64_t trace_from = 1;
    std::uint64_t trace_to = ~std::uint64_t{0};
};

class DebugObserver final : public core::StepObserver {
  public:
    // The trace goes to `err`.
    DebugObserver(const DebugOptions& opts, std::ostream& err);

    void before(const core::StepInfo& s) override;

    // Writes out the buffered trace lines. Call before printing anything else to the same stream.
    // Once a write to `err` has failed, tracing stops and later lines are dropped.
    void flush_trace();

    // Prints the `malm stats:` block. `n`: the final n (it never shrinks, so it is the peak).
    // `pages`: touched pages (unleashed mode), nullopt shows "-". `halted`: the run ended with a
    // halt; otherwise `error` names the fault.
    void print_stats(bool halted, std::string_view error, std::uint64_t steps, unsigned n,
                     std::optional<std::size_t> pages, double wall_seconds);

  private:
    DebugOptions opts_;
    std::ostream& err_;
    std::string buf_;
    bool dead_ = false;                        // a write to err_ failed
    std::array<std::uint64_t, 8> op_counts_{}; // indexed by core::Op
};

} // namespace malm
