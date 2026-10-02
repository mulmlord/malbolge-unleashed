#include "driver/export_cmd.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "core/classic.hpp"
#include "core/loader.hpp"
#include "driver/program_io.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm export (--text|--malm) [--classic] [--exit-a] [--codec packed|zstd]\n"
    "                   [--max-cells N] -o <out> [--] <in>\n"
    "       (-o - writes to stdout; --exit-a and --codec apply to --malm only)\n";

int usage_error(std::ostream& err, std::string_view msg) {
    if (!msg.empty()) {
        err << "malm export: " << msg << '\n';
    }
    err << kUsage;
    return 2;
}

} // namespace

int export_command(std::span<const std::string_view> args, std::istream&, std::ostream& out,
                   std::ostream& err) {
    std::string_view input;
    std::string_view output;
    bool have_output = false;
    bool want_text = false;
    bool want_malm = false;
    bool classic = false;
    bool exit_a = false;
    bool have_codec = false;
    container::Codec codec = container::Codec::Zstd;
    bool options_done = false;
    std::size_t max_cells = core::kDefaultMaxCells;
    bool max_cells_given = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (!options_done && arg == "--") {
            options_done = true;
        } else if (!options_done && arg == "--text") {
            want_text = true;
        } else if (!options_done && arg == "--malm") {
            want_malm = true;
        } else if (!options_done && arg == "--classic") {
            classic = true;
        } else if (!options_done && arg == "--exit-a") {
            exit_a = true;
        } else if (!options_done && arg == "--max-cells") {
            const auto v = i + 1 < args.size() ? parse_max_cells(args[i + 1]) : std::nullopt;
            if (!v) {
                return usage_error(err, "--max-cells needs a positive integer");
            }
            ++i;
            max_cells = *v;
            max_cells_given = true;
        } else if (!options_done && (arg == "--codec" || arg == "-o")) {
            if (i + 1 == args.size()) {
                return usage_error(err, "option '" + std::string(arg) + "' needs a value");
            }
            const std::string_view value = args[++i];
            if (arg == "-o") {
                if (have_output) {
                    return usage_error(err, "-o given twice");
                }
                output = value;
                have_output = true;
            } else if (value == "packed") {
                codec = container::Codec::Packed;
                have_codec = true;
            } else if (value == "zstd") {
                codec = container::Codec::Zstd;
                have_codec = true;
            } else {
                return usage_error(err, "unknown codec '" + std::string(value) +
                                            "' (expected packed or zstd)");
            }
        } else if ((!options_done && arg.starts_with('-')) || !input.empty()) {
            return usage_error(err, "unexpected argument '" + std::string(arg) + "'");
        } else {
            input = arg;
        }
    }
    if (want_text == want_malm) {
        return usage_error(err, "exactly one of --text and --malm is required");
    }
    if (input.empty()) {
        return usage_error(err, "missing input file");
    }
    if (!have_output) {
        return usage_error(err, "missing -o <out>");
    }
    if (want_text && (exit_a || have_codec)) {
        return usage_error(err, "--exit-a and --codec apply to --malm only");
    }

    if (max_cells_given) {
        if (const std::string msg = max_cells_limit_error(classic, max_cells); !msg.empty()) {
            return usage_error(err, msg);
        }
    }
    LoadedProgram prog;
    if (const int rc = load_program(input, classic, err, prog, max_cells); rc != 0) {
        return rc;
    }
    if (want_text && prog.from_malm && (prog.classic || prog.exit_a)) {
        err << "malm: warning: plain text cannot represent " << (prog.classic ? "classic mode" : "")
            << (prog.classic && prog.exit_a ? " or " : "") << (prog.exit_a ? "EXIT_A" : "")
            << "; not exported\n";
    }

    std::vector<std::uint8_t> bytes;
    if (want_text) {
        // The denormalized cells are the program text; one trailing newline (the loader skips it).
        bytes = std::move(prog.cells);
        bytes.push_back('\n');
    } else {
        bytes = encode_malm(std::move(prog), exit_a, codec);
    }

    if (output == "-") {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            err << "malm: E_IO: cannot write to stdout\n";
            return 1;
        }
        return 0;
    }
    std::ofstream file{std::string(output), std::ios::binary | std::ios::trunc};
    if (!file) {
        err << "malm: cannot write '" << output << "': " << std::strerror(errno) << '\n';
        return 1;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
        err << "malm: cannot write '" << output << "': " << std::strerror(errno) << '\n';
        return 1;
    }
    return 0;
}

} // namespace malm
