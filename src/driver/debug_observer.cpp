#include "driver/debug_observer.hpp"

#include <cstdio>
#include <ostream>

namespace malm {
namespace {

constexpr std::size_t kFlushAt = 1 << 16;

// "  <label padded to 13>" (the value follows).
std::string row(std::string_view label) {
    std::string s = "  ";
    s += label;
    while (s.size() < 15) {
        s += ' ';
    }
    return s;
}

} // namespace

DebugObserver::DebugObserver(const DebugOptions& opts, std::ostream& err) : opts_(opts), err_(err) {
    buf_.reserve(kFlushAt + 512);
}

void DebugObserver::before(const core::StepInfo& s) {
    if (opts_.stats && s.decoded) {
        ++op_counts_[static_cast<std::size_t>(s.op)];
    }
    if (!opts_.trace || dead_ || s.step < opts_.trace_from || s.step > opts_.trace_to) {
        return;
    }
    buf_ += "step=";
    buf_ += std::to_string(s.step);
    buf_ += " C=";
    buf_ += s.c.to_string();
    buf_ += " [C]=";
    buf_ += s.decoded ? std::to_string(s.cell.raw()) : s.cell.to_string();
    buf_ += " op=";
    buf_ += s.decoded ? core::op_name(s.op) : std::string_view("?");
    buf_ += " D=";
    buf_ += s.d.to_string();
    buf_ += " [D]=";
    buf_ += s.dval.to_string();
    buf_ += " A=";
    buf_ += s.a.to_string();
    buf_ += " n=";
    buf_ += std::to_string(s.n);
    buf_ += '\n';
    // A step that may block on input flushes first, so the trace is current while waiting.
    if (buf_.size() >= kFlushAt || (s.decoded && s.op == core::Op::In)) {
        flush_trace();
    }
}

void DebugObserver::flush_trace() {
    if (!buf_.empty()) {
        if (!dead_) {
            err_.write(buf_.data(), static_cast<std::streamsize>(buf_.size()));
            err_.flush();
            dead_ = !err_;
        }
        buf_.clear();
    }
}

void DebugObserver::print_stats(bool halted, std::string_view error, std::uint64_t steps,
                                unsigned n, std::optional<std::size_t> pages, double wall_seconds) {
    std::string s = "malm stats:\n";
    s += row("steps") + std::to_string(steps) + '\n';
    s += row("halted");
    s += halted ? "yes" : "no (" + std::string(error) + ")";
    s += '\n';
    s += row("n") + std::to_string(n) + '\n';
    s += row("pages") + (pages ? std::to_string(*pages) : "-") + '\n';
    for (std::size_t i = 0; i < op_counts_.size(); ++i) {
        s += row("op " + std::string(core::op_name(static_cast<core::Op>(i)))) +
             std::to_string(op_counts_[i]) + '\n';
    }
    char tmp[64];
    std::snprintf(tmp, sizeof tmp, "%.6f s", wall_seconds);
    s += row("wall") + tmp + '\n';
    const auto rate = wall_seconds > 0
                          ? static_cast<std::uint64_t>(static_cast<double>(steps) / wall_seconds)
                          : std::uint64_t{0};
    s += row("steps/s") + std::to_string(rate) + '\n';
    flush_trace();
    err_ << s;
}

} // namespace malm
