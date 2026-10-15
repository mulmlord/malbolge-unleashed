#include <doctest/doctest.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "container/container.hpp"
#include "core/loader.hpp"
#include "core/tables.hpp"

namespace {

using namespace malm::container;
namespace core = malm::core;
using Bytes = std::vector<std::uint8_t>;

Bytes read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), "cannot open " << p);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Random loadable program of `n` cells.
Program random_program(std::mt19937_64& rng, std::size_t n, Mode mode = Mode::Unleashed) {
    Program p;
    p.mode = mode;
    p.cells.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        p.cells.push_back(denormalize(i, static_cast<std::uint8_t>(rng() % 8)));
    }
    return p;
}

void put_u32(Bytes& b, std::size_t off, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) {
        b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}
void put_u64(Bytes& b, std::size_t off, std::uint64_t v) {
    for (std::size_t i = 0; i < 8; ++i) {
        b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

// Recomputes the CRC field from a codec-0 file's payload.
void fix_crc(Bytes& b) {
    put_u32(b, 12, crc32(std::span<const std::uint8_t>(b).subspan(kHeaderSize)));
}

std::string error_of(const Bytes& b) {
    const auto r = read_malm(b);
    REQUIRE_FALSE(r.has_value());
    return r.error();
}

bool contains(const std::string& s, std::string_view sub) {
    return s.find(sub) != std::string::npos;
}

Program small_program() {
    std::mt19937_64 rng(1);
    return random_program(rng, 11); // 33 bits: 5 bytes, 7 padding bits
}

} // namespace

TEST_CASE("crc32 matches the IEEE check value") {
    const std::string s = "123456789";
    CHECK(crc32({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}) == 0xCBF43926u);
    CHECK(crc32({}) == 0u);
}

TEST_CASE("crc32 matches a bitwise CRC at every length and alignment") {
    const auto bitwise = [](std::span<const std::uint8_t> data) {
        std::uint32_t c = 0xFFFFFFFFu;
        for (const std::uint8_t b : data) {
            c ^= b;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) != 0 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            }
        }
        return c ^ 0xFFFFFFFFu;
    };
    std::mt19937_64 rng(3);
    Bytes data(300);
    for (auto& b : data) {
        b = static_cast<std::uint8_t>(rng());
    }
    for (std::size_t off = 0; off < 8; ++off) {
        for (std::size_t len = 0; off + len <= data.size(); len += 1 + len / 16) {
            const std::span<const std::uint8_t> s(data.data() + off, len);
            CHECK_MESSAGE(crc32(s) == bitwise(s), "off=" << off << " len=" << len);
        }
    }
}

TEST_CASE("normalization is a bijection at every position") {
    for (std::size_t pos = 0; pos < 94; ++pos) {
        std::array<bool, 8> seen{};
        int loadable = 0;
        for (std::uint32_t x = 33; x <= 126; ++x) {
            const std::uint8_t s = normalize(pos, static_cast<std::uint8_t>(x));
            CHECK(core::is_loadable(static_cast<std::uint32_t>(pos), x) == (s != kNoSymbol));
            if (s == kNoSymbol) {
                continue;
            }
            ++loadable;
            REQUIRE(s < 8);
            CHECK_FALSE(seen[s]);
            seen[s] = true;
            // The symbol is the instruction letter xlat1[(x - 33 + pos) % 94].
            CHECK(kSymbolLetters[s] == core::kXlat1[(x - 33 + pos) % 94]);
            CHECK(denormalize(pos, s) == x);
        }
        CHECK(loadable == 8);
        CHECK(std::ranges::all_of(seen, [](bool b) { return b; }));
        // And the other direction: every symbol denormalizes to a loadable cell that maps back.
        for (std::uint8_t s = 0; s < 8; ++s) {
            const std::uint8_t c = denormalize(pos, s);
            CHECK(core::is_loadable(static_cast<std::uint32_t>(pos), c));
            CHECK(normalize(pos, c) == s);
        }
    }
    // Positions beyond 94 wrap.
    CHECK(normalize(94 * 1000 + 5, 'x') == normalize(5, 'x'));
    CHECK(denormalize(94 * 1000 + 5, 3) == denormalize(5, 3));
}

TEST_CASE("MALM is never a valid text program") {
    CHECK_FALSE(core::is_loadable(0, 'M'));
    const std::string m = "MALM";
    CHECK_FALSE(is_malm(Bytes{}));
    CHECK(is_malm({reinterpret_cast<const std::uint8_t*>(m.data()), m.size()}));
}

