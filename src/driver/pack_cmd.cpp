#include "driver/pack_cmd.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "core/loader.hpp"
#include "driver/packed_run.hpp"
#include "driver/program_io.hpp"
#include "pack/trailer.hpp"

namespace malm {
namespace {

constexpr std::string_view kUsage =
    "usage: malm pack [--classic] [--exit-a] [--max-cells N] -o <out> [--] <in>\n";

int usage_error(std::ostream& err, std::string_view msg) {
    if (!msg.empty()) {
        err << "malm pack: " << msg << '\n';
    }
    err << kUsage;
    return 2;
}

// The bytes of the running binary without a payload of its own (defensive: a packed malm never
// reaches the CLI).
bool read_base_binary(const char* self_exe, std::vector<std::uint8_t>& bytes, std::ostream& err) {
    if (!read_file(self_exe, bytes, err)) {
        return false;
    }
    if (bytes.size() >= pack::kTrailerSize) {
        const auto tail = std::span<const std::uint8_t>(bytes).last(pack::kTrailerSize);
        const auto t = pack::parse_trailer(tail, bytes.size());
        if (t && t->has_value()) {
            bytes.resize(static_cast<std::size_t>((*t)->offset));
        }
    }
    return true;
}

// True if both paths exist and name the same file (same device and inode; stat follows links, so
// hard links and symlinks to it count too).
bool same_file(const std::string& a, const std::string& b) {
    struct stat sa{};
    struct stat sb{};
    return ::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev &&
           sa.st_ino == sb.st_ino;
}

// `ld.so ./malm pack ...` would pack the dynamic loader, not malm.
bool is_dynamic_loader(const char* self_exe) {
    std::error_code ec;
    std::filesystem::path target = std::filesystem::read_symlink(self_exe, ec);
    if (ec) {
        target = self_exe;
    }
    const std::string name = target.filename().string();
    return name.starts_with("ld-") || name.starts_with("ld.so");
}

// Ignores SIGXFSZ while alive, so a file-size limit surfaces as EFBIG from write().
struct IgnoreSigXfsz {
    struct sigaction old{};
    IgnoreSigXfsz() {
        struct sigaction ign{};
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        ::sigaction(SIGXFSZ, &ign, &old);
    }
    ~IgnoreSigXfsz() { ::sigaction(SIGXFSZ, &old, nullptr); }
};

bool write_all(int fd, const std::uint8_t* p, std::size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w == 0) {
            errno = EIO;
        }
        if (w <= 0) {
            return false;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

// Writes `parts` to a temp file next to `output`, fsyncs it and renames it over `output`, so a
// failure never leaves a half-written executable. Mode is 0777 & ~umask, like a compiler's.
bool write_executable(const std::string& output,
                      const std::vector<std::span<const std::uint8_t>>& parts, std::ostream& err) {
    const IgnoreSigXfsz no_xfsz;
    const mode_t mask = ::umask(0);
    ::umask(mask);

    std::filesystem::path dir = std::filesystem::path(output).parent_path();
    if (dir.empty()) {
        dir = ".";
    }
    std::string tmpl = (dir / ".malm-pack-XXXXXX").string();
    const int fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
        err << "malm: cannot write '" << output << "': " << std::strerror(errno) << '\n';
        return false;
    }
    int e = 0; // errno of the first failure
    for (const auto& part : parts) {
        if (e == 0 && !write_all(fd, part.data(), part.size())) {
            e = errno;
        }
    }
    if (e == 0 && ::fchmod(fd, static_cast<mode_t>(0777 & ~mask)) != 0) {
        e = errno;
    }
    if (e == 0 && ::fsync(fd) != 0) {
        e = errno;
    }
    if (::close(fd) != 0 && e == 0) {
        e = errno;
    }
    if (e == 0 && ::rename(tmpl.c_str(), output.c_str()) != 0) {
        e = errno;
    }
    if (e == 0) {
        // Make the rename durable too; best effort.
        const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
    }
    if (e != 0) {
        ::unlink(tmpl.c_str());
        err << "malm: cannot write '" << output << "': " << std::strerror(e) << '\n';
    }
    const bool ok = e == 0;
    return ok;
}

} // namespace

int pack_command(std::span<const std::string_view> args, std::istream&, std::ostream&,
                 std::ostream& err) {
    return pack_command_with(args, err, kSelfExe);
}

int pack_command_with(std::span<const std::string_view> args, std::ostream& err,
                      const char* self_exe) {
    std::string_view input;
    std::string_view output;
    bool have_output = false;
    bool classic = false;
    bool exit_a = false;
    bool options_done = false;
    std::size_t max_cells = core::kDefaultMaxCells;
    bool max_cells_given = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (!options_done && arg == "--") {
            options_done = true;
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
        } else if (!options_done && arg == "-o") {
            if (i + 1 == args.size()) {
                return usage_error(err, "option '-o' needs a value");
            }
            if (have_output) {
                return usage_error(err, "-o given twice");
            }
            output = args[++i];
            have_output = true;
        } else if ((!options_done && arg.starts_with('-')) || !input.empty()) {
            return usage_error(err, "unexpected argument '" + std::string(arg) + "'");
        } else {
            input = arg;
        }
    }
    if (input.empty()) {
        return usage_error(err, "missing input file");
    }
    if (!have_output) {
        return usage_error(err, "missing -o <out>");
    }
    if (output == "-") {
        return usage_error(err, "-o - is not supported (a binary cannot be written to a terminal)");
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
    if (same_file(std::string(output), std::string(input))) {
        err << "malm: refusing to overwrite the input file '" << output << "'\n";
        return 1;
    }
    std::vector<std::uint8_t> payload;
    try {
        payload = encode_malm(std::move(prog), exit_a, container::Codec::Zstd);
    } catch (const std::bad_alloc&) {
        err << "malm: out of memory\n";
        return 1;
    }
    return write_packed(std::string(output), payload, self_exe, err) ? 0 : 1;
}

bool write_packed(const std::string& output, std::span<const std::uint8_t> malm,
                  const char* self_exe, std::ostream& err) {
    if (is_dynamic_loader(self_exe)) {
        err << "malm: refusing to pack: the running binary is the dynamic loader; run malm "
               "directly, "
               "not through ld.so\n";
        return false;
    }
    if (same_file(output, self_exe)) {
        err << "malm: refusing to overwrite the running binary '" << output << "'\n";
        return false;
    }

    std::vector<std::uint8_t> base;
    try {
        if (!read_base_binary(self_exe, base, err)) {
            return false;
        }
    } catch (const std::bad_alloc&) {
        err << "malm: out of memory\n";
        return false;
    }
    const auto trailer = pack::make_trailer({base.size(), malm.size()});

#ifndef MALM_STATIC
    err << "malm: warning: this malm is dynamically linked; the packed executable needs the same "
           "shared libraries at run time\n";
#endif
    return write_executable(output, {base, malm, trailer}, err);
}

} // namespace malm
