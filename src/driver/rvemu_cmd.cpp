#include "driver/rvemu_cmd.hpp"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "driver/program_io.hpp"
#include "driver/rvref_cmd.hpp"
#include "rv/elf.hpp"
#include "rv/emu.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm rvemu [--ram <size>] [--harness] [-o <out.mhl>] [--] [<prog.elf>]\n"
    "  --ram <size>  guest RAM in bytes, or with a K/M suffix (default 1M, at most 4M)\n"
    "  --harness     stops continue at the fetch instead of halting (for tests)\n"
    "  -o <out.mhl>  write the HLA program there (default: stdout)\n"
    "  <prog.elf>    a guest program to put in RAM (default: none, pc = 0)\n";

int usage_error(std::ostream& err, std::string_view msg) {
    err << "malm rvemu: " << msg << '\n' << kUsage;
    return 2;
}

} // namespace

int rvemu_command(std::span<const std::string_view> args, std::istream&, std::ostream& out,
                  std::ostream& err) {
    std::string_view path;
    std::string_view output = "-";
    rv::EmuOptions opt;
    bool options_done = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (!options_done && arg == "--") {
            options_done = true;
        } else if (!options_done && arg == "--harness") {
            opt.harness = true;
        } else if (!options_done && arg == "-o") {
            if (i + 1 >= args.size()) {
                return usage_error(err, "-o needs a file name");
            }
            output = args[++i];
        } else if (!options_done && arg == "--ram") {
            const auto v = i + 1 < args.size() ? parse_ram_size(args[i + 1]) : std::nullopt;
            if (!v || *v > kMaxEmuRam) {
                return usage_error(err, "--ram needs a positive size, a multiple of 16 bytes up "
                                        "to 4M");
            }
            opt.ram_size = *v;
            ++i;
        } else if ((!options_done && arg.starts_with('-')) || !path.empty()) {
            return usage_error(err, "unexpected argument '" + std::string(arg) + "'");
        } else {
            path = arg;
        }
    }

    if (!path.empty()) {
        std::vector<std::uint8_t> bytes;
        if (!read_file(path, bytes, err)) {
            return 1;
        }
        auto image = rv::load_elf(bytes, opt.ram_size);
        if (!image) {
            err << "malm rvemu: " << path << ": " << image.error() << '\n';
            return 1;
        }
        opt.image = std::move(*image);
    }
    const std::string src = rv::emu_source(opt);
    return write_output(output, std::vector<std::uint8_t>(src.begin(), src.end()), out, err) ? 0
                                                                                             : 1;
}

} // namespace malm
