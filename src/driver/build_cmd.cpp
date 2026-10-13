#include "driver/build_cmd.hpp"

#include <cstdlib>
#include <optional>
#include <ostream>
#include <utility>

#include "asm/assemble.hpp"
#include "aot/aot.hpp"
#include "asm/parser.hpp"
#include "container/container.hpp"
#include "driver/pack_cmd.hpp"
#include "driver/packed_run.hpp"
#include "driver/program_io.hpp"
#include "driver/rvemu_cmd.hpp"
#include "driver/rvref_cmd.hpp"
#include "driver/toolchain.hpp"
#include "hla/lower.hpp"
#include "rv/emu.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm build [--mode emu|aot] [--ram <size>] [--cc <compiler>]\n"
    "                  (-o <out.malm> | --exe -o <out>) (<prog.c>... | <prog.elf>)\n"
    "                  [-- <compiler args>...]\n"
    "  --mode emu       run it in rv-emu, the RV32IM emulator (the default)\n"
    "  --mode aot       translate it to Malbolge ahead of time: faster, larger\n"
    "  --ram <size>     guest RAM in bytes, or with a K/M suffix (default 1M, at most 4M)\n"
    "  --cc <compiler>  the C compiler, clang with the riscv32 target (default: clang in PATH)\n"
    "  -o <out.malm>    write the program there (-o - writes to stdout)\n"
    "  --exe -o <out>   write a native executable instead (malm pack)\n"
    "  <prog.c>...      C sources, compiled and linked with malm's runtime (docs/rv.md)\n"
    "  <prog.elf>       an RV32IM ELF executable\n"
    "  -- <args>...     the rest goes to the compiler; -Wl,<args> to the linker\n";

int usage_error(std::ostream& err, std::string_view msg) {
    err << "malm build: " << msg << '\n' << kUsage;
    return 2;
}

bool is_c_source(std::string_view path) { return path.size() > 2 && path.ends_with(".c"); }

bool is_elf(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() >= 4 && bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' &&
           bytes[3] == 'F';
}

// The generated HLA program `src` (as `name`) compiled to a pure .malm with EXIT_A; `what` names
// the generator in an internal error.
core::Result<std::vector<std::uint8_t>, std::string> compile_hla(const std::string& name,
                                                                 std::string src,
                                                                 std::string_view what) {
    using R = core::Result<std::vector<std::uint8_t>, std::string>;
    assembler::MemoryLoader loader({{name, std::move(src)}});
    assembler::Diagnostics diags;
    const assembler::Program program = hla::translate(name, loader, diags);
    std::optional<assembler::Assembly> assembly;
    if (!diags.has_errors()) {
        assembly = assembler::assemble(program, diags);
    }
    if (diags.has_errors()) {
        return R::err("internal error: " + std::string(what) + " does not compile:\n" +
                      assembler::format_diagnostics(diags, program.files));
    }
    const std::vector<std::uint8_t>& cells = assembly->startup.text;
    std::vector<std::uint8_t> malm = container::write_malm({cells, {}, true});
    if (!is_pure(cells, malm, true)) {
        return R::err("internal error: the program fails the purity check");
    }
    return R::ok(std::move(malm));
}

} // namespace

core::Result<std::vector<std::uint8_t>, std::string> build_emu(const rv::Image& image,
                                                               std::uint32_t ram_size) {
    rv::EmuOptions opt;
    opt.ram_size = ram_size;
    opt.image = image;
    return compile_hla("emu.mhl", rv::emu_source(opt), "the emulator");
}

core::Result<std::vector<std::uint8_t>, std::string> build_aot(const rv::Image& image,
                                                               std::uint32_t ram_size) {
    auto src = aot::aot_source(image, ram_size);
    if (!src) {
        return core::Result<std::vector<std::uint8_t>, std::string>::err(std::move(src.error()));
    }
    return compile_hla("aot.mhl", std::move(*src), "the translated program");
}

int build_command(std::span<const std::string_view> args, std::istream&, std::ostream& out,
                  std::ostream& err) {
    const char* path_env = std::getenv("PATH");
    return build_command_with(args, out, err, kSelfExe, path_env != nullptr ? path_env : "");
}

