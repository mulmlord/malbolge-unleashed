#include "driver/program_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <ostream>
#include <string>

#include "container/container.hpp"
#include "core/classic.hpp"
#include "core/loader.hpp"

namespace malm {

bool same_file(std::string_view a, std::string_view b) {
    if (a == "-" || b == "-") {
        return false;
    }
    std::error_code ec;
    return a == b || std::filesystem::equivalent(std::filesystem::path(std::string(a)),
                                                 std::filesystem::path(std::string(b)), ec);
}

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

bool is_pure(const std::vector<std::uint8_t>& text, const std::vector<std::uint8_t>& malm,
             bool exit_a) {
    const auto loaded = core::load_text(text, core::kUnleashedMaxCells);
    if (!loaded || loaded.value() != text) {
        return false;
    }
    const auto read = container::read_malm(malm, container::Limits{core::kUnleashedMaxCells});
    return read && read->cells == text && read->mode == container::Mode::Unleashed &&
           read->exit_a == exit_a;
}

bool read_file(std::string_view path, std::vector<std::uint8_t>& bytes, std::ostream& err) {
    const std::filesystem::path fs_path{std::string(path)};
    std::error_code ec;
    const bool regular = std::filesystem::is_regular_file(fs_path, ec);
    if (ec) {
        err << "malm: cannot open '" << path << "': " << ec.message() << '\n';
        return false;
    }
    if (!regular) {
        err << "malm: cannot read '" << path << "': not a regular file\n";
        return false;
    }
    std::ifstream file(fs_path, std::ios::binary);
    if (!file) {
        err << "malm: cannot open '" << path << "': " << std::strerror(errno) << '\n';
        return false;
    }
    std::array<char, 65536> buf;
    while (file.read(buf.data(), static_cast<std::streamsize>(buf.size())) || file.gcount() > 0) {
        bytes.insert(bytes.end(), buf.data(), buf.data() + file.gcount());
    }
    if (file.bad()) {
        err << "malm: cannot read '" << path << "': " << std::strerror(errno) << '\n';
        return false;
    }
    return true;
}

std::optional<std::size_t> parse_max_cells(std::string_view text) {
    std::size_t v = 0;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
    if (text.empty() || r.ec != std::errc{} || r.ptr != text.data() + text.size() || v == 0) {
        return std::nullopt;
    }
    return v;
}

std::string max_cells_limit_error(bool classic, std::size_t max_cells) {
    const std::size_t format_max = classic ? core::kClassicCells : core::kUnleashedMaxCells;
    if (max_cells > format_max) {
        return "--max-cells " + std::to_string(max_cells) + " exceeds the format limit " +
               std::to_string(format_max);
    }
    return {};
}

std::vector<std::uint8_t> encode_malm(LoadedProgram&& prog, bool exit_a, container::Codec codec) {
    container::Program p{std::move(prog.cells),
                         prog.classic ? container::Mode::Classic : container::Mode::Unleashed,
                         prog.exit_a || exit_a};
    return container::write_malm(p, codec);
}

namespace {

int load_program_impl(std::string_view path, bool classic_flag, std::ostream& err,
                      LoadedProgram& out, std::size_t max_cells) {
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes, err)) {
        return 1;
    }
    if (container::is_malm(bytes)) {
        auto p = container::read_malm(bytes, container::Limits{max_cells});
        if (!p) {
            err << "malm: E_LOAD: invalid .malm '" << path << "': " << p.error() << '\n';
            return 1;
        }
        const bool classic = p->mode == container::Mode::Classic;
        if (classic_flag && !classic) {
            err << "malm: '" << path
                << "' is an unleashed-mode .malm; --classic cannot be used with it\n";
            return 2;
        }
        out = {std::move(p->cells), classic, p->exit_a, true};
        return 0;
    }
    const std::size_t format_max = classic_flag ? core::kClassicCells : core::kUnleashedMaxCells;
    const std::size_t limit = std::min(max_cells, format_max);
    auto cells = core::load_text(bytes, limit);
    if (!cells) {
        err << "malm: E_LOAD at byte " << cells.error().offset << ": " << cells.error().reason;
        if (limit < format_max && cells.error().reason.starts_with("program is longer than")) {
            err << " (use --max-cells)";
        }
        err << '\n';
        return 1;
    }
    out = {std::move(*cells), classic_flag, false, false};
    return 0;
}

} // namespace

int load_program(std::string_view path, bool classic_flag, std::ostream& err, LoadedProgram& out,
                 std::size_t max_cells) {
    try {
        return load_program_impl(path, classic_flag, err, out, max_cells);
    } catch (const std::bad_alloc&) {
        err << "malm: out of memory\n";
        return 1;
    }
}

} // namespace malm
