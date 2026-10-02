#include "driver/packed_run.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>
#include <ostream>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "core/loader.hpp"
#include "driver/program_io.hpp"
#include "driver/run_cmd.hpp"
#include "pack/trailer.hpp"

namespace malm {
namespace {

struct Fd {
    int fd;
    ~Fd() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

// Reads exactly `n` bytes at `offset`; false on error or short file.
bool pread_full(int fd, std::uint8_t* dst, std::size_t n, std::uint64_t offset) {
    while (n > 0) {
        const ssize_t got = ::pread(fd, dst, n, static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return false;
        }
        dst += got;
        n -= static_cast<std::size_t>(got);
        offset += static_cast<std::uint64_t>(got);
    }
    return true;
}

int corrupt(std::ostream& err, const std::string& reason) {
    err << "malm: corrupt packed executable: " << reason << '\n';
    return 1;
}

} // namespace

int open_self_exe(const char* path) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0 && fd <= 2) {
        const int high = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
        const int saved = errno;
        ::close(fd);
        errno = saved;
        fd = high;
    }
    return fd;
}

namespace {

enum class Probe { NotPacked, Corrupt, Packed };

// Looks at the executable's tail and, for a packed one, reads and decodes the payload. The file
// is closed on return, so it can never be mistaken for a standard stream while the program runs.
Probe probe_packed(const char* exe_path, std::ostream& err, LoadedProgram& prog, int& status) {
    const Fd exe{open_self_exe(exe_path)};
    if (exe.fd < 0) {
        if (errno == EACCES) {
            err << "malm: note: cannot read own executable (" << std::strerror(errno)
                << "); if this is a packed program, it must be readable\n";
        }
        return Probe::NotPacked; // no /proc, or not readable: behave as the plain CLI
    }
    struct stat st{};
    if (::fstat(exe.fd, &st) != 0 || st.st_size < static_cast<off_t>(pack::kTrailerSize)) {
        return Probe::NotPacked;
    }
    const auto file_size = static_cast<std::uint64_t>(st.st_size);
    std::array<std::uint8_t, pack::kTrailerSize> tail{};
    if (!pread_full(exe.fd, tail.data(), tail.size(), file_size - pack::kTrailerSize)) {
        return Probe::NotPacked;
    }
    const auto parsed = pack::parse_trailer(tail, file_size);
    if (!parsed) {
        status = corrupt(err, parsed.error());
        return Probe::Corrupt;
    }
    if (!parsed->has_value()) {
        return Probe::NotPacked;
    }
    const pack::Trailer trailer = **parsed;

    std::vector<std::uint8_t> payload(static_cast<std::size_t>(trailer.length));
    if (!pread_full(exe.fd, payload.data(), payload.size(), trailer.offset)) {
        status = corrupt(err, "cannot read the payload");
        return Probe::Corrupt;
    }
    // The packer already accepted the program; only the format limit of its mode applies.
    auto p = container::read_malm(payload, container::Limits{core::kUnleashedMaxCells});
    if (!p) {
        status = corrupt(err, "invalid payload: " + p.error());
        return Probe::Corrupt;
    }
    prog.classic = p->mode == container::Mode::Classic;
    prog.exit_a = p->exit_a;
    prog.from_malm = true;
    prog.cells = std::move(p->cells);
    return Probe::Packed;
}

} // namespace

std::optional<int> run_packed(const char* exe_path, std::istream& in, std::ostream& out,
                              std::ostream& err) {
    try {
        LoadedProgram prog;
        int status = 0;
        switch (probe_packed(exe_path, err, prog, status)) {
        case Probe::NotPacked:
            return std::nullopt;
        case Probe::Corrupt:
            return status;
        case Probe::Packed:
            break;
        }
        return run_loaded(prog, false, in, out, err);
    } catch (const std::bad_alloc&) {
        err << "malm: out of memory\n";
        return 1;
    }
}

} // namespace malm
