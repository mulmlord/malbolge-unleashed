#include "driver/run_cmd.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "core/loader.hpp"
#include "core/vm.hpp"
#include "driver/stream_io.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage = "usage: malm run <file> [--exit-a]\n";

} // namespace

int run_command(std::span<const std::string_view> args, std::istream& in, std::ostream& out,
                std::ostream& err) {
    std::string_view path;
    bool exit_a = false;
    bool options_done = false;
    for (const std::string_view arg : args) {
        if (!options_done && arg == "--") {
            options_done = true;
        } else if (!options_done && arg == "--exit-a") {
            exit_a = true;
        } else if ((!options_done && arg.starts_with('-')) || !path.empty()) {
            err << "malm run: unexpected argument '" << arg << "'\n" << kUsage;
            return 2;
        } else {
            path = arg;
        }
    }
    if (path.empty()) {
        err << kUsage;
        return 2;
    }

    std::vector<std::uint8_t> bytes;
    {
        const std::filesystem::path fs_path{std::string(path)};
        std::error_code ec;
        const bool regular = std::filesystem::is_regular_file(fs_path, ec);
        if (ec) {
            err << "malm: cannot open '" << path << "': " << ec.message() << '\n';
            return 1;
        }
        if (!regular) {
            err << "malm: cannot read '" << path << "': not a regular file\n";
            return 1;
        }
        std::ifstream file(fs_path, std::ios::binary);
        if (!file) {
            err << "malm: cannot open '" << path << "': " << std::strerror(errno) << '\n';
            return 1;
        }
        std::array<char, 65536> buf;
        while (file.read(buf.data(), static_cast<std::streamsize>(buf.size())) ||
               file.gcount() > 0) {
            bytes.insert(bytes.end(), buf.data(), buf.data() + file.gcount());
        }
        if (file.bad()) {
            err << "malm: cannot read '" << path << "': " << std::strerror(errno) << '\n';
            return 1;
        }
    }

    const auto cells = core::load_text(bytes);
    if (!cells) {
        err << "malm: E_LOAD at byte " << cells.error().offset << ": " << cells.error().reason
            << '\n';
        return 1;
    }

    core::Vm vm(*cells);
    StreamIo io(in, out);
    const core::RunResult r = vm.run(io);
    if (r.kind == core::RunResult::Kind::Fault) {
        err << "malm: " << core::format_fault(*r.fault) << '\n';
    }
    return core::exit_status(r, vm, exit_a);
}

} // namespace malm
