#include "driver/rvref_cmd.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "driver/program_io.hpp"
#include "driver/stream_io.hpp"
#include "rv/elf.hpp"
#include "rv/machine.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm rvref [--ram <size>] [--trace] [--regs] [--] <prog.elf>\n"
    "  --ram <size>  guest RAM in bytes, or with a K/M suffix (default 1M)\n"
    "  --trace       one line per instruction on stderr\n"
    "  --regs        the final pc, step count and registers on stderr\n";

constexpr std::array<std::string_view, 32> kAbiNames{
    "zero", "ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0",
    "a1",   "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5",
    "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};

int usage_error(std::ostream& err, std::string_view msg) {
    err << "malm rvref: " << msg << '\n' << kUsage;
    return 2;
}

std::string hex(std::uint32_t v) {
    char buf[11];
    std::snprintf(buf, sizeof buf, "0x%08x", v);
    return buf;
}

// Runs with a trace line per instruction: `step=S pc=0x... insn=0x...`, then ` <reg>=0x...` for
// the register the instruction wrote. Lines are buffered and flushed before a read from stdin and
// at the end, so the trace is current while the guest waits for input.
rv::Stop run_traced(rv::Machine& m, core::Io& io, std::ostream& err) {
    std::string buf;
    const auto flush = [&] {
        err << buf;
        buf.clear();
    };
    for (;;) {
        const std::uint64_t step = m.steps() + 1;
        const std::uint32_t pc = m.pc();
        const auto insn = m.peek32(pc);
        const auto before = m.regs();
        if (insn == 0x00000073 && before[17] == rv::kSysRead) {
            flush();
        }
        const std::optional<rv::Stop> stop = m.step(io);
        buf += "step=" + std::to_string(step) + " pc=" + hex(pc) +
               " insn=" + (insn ? hex(*insn) : std::string("?"));
        for (std::size_t i = 1; i < 32; ++i) {
            if (m.regs()[i] != before[i]) {
                buf += ' ' + std::string(kAbiNames[i]) + '=' + hex(m.regs()[i]);
            }
        }
        buf += '\n';
        if (stop) {
            flush();
            if (!io.flush() && stop->kind == rv::Stop::Kind::Exit) {
                return {rv::Stop::Kind::Fault, 0, "E_IO: cannot write to stdout"};
            }
            return *stop;
        }
        if (buf.size() > (1u << 16)) {
            flush();
        }
    }
}

} // namespace

std::optional<std::uint32_t> parse_ram_size(std::string_view text) {
    std::uint64_t unit = 1;
    if (text.ends_with('K') || text.ends_with('M')) {
        unit = text.back() == 'K' ? 1u << 10 : 1u << 20;
        text.remove_suffix(1);
    }
    std::uint64_t v = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() || v == 0 ||
        v > rv::kMaxRamSize / unit || v * unit % rv::kRamAlign != 0) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(v * unit);
}

int rvref_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                  std::ostream& err) {
    std::string_view path;
    std::uint32_t ram_size = rv::kDefaultRamSize;
    bool trace = false;
    bool regs = false;
    bool options_done = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (!options_done && arg == "--") {
            options_done = true;
        } else if (!options_done && arg == "--trace") {
            trace = true;
        } else if (!options_done && arg == "--regs") {
            regs = true;
        } else if (!options_done && arg == "--ram") {
            const auto v = i + 1 < args.size() ? parse_ram_size(args[i + 1]) : std::nullopt;
            if (!v) {
                return usage_error(err, "--ram needs a positive size, a multiple of 16 bytes up "
                                        "to 1024M");
            }
            ram_size = *v;
            ++i;
        } else if ((!options_done && arg.starts_with('-')) || !path.empty()) {
            return usage_error(err, "unexpected argument '" + std::string(arg) + "'");
        } else {
            path = arg;
        }
    }
    if (path.empty()) {
        return usage_error(err, "missing input file");
    }

    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes, err)) {
        return 1;
    }
    const auto image = rv::load_elf(bytes, ram_size);
    if (!image) {
        err << "malm rvref: " << path << ": " << image.error() << '\n';
        return 1;
    }

    rv::Machine m(*image, ram_size);
    StreamIo io(in, out);
    const rv::Stop stop = trace ? run_traced(m, io, err) : m.run(io);
    if (stop.kind == rv::Stop::Kind::Fault) {
        err << "malm rvref: " << stop.message << " at pc=" << hex(m.pc()) << " (step "
            << m.steps() + 1 << ")\n";
    }
    if (regs) {
        err << "pc   " << hex(m.pc()) << "\nsteps " << m.steps() << '\n';
        for (std::size_t i = 0; i < 32; ++i) {
            std::string name = "x" + std::to_string(i) + ' ' + std::string(kAbiNames[i]);
            name.resize(9, ' ');
            err << name << hex(m.regs()[i]) << '\n';
        }
    }
    return stop.kind == rv::Stop::Kind::Exit ? stop.exit_code : 1;
}

} // namespace malm
