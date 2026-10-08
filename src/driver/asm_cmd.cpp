#include "driver/asm_cmd.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>
#include <vector>

#include "asm/assemble.hpp"
#include "asm/check.hpp"
#include "asm/parser.hpp"
#include "asm/printer.hpp"
#include "container/container.hpp"
#include "core/loader.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm asm [--text] [--map <file>] -o <out> [--] <file.mua>\n"
    "       malm asm (--check | --print) [--] <file.mua>\n"
    "  -o <out>      write the assembled program (.malm; -o - writes to stdout)\n"
    "  --text        write Malbolge text instead of a .malm\n"
    "  --map <file>  also write the label map (name, address, code|data, block extent)\n"
    "  --check       parse and run the static checks; diagnostics on stderr, exit 0 or 1\n"
    "  --print       after the checks, print the program in canonical form on stdout\n";

int usage_error(std::ostream& err, std::string_view msg) {
    if (!msg.empty()) {
        err << "malm asm: " << msg << '\n';
    }
    err << kUsage;
    return 2;
}

// True if both paths name the same existing file, or are spelled the same.
bool same_file(std::string_view a, std::string_view b) {
    if (a == "-" || b == "-") {
        return false;
    }
    std::error_code ec;
    return a == b || std::filesystem::equivalent(std::filesystem::path(std::string(a)),
                                                 std::filesystem::path(std::string(b)), ec);
}

// Writes `bytes` to `path`, or to `out` for "-". On failure prints a diagnostic and returns false.
bool write_output(std::string_view path, const std::vector<std::uint8_t>& bytes, std::ostream& out,
                  std::ostream& err) {
    if (path == "-") {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            err << "malm: E_IO: cannot write to stdout\n";
            return false;
        }
        return true;
    }
    std::ofstream file{std::string(path), std::ios::binary | std::ios::trunc};
    if (file) {
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.close();
    }
    if (!file) {
        err << "malm: cannot write '" << path << "': " << std::strerror(errno) << '\n';
        return false;
    }
    return true;
}

// The label map (docs/asm.md 7): one line per label, `name address code|data first last`, ordered
// by address and then by name.
std::vector<std::uint8_t> label_map(const assembler::Assembly& a) {
    std::vector<std::pair<std::string, const assembler::LabelInfo*>> labels;
    for (const auto& [name, info] : a.layout.labels) {
        labels.emplace_back(name, &info);
    }
    std::ranges::sort(labels, [](const auto& x, const auto& y) {
        return x.second->addr.raw() != y.second->addr.raw()
                   ? x.second->addr.raw() < y.second->addr.raw()
                   : x.first < y.first;
    });
    std::string text;
    for (const auto& [name, info] : labels) {
        const assembler::PlacedBlock& block = a.layout.blocks[info->block];
        text += name + ' ' + info->addr.to_string() + ' ' +
                (info->section == assembler::Section::Code ? "code" : "data") + ' ' +
                block.first.to_string() + ' ' + block.last.to_string() + '\n';
    }
    return {text.begin(), text.end()};
}

// Purity check (spec 4): the program text passes the strict loader unchanged, and the .malm holds
// exactly these cells and nothing else. A failure is an assembler bug.
bool is_pure(const std::vector<std::uint8_t>& text, const std::vector<std::uint8_t>& malm) {
    const auto loaded = core::load_text(text, core::kUnleashedMaxCells);
    if (!loaded || loaded.value() != text) {
        return false;
    }
    const auto read = container::read_malm(malm, container::Limits{core::kUnleashedMaxCells});
    return read && read->cells == text && read->mode == container::Mode::Unleashed && !read->exit_a;
}

} // namespace

int asm_command(std::span<const std::string_view> args, std::istream&, std::ostream& out,
                std::ostream& err) {
    std::string_view input;
    std::string_view output;
    std::string_view map;
    bool have_output = false;
    bool have_map = false;
    bool check = false;
    bool print = false;
    bool text = false;
    bool options_done = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (!options_done && arg == "--") {
            options_done = true;
        } else if (!options_done && arg == "--check") {
            check = true;
        } else if (!options_done && arg == "--print") {
            print = true;
        } else if (!options_done && arg == "--text") {
            text = true;
        } else if (!options_done && (arg == "-o" || arg == "--map")) {
            if (i + 1 == args.size()) {
                return usage_error(err, "option '" + std::string(arg) + "' needs a value");
            }
            bool& have = arg == "-o" ? have_output : have_map;
            if (have) {
                return usage_error(err, std::string(arg) + " given twice");
            }
            have = true;
            (arg == "-o" ? output : map) = args[++i];
        } else if ((!options_done && arg.starts_with('-') && arg != "-") || !input.empty()) {
            return usage_error(err, "unexpected argument '" + std::string(arg) + "'");
        } else {
            input = arg;
        }
    }
    if (input.empty()) {
        return usage_error(err, "missing input file");
    }
    const bool assemble = have_output || have_map || text;
    if ((check || print) && assemble) {
        return usage_error(err, "-o, --text and --map cannot be combined with --check or --print");
    }
    if (!check && !print && !have_output) {
        return usage_error(err, "missing -o <out>");
    }
    if (have_map && map == "-") {
        return usage_error(err, "--map - is not supported");
    }
    if (have_map && have_output && same_file(map, output)) {
        return usage_error(err, "-o and --map name the same file");
    }
    for (const std::string_view path : {output, map}) {
        if (!path.empty() && same_file(path, input)) {
            err << "malm asm: refusing to overwrite the input file '" << input << "'\n";
            return 1;
        }
    }

    assembler::FsLoader loader;
    assembler::Diagnostics diags;
    const assembler::Program program = assembler::analyze(std::string(input), loader, diags);
    std::optional<assembler::Assembly> assembly;
    if (!diags.has_errors() && assemble) {
        assembly = assembler::assemble(program, diags);
    }
    if (diags.has_errors()) {
        err << assembler::format_diagnostics(diags, program.files);
        return 1;
    }
    if (print) {
        out << assembler::print_program(program);
        out.flush();
        if (!out) {
            err << "malm: E_IO: cannot write to stdout\n";
            return 1;
        }
    }
    if (!assemble) {
        return 0;
    }

    const std::vector<std::uint8_t>& cells = assembly->startup.text;
    const std::vector<std::uint8_t> malm = container::write_malm({cells});
    if (!is_pure(cells, malm)) {
        err << "malm asm: internal error: the assembled program fails the purity check\n";
        return 1;
    }
    std::vector<std::uint8_t> bytes = malm;
    if (text) {
        bytes = cells;
        bytes.push_back('\n');
    }
    if (!write_output(output, bytes, out, err)) {
        return 1;
    }
    if (have_map && !write_output(map, label_map(*assembly), out, err)) {
        return 1;
    }
    return 0;
}

} // namespace malm
