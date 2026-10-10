#include "driver/build_cmd.hpp"

#include <optional>
#include <ostream>
#include <utility>

#include "asm/assemble.hpp"
#include "asm/parser.hpp"
#include "container/container.hpp"
#include "driver/program_io.hpp"
#include "driver/rvemu_cmd.hpp"
#include "driver/rvref_cmd.hpp"
#include "hla/lower.hpp"
#include "rv/emu.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm build [--ram <size>] -o <out.malm> [--] <prog.elf>\n"
    "  --ram <size>    guest RAM in bytes, or with a K/M suffix (default 1M, at most 4M)\n"
    "  -o <out.malm>   write the program there (-o - writes to stdout)\n"
    "  <prog.elf>      an RV32IM ELF executable (docs/rv.md)\n";

int usage_error(std::ostream& err, std::string_view msg) {
    err << "malm build: " << msg << '\n' << kUsage;
    return 2;
}

} // namespace

core::Result<std::vector<std::uint8_t>, std::string> build_emu(const rv::Image& image,
                                                               std::uint32_t ram_size) {
    using R = core::Result<std::vector<std::uint8_t>, std::string>;
    rv::EmuOptions opt;
    opt.ram_size = ram_size;
    opt.image = image;
    assembler::MemoryLoader loader({{"emu.mhl", rv::emu_source(opt)}});
    assembler::Diagnostics diags;
    const assembler::Program program = hla::translate("emu.mhl", loader, diags);
    std::optional<assembler::Assembly> assembly;
    if (!diags.has_errors()) {
        assembly = assembler::assemble(program, diags);
    }
    if (diags.has_errors()) {
        return R::err("internal error: the emulator does not compile:\n" +
                      assembler::format_diagnostics(diags, program.files));
    }
    const std::vector<std::uint8_t>& cells = assembly->startup.text;
    std::vector<std::uint8_t> malm = container::write_malm({cells, {}, true});
    if (!is_pure(cells, malm, true)) {
        return R::err("internal error: the program fails the purity check");
    }
    return R::ok(std::move(malm));
}

int build_command(std::span<const std::string_view> args, std::istream&, std::ostream& out,
                  std::ostream& err) {
    std::string_view path;
    std::string_view output;
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
            if (!output.empty()) {
                return usage_error(err, "-o given twice");
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
    if (output.empty()) {
        return usage_error(err, "missing -o <out.malm>");
    }
    if (same_file(output, path)) {
        err << "malm build: refusing to overwrite the input file '" << path << "'\n";
        return 1;
    }

    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes, err)) {
        return 1;
    }
    const auto image = rv::load_elf(bytes, ram_size);
    if (!image) {
        err << "malm build: " << path << ": " << image.error() << '\n';
        return 1;
    }
    auto malm = build_emu(*image, ram_size);
    if (!malm) {
        err << "malm build: " << malm.error() << '\n';
        return 1;
    }
    return write_output(output, *malm, out, err) ? 0 : 1;
}

} // namespace malm
