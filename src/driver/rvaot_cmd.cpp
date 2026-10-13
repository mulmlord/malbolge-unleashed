#include "driver/rvaot_cmd.hpp"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "aot/aot.hpp"
#include "driver/program_io.hpp"
#include "driver/rvemu_cmd.hpp"
#include "driver/rvref_cmd.hpp"
#include "rv/elf.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm rvaot [--ram <size>] [-o <out.mhl>] [--] <prog.elf>\n"
    "  --ram <size>  guest RAM in bytes, or with a K/M suffix (default 1M, at most 4M)\n"
    "  -o <out.mhl>  write the HLA program there (default: stdout)\n"
    "  <prog.elf>    the RV32IM program to translate\n";

int usage_error(std::ostream& err, std::string_view msg) {
    err << "malm rvaot: " << msg << '\n' << kUsage;
    return 2;
}

} // namespace

int rvaot_command(std::span<const std::string_view> args, std::istream&, std::ostream& out,
                  std::ostream& err) {
    std::string_view path;
    std::string_view output = "-";
    std::uint32_t ram_size = rv::kDefaultRamSize;
    bool options_done = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (!options_done && arg == "--") {
            options_done = true;
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
        err << "malm rvaot: " << path << ": " << image.error() << '\n';
        return 1;
    }
    const auto src = aot::aot_source(*image, ram_size);
    if (!src) {
        err << "malm rvaot: " << path << ": " << src.error() << '\n';
        return 1;
    }
    return write_output(output, std::vector<std::uint8_t>(src->begin(), src->end()), out, err) ? 0
                                                                                               : 1;
}

} // namespace malm
