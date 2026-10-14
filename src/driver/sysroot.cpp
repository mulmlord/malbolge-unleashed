#include "driver/sysroot.hpp"

#include <zstd.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <system_error>
#include <vector>

namespace malm {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kBlock = 512;

struct DCtxDeleter {
    void operator()(ZSTD_DCtx* d) const { ZSTD_freeDCtx(d); }
};

// The zstd frames of `in`, decompressed into `out`. Returns an error message, or empty.
std::string decompress(std::string_view in, std::string& out) {
    const std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx(ZSTD_createDCtx());
    if (!dctx) {
        return "zstd: cannot create a decompression context";
    }
    std::vector<char> buf(ZSTD_DStreamOutSize());
    ZSTD_inBuffer zin{in.data(), in.size(), 0};
    for (;;) {
        ZSTD_outBuffer zout{buf.data(), buf.size(), 0};
        const std::size_t ret = ZSTD_decompressStream(dctx.get(), &zout, &zin);
        if (ZSTD_isError(ret)) {
            return std::string("zstd: ") + ZSTD_getErrorName(ret);
        }
        out.append(buf.data(), zout.pos);
        if (zin.pos == zin.size && zout.pos < zout.size) {
            return ret == 0 ? "" : "zstd: truncated frame";
        }
    }
}

// A ustar header's octal number field: optional leading spaces, digits, then NUL or space.
bool octal(std::string_view field, std::uint64_t& v) {
    v = 0;
    std::size_t i = field.find_first_not_of(' ');
    if (i == std::string_view::npos || field[i] < '0' || field[i] > '7') {
        return false;
    }
    for (; i < field.size() && field[i] >= '0' && field[i] <= '7'; ++i) {
        v = v * 8 + static_cast<std::uint64_t>(field[i] - '0');
    }
    return i == field.size() || field[i] == '\0' || field[i] == ' ';
}

// A NUL-terminated header field.
std::string_view cstr(std::string_view field) { return field.substr(0, field.find('\0')); }

bool safe(const fs::path& p) {
    if (p.empty() || p.is_absolute()) {
        return false;
    }
    for (const fs::path& part : p) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

std::string untar(std::string_view tar, const fs::path& dir) {
    std::size_t pos = 0;
    while (pos + kBlock <= tar.size()) {
        const std::string_view h = tar.substr(pos, kBlock);
        if (h.find_first_not_of('\0') == std::string_view::npos) {
            return {}; // the end-of-archive block
        }
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < kBlock; ++i) {
            sum += i >= 148 && i < 156 ? std::uint64_t{' '} : static_cast<unsigned char>(h[i]);
        }
        std::uint64_t check = 0;
        std::uint64_t size = 0;
        if (!h.substr(257).starts_with("ustar") || !octal(h.substr(148, 8), check) ||
            check != sum || !octal(h.substr(124, 12), size)) {
            return "tar: bad header at offset " + std::to_string(pos);
        }
        const std::string_view prefix = cstr(h.substr(345, 155));
        const fs::path name = fs::path(prefix) / cstr(h.substr(0, 100));
        if (!safe(name)) {
            return "tar: unsafe member name '" + name.string() + "'";
        }
        pos += kBlock;
        if (size > tar.size() - pos) {
            return "tar: member '" + name.string() + "' runs past the end";
        }
        const char type = h[156];
        std::error_code ec;
        if (type == '5') {
            fs::create_directories(dir / name, ec);
        } else if (type == '0' || type == '\0') {
            fs::create_directories((dir / name).parent_path(), ec);
            std::ofstream f(dir / name, std::ios::binary);
            f.write(tar.data() + pos, static_cast<std::streamsize>(size));
            if (!f) {
                return "cannot write " + (dir / name).string();
            }
        } else {
            return "tar: member '" + name.string() + "' is neither a file nor a directory";
        }
        if (ec) {
            return "cannot create " + (dir / name).string() + ": " + ec.message();
        }
        pos += static_cast<std::size_t>((size + kBlock - 1) / kBlock * kBlock);
    }
    return "tar: no end-of-archive block";
}

} // namespace

std::string unpack_tar_zst(std::string_view archive, const fs::path& dir) {
    std::string tar;
    std::string error = decompress(archive, tar);
    return error.empty() ? untar(tar, dir) : error;
}

} // namespace malm