int build_command_with(std::span<const std::string_view> args, std::ostream& out, std::ostream& err,
                       const char* self_exe, std::string_view path_env) {
    std::vector<std::string_view> inputs;
    std::span<const std::string_view> cc_args;
    std::string_view output;
    std::optional<std::string_view> cc;
    bool exe = false;
    bool aot = false;
    std::uint32_t ram_size = rv::kDefaultRamSize;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--") {
            cc_args = args.subspan(i + 1);
            break;
        }
        if (arg == "-o") {
            if (i + 1 >= args.size()) {
                return usage_error(err, "-o needs a file name");
            }
            if (!output.empty()) {
                return usage_error(err, "-o given twice");
            }
            output = args[++i];
        } else if (arg == "--ram") {
            const auto v = i + 1 < args.size() ? parse_ram_size(args[i + 1]) : std::nullopt;
            if (!v || *v > kMaxEmuRam) {
                return usage_error(err, "--ram needs a positive size, a multiple of 16 bytes up "
                                        "to 4M");
            }
            ram_size = *v;
            ++i;
        } else if (arg == "--mode") {
            if (i + 1 >= args.size()) {
                return usage_error(err, "--mode needs emu or aot");
            }
            const std::string_view mode = args[++i];
            if (mode != "emu" && mode != "aot") {
                return usage_error(err, "unknown mode '" + std::string(mode) + "'");
            }
            aot = mode == "aot";
        } else if (arg == "--cc") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                return usage_error(err, "--cc needs a compiler");
            }
            cc = args[++i];
        } else if (arg == "--exe") {
            exe = true;
        } else if (arg.starts_with('-')) {
            return usage_error(err, "unexpected argument '" + std::string(arg) + "'");
        } else {
            inputs.push_back(arg);
        }
    }
    if (inputs.empty()) {
        return usage_error(err, "missing input file");
    }
    if (output.empty()) {
        return usage_error(err, exe ? "missing -o <out>" : "missing -o <out.malm>");
    }
    if (exe && output == "-") {
        return usage_error(err, "--exe cannot write to stdout");
    }
    const bool c = is_c_source(inputs.front());
    for (const std::string_view input : inputs) {
        if (is_c_source(input) != c || (!c && inputs.size() > 1)) {
            return usage_error(err, "give C sources or one ELF executable");
        }
    }
    if (!c && (cc || !cc_args.empty())) {
        return usage_error(err, "--cc and compiler arguments need C sources");
    }
    for (const std::string_view input : inputs) {
        if (same_file(output, input)) {
            err << "malm build: refusing to overwrite the input file '" << input << "'\n";
            return 1;
        }
    }

    std::vector<std::uint8_t> bytes;
    if (c) {
        const auto tools = find_toolchain(cc.value_or(""), path_env);
        if (!tools) {
            err << "malm build: " << tools.error() << '\n';
            return 1;
        }
        auto elf = compile_c(*tools, inputs, cc_args, err);
        if (!elf) {
            err << "malm build: " << elf.error() << '\n';
            return 1;
        }
        bytes = std::move(*elf);
    } else {
        if (!read_file(inputs.front(), bytes, err)) {
            return 1;
        }
        if (!is_elf(bytes)) {
            err << "malm build: " << inputs.front()
                << ": neither a C source (.c) nor an ELF executable\n";
            return 1;
        }
    }
    const auto image = rv::load_elf(bytes, ram_size);
    if (!image) {
        err << "malm build: " << (c ? std::string_view("the linked program") : inputs.front())
            << ": " << image.error() << '\n';
        return 1;
    }
    auto malm = aot ? build_aot(*image, ram_size) : build_emu(*image, ram_size);
    if (!malm) {
        err << "malm build: ";
        if (aot && !malm.error().starts_with("internal error")) {
            err << (c ? std::string_view("the linked program") : inputs.front()) << ": ";
        }
        err << malm.error() << '\n';
        return 1;
    }
    if (exe) {
        return write_packed(std::string(output), *malm, self_exe, err) ? 0 : 1;
    }
    return write_output(output, *malm, out, err) ? 0 : 1;
}

} // namespace malm