TEST_CASE("write_malm header layout") {
    const Program p = small_program();
    const Bytes b = write_malm(p, Codec::Packed);
    REQUIRE(b.size() == kHeaderSize + 5);
    CHECK(std::string(b.begin(), b.begin() + 4) == "MALM");
    CHECK(b[4] == 1);
    CHECK(b[5] == 0);
    CHECK(b[6] == 0); // unleashed
    CHECK(b[7] == 0);
    CHECK(b[8] == 0); // packed
    CHECK((b[9] | b[10] | b[11]) == 0);
    CHECK(b[16] == 11);
    CHECK(b[24] == 5);
    CHECK(b[12] == (crc32(std::span<const std::uint8_t>(b).subspan(kHeaderSize)) & 0xff));

    Program c = p;
    c.mode = Mode::Classic;
    c.exit_a = true;
    const Bytes bc = write_malm(c, Codec::ZstdPacked);
    CHECK(bc[6] == 1);
    CHECK(bc[7] == 1);
    CHECK(bc[8] == 1);
    CHECK(write_malm(c)[8] == 2); // the default codec
}

TEST_CASE("packing is 3 bits per cell, LSB first") {
    // Symbols 0..7 at cells 0..7: bits 000 001 010 011 100 101 110 111 from bit 0 upward.
    Program p;
    for (std::size_t i = 0; i < 8; ++i) {
        p.cells.push_back(denormalize(i, static_cast<std::uint8_t>(i)));
    }
    const Bytes b = write_malm(p, Codec::Packed);
    REQUIRE(b.size() == kHeaderSize + 3);
    const std::uint32_t v =
        0u | (1u << 3) | (2u << 6) | (3u << 9) | (4u << 12) | (5u << 15) | (6u << 18) | (7u << 21);
    CHECK(b[kHeaderSize] == (v & 0xff));
    CHECK(b[kHeaderSize + 1] == ((v >> 8) & 0xff));
    CHECK(b[kHeaderSize + 2] == ((v >> 16) & 0xff));
}

TEST_CASE("round trip: random programs of many lengths, every codec, both modes") {
    std::mt19937_64 rng(42);
    std::vector<std::size_t> lengths = {2, 3, 4, 5, 7, 8, 9, 10, 11, 94, 95, 1000, 4097};
    for (int i = 0; i < 20; ++i) {
        lengths.push_back(2 + rng() % 5000);
    }
    lengths.push_back(59049);
    for (const std::size_t n : lengths) {
        for (const Mode mode : {Mode::Unleashed, Mode::Classic}) {
            for (const Codec codec : {Codec::Packed, Codec::ZstdPacked, Codec::Zstd}) {
                Program p = random_program(rng, n, mode);
                p.exit_a = (rng() & 1) != 0;
                const Bytes b = write_malm(p, codec);
                const auto r = read_malm(b);
                REQUIRE_MESSAGE(r.has_value(), "n=" << n << ": " << r.error());
                CHECK(r->cells == p.cells);
                CHECK(r->mode == p.mode);
                CHECK(r->exit_a == p.exit_a);
            }
        }
    }
}

TEST_CASE("round trip: 59050 cells is valid unleashed, rejected for classic") {
    std::mt19937_64 rng(7);
    const Program p = random_program(rng, 59050);
    Bytes b = write_malm(p, Codec::Packed);
    const auto r = read_malm(b);
    REQUIRE(r.has_value());
    CHECK(r->cells == p.cells);
    b[6] = 1; // claim classic
    const std::string e = error_of(b);
    CHECK(contains(e, "cell count 59050"));
    CHECK(contains(e, "classic"));
}

TEST_CASE("round trip: every loadable committed program") {
    int unleashed = 0;
    int classic = 0;
    int files = 0;
    for (const char* dir : {MALM_PROGRAMS_DIR, MALM_CORPUS_DIR}) {
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            const auto ext = e.path().extension();
            if (ext != ".mal" && ext != ".mb") {
                continue;
            }
            ++files;
            const Bytes text = read_file(e.path());
            CHECK_MESSAGE(!is_malm(text), e.path());
            Bytes stripped;
            std::ranges::copy_if(text, std::back_inserter(stripped), [](std::uint8_t c) {
                return !(c == ' ' || (c >= '\t' && c <= '\r'));
            });
            for (const Mode mode : {Mode::Unleashed, Mode::Classic}) {
                const auto cells = core::load_text(text, max_cells(mode));
                if (!cells) {
                    continue;
                }
                (mode == Mode::Classic ? classic : unleashed)++;
                for (const Codec codec : {Codec::Packed, Codec::ZstdPacked, Codec::Zstd}) {
                    const Bytes b = write_malm({*cells, mode, false}, codec);
                    CHECK_MESSAGE(is_malm(b), e.path());
                    const auto r = read_malm(b);
                    REQUIRE_MESSAGE(r.has_value(), e.path() << ": " << r.error());
                    CHECK_MESSAGE(r->cells == *cells, e.path());
                    CHECK_MESSAGE(r->cells == stripped, e.path()); // identical text, no whitespace
                }
            }
        }
    }
    CHECK(files >= 8);
    CHECK(unleashed >= 8);
    CHECK(classic >= 8);
}

