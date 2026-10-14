// The picolibc sysroot built into malm (issue #32): unpacking zstd-compressed ustar archives, on
// hand-made ones and on the real one if malm has it.

#include <doctest/doctest.h>
#include <unistd.h>
#include <zstd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "driver/runtime_files.hpp"
#include "driver/sysroot.hpp"

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("malm_sysroot_test_" + std::to_string(::getpid()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() { fs::remove_all(path); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// A ustar member: header and data padded to 512-byte blocks. `type` '0' is a file, '5' a
// directory.
std::string member(std::string_view name, std::string_view data = {}, char type = '0') {
    std::string h(512, '\0');
    name.copy(h.data(), name.size());
    std::snprintf(&h[100], 8, "%07o", 0644);
    std::snprintf(&h[124], 12, "%011zo", data.size());
    h[156] = type;
    std::memcpy(&h[257], "ustar", 6); // magic, NUL-terminated
    std::memcpy(&h[263], "00", 2);    // version
    std::memset(&h[148], ' ', 8);
    unsigned sum = 0;
    for (const char c : h) {
        sum += static_cast<unsigned char>(c);
    }
    std::snprintf(&h[148], 7, "%06o", sum);
    std::string padded(data);
    padded.resize((data.size() + 511) / 512 * 512, '\0');
    return h + padded;
}

const std::string kEnd(1024, '\0');

std::string zst(const std::string& tar) {
    std::string z(ZSTD_compressBound(tar.size()), '\0');
    z.resize(ZSTD_compress(z.data(), z.size(), tar.data(), tar.size(), 3));
    return z;
}

std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("sysroot: unpacking files and directories") {
    const TempDir dir;
    const std::string big(1000, 'x');
    const std::string tar = member("include/", {}, '5') + member("include/a.h", "abc") +
                            member("lib/libx.a", big) + member("empty") + kEnd;
    CHECK(malm::unpack_tar_zst(zst(tar), dir.path).empty());
    CHECK(fs::is_directory(dir.path / "include"));
    CHECK(read(dir.path / "include/a.h") == "abc");
    CHECK(read(dir.path / "lib/libx.a") == big);
    CHECK(fs::is_regular_file(dir.path / "empty"));
    CHECK(fs::file_size(dir.path / "empty") == 0);
}

TEST_CASE("sysroot: bad archives") {
    const TempDir dir;
    const auto unpack = [&](const std::string& tar) {
        return malm::unpack_tar_zst(zst(tar), dir.path);
    };
    CHECK(malm::unpack_tar_zst("not zstd", dir.path).starts_with("zstd: "));
    const std::string good = zst(member("a", "abc") + kEnd);
    CHECK(malm::unpack_tar_zst(std::string_view(good).substr(0, good.size() - 4), dir.path) ==
          "zstd: truncated frame");
    CHECK(unpack(member("../evil", "x") + kEnd) == "tar: unsafe member name '../evil'");
    CHECK(unpack(member("/evil", "x") + kEnd) == "tar: unsafe member name '/evil'");
    CHECK(!fs::exists(dir.path.parent_path() / "evil"));
    CHECK(unpack(member("link", {}, '2') + kEnd) ==
          "tar: member 'link' is neither a file nor a directory");
    std::string bad = member("a", "abc");
    bad[0] = 'b'; // the checksum no longer matches
    CHECK(unpack(bad + kEnd) == "tar: bad header at offset 0");
    CHECK(unpack(member("a", std::string(2000, 'x')).substr(0, 1024)) ==
          "tar: member 'a' runs past the end");
    CHECK(unpack(member("a", "abc")) == "tar: no end-of-archive block");
}

TEST_CASE("sysroot: the picolibc sysroot built into malm") {
    if (malm::picolibc_sysroot().empty()) {
        MESSAGE("malm is built without picolibc (MALM_PICOLIBC)");
        return;
    }
    const TempDir dir;
    CHECK(malm::unpack_tar_zst(malm::picolibc_sysroot(), dir.path).empty());
    for (const char* file : {"include/stdio.h", "include/math.h", "include/picolibc.h",
                             "lib/libc.a", "lib/libmalm.a", "lib/libclang_rt.builtins.a"}) {
        INFO(file);
        CHECK(fs::is_regular_file(dir.path / file));
    }
}