TEST_CASE("validation: every error has its own message") {
    const Program p = small_program();
    const Bytes good = write_malm(p, Codec::Packed);
    REQUIRE(read_malm(good).has_value());

    SUBCASE("bad magic") {
        Bytes b = good;
        b[0] = 'X';
        CHECK(contains(error_of(b), "bad magic"));
        CHECK(contains(error_of(Bytes{}), "bad magic"));
        CHECK(contains(error_of(Bytes{'(', 'P'}), "bad magic"));
    }
    SUBCASE("truncated header") {
        for (const std::size_t n : {4u, 5u, 12u, 31u}) {
            const Bytes b(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
            CHECK_MESSAGE(contains(error_of(b), "truncated header"), n);
        }
    }
    SUBCASE("unsupported version") {
        for (const int v : {0, 2, 255}) {
            Bytes b = good;
            b[4] = static_cast<std::uint8_t>(v);
            CHECK(contains(error_of(b), "unsupported format version"));
        }
        Bytes b = good;
        b[5] = 1; // version 257
        CHECK(contains(error_of(b), "unsupported format version 257"));
    }
    SUBCASE("unknown mode") {
        Bytes b = good;
        b[6] = 2;
        CHECK(contains(error_of(b), "unknown mode 2"));
    }
    SUBCASE("unknown codec") {
        Bytes b = good;
        b[8] = 3;
        CHECK(contains(error_of(b), "unknown codec 3"));
    }
    SUBCASE("unknown flag bits") {
        for (const int f : {2, 0x80, 0xfe}) {
            Bytes b = good;
            b[7] = static_cast<std::uint8_t>(f);
            CHECK(contains(error_of(b), "unknown flag bits"));
        }
        Bytes b = good;
        b[7] = 1; // EXIT_A is fine
        CHECK(read_malm(b).has_value());
    }
    SUBCASE("nonzero reserved bytes") {
        for (const std::size_t off : {9u, 10u, 11u}) {
            Bytes b = good;
            b[off] = 1;
            CHECK(contains(error_of(b), "reserved"));
        }
    }
    SUBCASE("cell count out of range") {
        Bytes b = good;
        put_u64(b, 16, 1);
        CHECK(contains(error_of(b), "cell count 1"));
        put_u64(b, 16, 0);
        CHECK(contains(error_of(b), "cell count 0"));
        put_u64(b, 16, core::kUnleashedMaxCells + 1);
        CHECK(contains(error_of(b), "exceeds"));
        put_u64(b, 16, ~std::uint64_t{0});
        CHECK(contains(error_of(b), "exceeds"));
        put_u64(b, 16, 59050);
        b[6] = 1;
        CHECK(contains(error_of(b), "exceeds the classic limit 59049"));
    }
    SUBCASE("payload length does not match the file") {
        Bytes t(good.begin(), good.end() - 1);
        CHECK(contains(error_of(t), "payload length"));
        CHECK(contains(error_of(t), "truncated"));
        for (const std::size_t n : {kHeaderSize, kHeaderSize + 1, kHeaderSize + 4}) {
            const Bytes u(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
            CHECK(contains(error_of(u), "truncated"));
        }
        Bytes a = good;
        a.push_back(0);
        CHECK(contains(error_of(a), "trailing"));
        Bytes huge = good;
        put_u64(huge, 24, ~std::uint64_t{0});
        CHECK(contains(error_of(huge), "truncated"));
    }
    SUBCASE("packed payload length must match the cell count") {
        Bytes b = good;
        put_u64(b, 16, 100); // needs 38 bytes, payload has 5
        CHECK(contains(error_of(b), "packed payload"));
    }
    SUBCASE("CRC mismatch") {
        for (std::size_t bit = 0; bit < 32; ++bit) {
            Bytes b = good;
            b[12 + bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
            CHECK(contains(error_of(b), "CRC mismatch"));
        }
        Bytes b = good;
        b[kHeaderSize] ^= 1; // payload bit flip
        CHECK(contains(error_of(b), "CRC mismatch"));
    }
    SUBCASE("nonzero padding bits") {
        Bytes b = good;
        b[b.size() - 1] |= 0x80; // 11 cells = 33 bits; bits 33..39 are padding
        fix_crc(b);
        CHECK(contains(error_of(b), "padding"));
        b = good;
        b[b.size() - 1] |= 0x02;
        fix_crc(b);
        CHECK(contains(error_of(b), "padding"));
        // Without the CRC fix it is a CRC error first.
        b = good;
        b[b.size() - 1] |= 0x80;
        CHECK(contains(error_of(b), "CRC mismatch"));
    }
}

TEST_CASE("validation: codec 1, zstd of the packed bytes") {
    std::mt19937_64 rng(3);
    const Program p = random_program(rng, 5000);
    const Bytes good = write_malm(p, Codec::ZstdPacked);
    REQUIRE(read_malm(good).has_value());
    const std::size_t packed = (3 * 5000 + 7) / 8;

    SUBCASE("corrupted frame fails to decompress or fails the CRC") {
        Bytes b = good;
        b[b.size() / 2] ^= 0x55;
        const std::string e = error_of(b);
        CHECK((contains(e, "zstd") || contains(e, "CRC mismatch")));
        // Smash the frame magic: not a zstd frame at all.
        b = good;
        b[kHeaderSize] ^= 0xff;
        CHECK(contains(error_of(b), "zstd"));
    }
    SUBCASE("truncated frame") {
        Bytes b(good.begin(), good.end() - 3);
        CHECK(contains(error_of(b), "truncated"));
        // Consistent header, cut frame.
        put_u64(b, 24, b.size() - kHeaderSize);
        CHECK(contains(error_of(b), "zstd"));
    }
    SUBCASE("frame content size differs from the cell count") {
        // Header claims 5001 cells but the frame holds the packed bytes of 5000.
        Bytes b = good;
        put_u64(b, 16, 5001);
        const std::string e = error_of(b);
        CHECK(contains(e, "content size"));
        CHECK(contains(e, std::to_string(packed)));
    }
    SUBCASE("frame without a content size") {
        Bytes pk = write_malm(p, Codec::Packed);
        const std::span<const std::uint8_t> raw =
            std::span<const std::uint8_t>(pk).subspan(kHeaderSize);
        ZSTD_CCtx* cctx = ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0);
        Bytes frame(ZSTD_compressBound(raw.size()));
        const std::size_t n =
            ZSTD_compress2(cctx, frame.data(), frame.size(), raw.data(), raw.size());
        ZSTD_freeCCtx(cctx);
        REQUIRE_FALSE(ZSTD_isError(n));
        frame.resize(n);
        Bytes b(pk.begin(), pk.begin() + kHeaderSize);
        b[8] = 1;
        put_u64(b, 24, frame.size());
        b.insert(b.end(), frame.begin(), frame.end());
        CHECK(contains(error_of(b), "content size"));
        CHECK(contains(error_of(b), "does not declare"));
    }
    SUBCASE("a claim beyond the reader cap is rejected before decompressing") {
        Bytes b = good;
        put_u64(b, 16, core::kUnleashedMaxCells);
        const std::string e = error_of(b);
        CHECK(contains(e, "above the limit"));
        CHECK(contains(e, "--max-cells"));
    }
    SUBCASE("data after the frame: empty frame, skippable frame") {
        const Bytes empty_frame = {0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x00, 0x01, 0x00, 0x00};
        const Bytes skippable = {0x50, 0x2a, 0x4d, 0x18, 4, 0, 0, 0, 'h', 'i', 'd', 'e'};
        for (const Bytes& extra : {empty_frame, skippable, Bytes{0}}) {
            Bytes b = good;
            b.insert(b.end(), extra.begin(), extra.end());
            put_u64(b, 24, b.size() - kHeaderSize);
            const std::string e = error_of(b);
            CHECK_MESSAGE(contains(e, "after the first frame"), e);
        }
    }
    SUBCASE("a skippable frame in front is rejected") {
        const Bytes skippable = {0x50, 0x2a, 0x4d, 0x18, 4, 0, 0, 0, 'h', 'i', 'd', 'e'};
        Bytes b(good.begin(), good.begin() + kHeaderSize);
        b.insert(b.end(), skippable.begin(), skippable.end());
        b.insert(b.end(), good.begin() + kHeaderSize, good.end());
        put_u64(b, 24, b.size() - kHeaderSize);
        CHECK(contains(error_of(b), "zstd"));
    }
    SUBCASE("CRC mismatch after a valid decompression") {
        Bytes b = good;
        b[12] ^= 1;
        CHECK(contains(error_of(b), "CRC mismatch"));
    }
    SUBCASE("nonzero padding bits inside a valid frame") {
        const Program q = small_program(); // 33 bits
        Bytes pk = write_malm(q, Codec::Packed);
        pk[pk.size() - 1] |= 0x80;
        const std::span<const std::uint8_t> raw =
            std::span<const std::uint8_t>(pk).subspan(kHeaderSize);
        Bytes frame(ZSTD_compressBound(raw.size()));
        const std::size_t n = ZSTD_compress(frame.data(), frame.size(), raw.data(), raw.size(), 3);
        REQUIRE_FALSE(ZSTD_isError(n));
        frame.resize(n);
        Bytes b(pk.begin(), pk.begin() + kHeaderSize);
        b[8] = 1;
        put_u32(b, 12, crc32(raw));
        put_u64(b, 24, frame.size());
        b.insert(b.end(), frame.begin(), frame.end());
        CHECK(contains(error_of(b), "padding"));
    }
}

namespace {

// A codec-2 file of `cells` (header from write_malm, CRC of their packed bytes) whose frame holds
// `symbols` instead of the cells' own.
Bytes codec2_with_symbols(const Program& p, const Bytes& symbols, bool content_size = true) {
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, content_size ? 1 : 0);
    Bytes frame(ZSTD_compressBound(symbols.size()));
    const std::size_t n =
        ZSTD_compress2(cctx, frame.data(), frame.size(), symbols.data(), symbols.size());
    ZSTD_freeCCtx(cctx);
    REQUIRE_FALSE(ZSTD_isError(n));
    frame.resize(n);
    Bytes b = write_malm(p, Codec::Zstd);
    b.resize(kHeaderSize);
    put_u64(b, 24, frame.size());
    b.insert(b.end(), frame.begin(), frame.end());
    return b;
}

Bytes symbols_of(const Program& p) {
    Bytes s;
    for (std::size_t i = 0; i < p.cells.size(); ++i) {
        s.push_back(normalize(i, p.cells[i]));
    }
    return s;
}

} // namespace

TEST_CASE("validation: codec 2, zstd of one symbol per byte") {
    std::mt19937_64 rng(4);
    const Program p = random_program(rng, 5000);
    const Bytes good = write_malm(p, Codec::Zstd);
    REQUIRE(good[8] == 2);
    REQUIRE(read_malm(good).has_value());
    // The CRC is that of the packed bytes, as for the other codecs.
    const Bytes pk = write_malm(p, Codec::Packed);
    CHECK(std::equal(good.begin() + 12, good.begin() + 16, pk.begin() + 12));
    // A frame built here, outside write_malm, reads back too.
    REQUIRE(read_malm(codec2_with_symbols(p, symbols_of(p))).has_value());

    SUBCASE("corrupted frame fails to decompress or fails the CRC") {
        Bytes b = good;
        b[b.size() / 2] ^= 0x55;
        const std::string e = error_of(b);
        CHECK((contains(e, "zstd") || contains(e, "CRC mismatch")));
    }
    SUBCASE("truncated frame") {
        Bytes b(good.begin(), good.end() - 3);
        put_u64(b, 24, b.size() - kHeaderSize);
        CHECK(contains(error_of(b), "zstd"));
    }
    SUBCASE("frame content size differs from the cell count") {
        Bytes b = good;
        put_u64(b, 16, 5001);
        const std::string e = error_of(b);
        CHECK(contains(e, "content size 5000"));
        CHECK(contains(e, "expected 5001"));
    }
    SUBCASE("frame without a content size") {
        CHECK(contains(error_of(codec2_with_symbols(p, symbols_of(p), false)), "does not declare"));
    }
    SUBCASE("a byte above 7 is not a symbol") {
        for (const int bad : {8, 0x80, 0xff}) {
            for (const std::size_t at : {std::size_t{0}, std::size_t{2500}, std::size_t{4999}}) {
                Bytes s = symbols_of(p);
                s[at] = static_cast<std::uint8_t>(bad);
                CHECK_MESSAGE(contains(error_of(codec2_with_symbols(p, s)), "above 7"),
                              "byte " << bad << " at " << at);
            }
        }
    }
    SUBCASE("other symbols fail the CRC") {
        Bytes s = symbols_of(p);
        s[1234] ^= 1;
        CHECK(contains(error_of(codec2_with_symbols(p, s)), "CRC mismatch"));
    }
    SUBCASE("data after the frame") {
        Bytes b = good;
        b.push_back(0);
        put_u64(b, 24, b.size() - kHeaderSize);
        CHECK(contains(error_of(b), "after the first frame"));
    }
    SUBCASE("a claim beyond the reader cap is rejected before decompressing") {
        Bytes b = good;
        put_u64(b, 16, core::kUnleashedMaxCells);
        CHECK(contains(error_of(b), "above the limit"));
    }
}

TEST_CASE("zstd: the bytes do not depend on the number of workers, over several jobs") {
    // write_malm uses up to 8 workers, as many as the machine has; zstd's output is the same for
    // any number from 1 on. A job is 4 windows, at least 512 KiB: with a 2^16 window this input is
    // 4 jobs, where write_malm's 2^23 window would need 128 MiB for as many.
    std::mt19937_64 rng(9);
    Bytes src(std::size_t{2} << 20);
    for (std::size_t i = 0; i < src.size(); i += 4096) {
        // Compressible but not trivial: runs of repeated random blocks.
        const std::size_t len = std::min<std::size_t>(4096, src.size() - i);
        const std::size_t from = i >= 65536 && rng() % 2 == 0 ? i - 4096 * (1 + rng() % 16) : 0;
        for (std::size_t k = 0; k < len; ++k) {
            src[i + k] = from != 0 ? src[from + k] : static_cast<std::uint8_t>(rng() % 8);
        }
    }
    Bytes first;
    for (const int workers : {1, 3, 8}) {
        ZSTD_CCtx* cctx = ZSTD_createCCtx();
        REQUIRE_FALSE(ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 19)));
        REQUIRE_FALSE(ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 16)));
        REQUIRE_FALSE(ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, workers)));
        Bytes z(ZSTD_compressBound(src.size()));
        const std::size_t n = ZSTD_compress2(cctx, z.data(), z.size(), src.data(), src.size());
        ZSTD_freeCCtx(cctx);
        REQUIRE_FALSE(ZSTD_isError(n));
        z.resize(n);
        if (first.empty()) {
            first = z;
        } else {
            CHECK_MESSAGE(z == first, workers << " workers");
        }
    }
}

namespace {

// One ZSTD_compress2 frame of `src`.
Bytes zstd_one_call(const Bytes& src, int level, int workers) {
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    REQUIRE_FALSE(ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level)));
    REQUIRE_FALSE(ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, workers)));
    Bytes z(ZSTD_compressBound(src.size()));
    const std::size_t n = ZSTD_compress2(cctx, z.data(), z.size(), src.data(), src.size());
    ZSTD_freeCCtx(cctx);
    REQUIRE_FALSE(ZSTD_isError(n));
    z.resize(n);
    return z;
}

// write_malm streams codec 2 in chunks on `workers` threads; its frame is the one ZSTD_compress2
// writes over all the symbols at once, whatever the number of workers.
void check_codec2_frame(std::size_t cells, std::initializer_list<unsigned> workers) {
    std::mt19937_64 rng(17 + cells);
    const Program p = random_program(rng, cells);
    const Bytes want = zstd_one_call(symbols_of(p), 19, 1);
    for (const unsigned w : workers) {
        const Bytes b = write_malm(p, Codec::Zstd, w);
        CHECK_MESSAGE(Bytes(b.begin() + kHeaderSize, b.end()) == want,
                      cells << " cells, " << w << " workers");
    }
}

} // namespace

TEST_CASE("codec 2: write_malm's frame does not depend on the number of workers") {
    check_codec2_frame(5000, {1, 8}); // single-threaded below 512 KiB
    check_codec2_frame(600'000, {1, 3, 8});
}

TEST_CASE("codec 2: the frame over several chunks" * doctest::test_suite("slow")) {
    check_codec2_frame((std::size_t{5} << 19) + 3, {1, 8});
}

TEST_CASE("codec 1: the frame is one single-threaded ZSTD_compress2 call") {
    std::mt19937_64 rng(19);
    const Program p = random_program(rng, 600'000);
    const Bytes pk = write_malm(p, Codec::Packed);
    const Bytes b = write_malm(p, Codec::ZstdPacked);
    CHECK(Bytes(b.begin() + kHeaderSize, b.end()) ==
          zstd_one_call(Bytes(pk.begin() + kHeaderSize, pk.end()), 9, 0));
}

TEST_CASE("codec 2: a thread that cannot start is a WriteError") {
    // RLIMIT_NPROC counts the user's threads, so a limit of 0 stops every new one; root is exempt.
    std::mt19937_64 rng(21);
    const Program p = random_program(rng, 600'000); // above 512 KiB, so zstd uses its workers
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        int rc = 3;
        const struct rlimit none{0, 0};
        if (setrlimit(RLIMIT_NPROC, &none) == 0) {
            try {
                (void)write_malm(p, Codec::Zstd, 4);
                rc = 1;
            } catch (const WriteError& e) {
                rc = contains(e.what(), "cannot start a zstd compression thread") ? 0 : 2;
            } catch (...) {
                rc = 2;
            }
        }
        _exit(rc);
    }
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    if (WEXITSTATUS(status) == 1 && geteuid() == 0) {
        MESSAGE("skipped: root is exempt from RLIMIT_NPROC");
        return;
    }
    CHECK(WEXITSTATUS(status) == 0);
}

namespace {

// Symbol distribution of generated code: mostly Nop/Opr. Indices follow kSymbolLetters
// "ji*p</vo": j=MovD i=Jmp *=Rot p=Opr <=Out /=In v=Hlt o=Nop.
Program skewed_program(std::size_t n) {
    std::mt19937_64 rng(2024);
    const std::array<double, 8> weights = {8, 3, 4, 20, 2, 2, 1, 60};
    std::discrete_distribution<int> dist(weights.begin(), weights.end());
    Program p;
    p.cells.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        p.cells.push_back(denormalize(i, static_cast<std::uint8_t>(dist(rng))));
    }
    return p;
}

void report_ratio(const char* name, const Program& p) {
    const Bytes packed = write_malm(p, Codec::Packed);
    const Bytes z = write_malm(p, Codec::Zstd);
    const double raw = static_cast<double>(p.cells.size());
    const double pk = static_cast<double>(packed.size() - kHeaderSize);
    const double zz = static_cast<double>(z.size() - kHeaderSize);
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << "compression [" << name
       << "] cells=" << p.cells.size() << " raw_text=" << raw << " packed=" << pk << " zstd=" << zz
       << " ratio_vs_packed=" << (pk / zz) << " ratio_vs_text=" << (raw / zz);
    MESSAGE(os.str());
    std::cout << os.str() << std::endl;
    const auto r = read_malm(z);
    REQUIRE(r.has_value());
    CHECK(r->cells == p.cells);
}

} // namespace

TEST_CASE("compression ratio of a 1M-cell normalized program") {
    const Program skew = skewed_program(1'000'000);
    report_ratio("skewed", skew);
    const Bytes packed = write_malm(skew, Codec::Packed);
    const Bytes z = write_malm(skew, Codec::Zstd);
    CHECK(z.size() < packed.size()); // the skewed distribution must actually compress

    std::mt19937_64 rng(5);
    report_ratio("uniform", random_program(rng, 1'000'000));
}

namespace {

// A zstd frame header declaring `content` bytes, single segment (window = content).
Bytes zstd_single_segment_header(std::uint64_t content) {
    Bytes f = {0x28, 0xb5, 0x2f, 0xfd, 0xE0};
    for (int i = 0; i < 8; ++i) {
        f.push_back(static_cast<std::uint8_t>(content >> (8 * i)));
    }
    return f;
}

// A valid codec-1 or codec-2 file of `cells` Nop cells, built by hand as RLE blocks (codec 1:
// every packed byte is 0xFF except the padded last one; codec 2: every symbol is 7), so the file
// stays small whatever the cell count. With `real_crc` the CRC is computed (needs the packed bytes
// in memory); otherwise it is left 0.
Bytes nop_malm(std::uint64_t cells, bool real_crc, Codec codec = Codec::ZstdPacked) {
    const std::uint64_t packed_total = (cells * 3 + 7) / 8;
    const unsigned used = static_cast<unsigned>((cells * 3) % 8);
    const std::uint8_t packed_last =
        static_cast<std::uint8_t>(used == 0 ? 0xFFu : (1u << used) - 1);
    const bool symbols = codec == Codec::Zstd;
    const std::uint64_t total = symbols ? cells : packed_total;
    const std::uint8_t fill = symbols ? 7 : 0xFF;
    const std::uint8_t last = symbols ? 7 : packed_last;
    Bytes frame = zstd_single_segment_header(total);
    const auto block = [&](std::uint64_t size, std::uint8_t value, bool is_last) {
        const std::uint32_t h =
            static_cast<std::uint32_t>(size << 3) | (1u << 1) | (is_last ? 1u : 0u);
        frame.push_back(static_cast<std::uint8_t>(h));
        frame.push_back(static_cast<std::uint8_t>(h >> 8));
        frame.push_back(static_cast<std::uint8_t>(h >> 16));
        frame.push_back(value);
    };
    for (std::uint64_t left = total - 1; left > 0;) {
        const std::uint64_t n = std::min<std::uint64_t>(left, 131072);
        block(n, fill, false);
        left -= n;
    }
    block(1, last, true);

    Bytes b(kHeaderSize, 0);
    std::copy(kMagic.begin(), kMagic.end(), b.begin());
    b[4] = 1;
    b[8] = static_cast<std::uint8_t>(codec);
    if (real_crc) {
        Bytes packed(packed_total, 0xFF);
        packed.back() = packed_last;
        put_u32(b, 12, crc32(packed));
    }
    put_u64(b, 16, cells);
    put_u64(b, 24, frame.size());
    b.insert(b.end(), frame.begin(), frame.end());
    return b;
}

} // namespace

TEST_CASE("reader cap: a small valid file claiming 1.6e9 cells is rejected before decompressing") {
    for (const Codec codec : {Codec::ZstdPacked, Codec::Zstd}) {
        const Bytes b = nop_malm(1'600'000'000ull, false, codec);
        CHECK(b.size() < 64 * 1024);
        const std::string e = error_of(b);
        CHECK(contains(e, "program has 1600000000 cells, above the limit 1073741824"));
        CHECK(contains(e, "(use --max-cells)"));
    }
}

static void check_limits(const Bytes& b) {
    auto r = read_malm(b, Limits{4'999'999});
    REQUIRE_FALSE(r.has_value());
    CHECK(contains(r.error(), "program has 5000000 cells, above the limit 4999999"));
    r = read_malm(b, Limits{5'000'000});
    REQUIRE_MESSAGE(r.has_value(), r.error());
    REQUIRE(r->cells.size() == 5'000'000);
    for (std::size_t i = 0; i < r->cells.size(); i += 997) {
        CHECK(normalize(i, r->cells[i]) == 7); // Nop
    }
    // The default cap accepts it too (it is far below 2^30).
    CHECK(read_malm(b).has_value());
    // A raised cap does not lift the classic format limit.
    Bytes c = b;
    c[6] = 1;
    CHECK(contains(read_malm(c, Limits{~std::size_t{0}}).error(), "exceeds the classic limit"));
}

TEST_CASE("reader cap: Limits raises and lowers it; classic keeps its format limit") {
    for (const Codec codec : {Codec::ZstdPacked, Codec::Zstd}) {
        CAPTURE(static_cast<int>(codec));
        check_limits(nop_malm(5'000'000, true, codec));
    }
}

TEST_CASE("garbage frames claiming huge sizes fail cleanly and cheaply") {
    // The header claims a 2 GB frame (codec 1: ~5.3e9 cells; codec 2: 2e9); the cap is raised so
    // the claim reaches zstd. The frame has a window descriptor (so it passes the header checks)
    // and then only zeros.
    const std::uint64_t content = 2'000'000'001ull / 3 * 3;
    Bytes frame = {0x28, 0xb5, 0x2f, 0xfd, 0xC0, 0x40};
    for (int i = 0; i < 8; ++i) {
        frame.push_back(static_cast<std::uint8_t>(content >> (8 * i)));
    }
    for (const Codec codec : {Codec::ZstdPacked, Codec::Zstd}) {
        const std::uint64_t cells = codec == Codec::Zstd ? content : content / 3 * 8;
        for (const bool bad_block : {false, true}) {
            CAPTURE(static_cast<int>(codec));
            CAPTURE(bad_block);
            Bytes f = frame;
            if (bad_block) {
                // One last "compressed" block (type 2) of 100 garbage bytes: the frame is well
                // formed as far as zstd can tell without decoding, but decoding fails in the first
                // block.
                const std::uint32_t h = (100u << 3) | (2u << 1) | 1u;
                f.insert(f.end(), {static_cast<std::uint8_t>(h), static_cast<std::uint8_t>(h >> 8),
                                   static_cast<std::uint8_t>(h >> 16)});
                f.resize(f.size() + 100, 0xA5);
            } else {
                f.resize(64 * 1024);
            }
            Bytes b(kHeaderSize, 0);
            std::copy(kMagic.begin(), kMagic.end(), b.begin());
            b[4] = 1;
            b[8] = static_cast<std::uint8_t>(codec);
            put_u64(b, 16, cells);
            put_u64(b, 24, f.size());
            b.insert(b.end(), f.begin(), f.end());

            struct rusage before{};
            getrusage(RUSAGE_SELF, &before);
            const auto r = read_malm(b, Limits{~std::size_t{0}});
            struct rusage after{};
            getrusage(RUSAGE_SELF, &after);
            REQUIRE_FALSE(r.has_value());
            CHECK_MESSAGE(contains(r.error(), "zstd"), r.error());
            // Peak RSS grows by far less than the claimed 2 GB (ru_maxrss is in KiB).
            CHECK(after.ru_maxrss - before.ru_maxrss < 64 * 1024);
            // And with the default cap it is rejected without touching zstd at all.
            CHECK(contains(error_of(b), "above the limit"));
        }
    }
}

// Round trips with the packed size around multiples of the 1 MiB streaming chunk.
static void check_chunk_boundary(const std::uint64_t target, const std::uint64_t dl) {
    std::mt19937_64 rng(11 + target + dl);
    const std::size_t n = static_cast<std::size_t>(target * 8 / 3 + dl);
    const Program p = random_program(rng, n);
    for (const Codec codec : {Codec::Packed, Codec::ZstdPacked}) {
        const Bytes b = write_malm(p, codec);
        const auto r = read_malm(b);
        REQUIRE_MESSAGE(r.has_value(), "n=" << n << ": " << r.error());
        CHECK_MESSAGE(r->cells == p.cells, "n=" << n);
    }
}

// Codec 2 streams one cell per byte: round trips with the cell count around 1 MiB.
static void check_symbol_chunk_boundary(const std::size_t n) {
    std::mt19937_64 rng(13 + n);
    const Program p = random_program(rng, n);
    const auto r = read_malm(write_malm(p, Codec::Zstd));
    REQUIRE_MESSAGE(r.has_value(), "n=" << n << ": " << r.error());
    CHECK_MESSAGE(r->cells == p.cells, "n=" << n);
}

TEST_CASE("streaming: codec 2 round trip just past a chunk boundary (1 MiB + 3 cells)") {
    check_symbol_chunk_boundary((std::size_t{1} << 20) + 3);
}

TEST_CASE("streaming: codec 2 round trips at chunk boundaries" * doctest::test_suite("slow")) {
    const std::size_t mib = std::size_t{1} << 20;
    for (const std::size_t n : {mib - 1, mib, mib + 1, mib + 8, 2 * mib + 5}) {
        check_symbol_chunk_boundary(n);
    }
}

TEST_CASE("streaming: round trip just past a chunk boundary (packed size 1 MiB + 1)") {
    check_chunk_boundary(std::uint64_t{1} << 20 | 1, 2);
}

TEST_CASE("streaming: round trips at chunk boundaries (packed size around 1 MiB multiples)" *
          doctest::test_suite("slow")) {
    const std::uint64_t mib = 1u << 20;
    for (const std::uint64_t target : {mib - 1, mib, mib + 1, 2 * mib}) {
        for (const std::uint64_t dl : {std::uint64_t{0}, std::uint64_t{2}}) {
            if (target == 2 * mib && dl != 0) {
                continue;
            }
            check_chunk_boundary(target, dl);
        }
    }
}
