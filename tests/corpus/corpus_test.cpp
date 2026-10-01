// Conformance corpus: runs every program of the manifest (MALM_CORPUS_MANIFEST: manifest.txt for
// the fast test, manifest_slow.txt for the slow one) in classic mode through ClassicVm with a
// string-backed Io and checks the outcome and the output (length and SHA-256). Provenance and
// licences: SOURCES.md; expected values: README.md.
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/classic.hpp"
#include "core/loader.hpp"
#include "core/vm.hpp"

namespace {

using namespace malm::core;

#ifndef MALM_CORPUS_MANIFEST
#error "MALM_CORPUS_MANIFEST not defined"
#endif

const std::string kDir = MALM_CORPUS_DIR;

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), "cannot open " << path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// SHA-256 (FIPS 180-4), for the expected-output hashes.
std::string sha256_hex(const std::string& data) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg = data;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) {
        msg.push_back('\0');
    }
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
    }
    const auto rotr = [](std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = 0;
            for (std::size_t j = 0; j < 4; ++j) {
                w[i] = (w[i] << 8) | static_cast<unsigned char>(msg[off + 4 * i + j]);
            }
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto v = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
            const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const std::uint32_t t1 = v[7] + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
            const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const std::uint32_t t2 = s0 + maj;
            v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
        }
        for (std::size_t i = 0; i < 8; ++i) {
            h[i] += v[i];
        }
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (const std::uint32_t x : h) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            hex.push_back(digits[(x >> shift) & 0xf]);
        }
    }
    return hex;
}

struct StrIo : Io {
    std::string in;
    std::size_t pos = 0;
    std::string out;
    int read_byte() override {
        return pos >= in.size() ? kEof : static_cast<unsigned char>(in[pos++]);
    }
    bool write_byte(std::uint8_t b) override {
        out.push_back(static_cast<char>(b));
        return true;
    }
    bool flush() override { return true; }
};

// One line of the manifest:
//   name program stdin step_cap outcome steps out_len out_sha256
// `-` for no stdin / no step cap / no step count. outcome: halt | limit | E_LOAD@<byte offset> |
// E_EXEC@<step> | E_ENCRYPT@<step>. `steps` is the exact step count of a halting program (the run
// is capped at steps + 1, so a program that no longer halts there fails instead of hanging).
struct Entry {
    std::string name, program, stdin_file, outcome, sha;
    std::optional<std::uint64_t> cap, steps;
    std::size_t out_len = 0;
};

std::vector<Entry> read_manifest() {
    std::vector<Entry> entries;
    std::ifstream f(kDir + "/" MALM_CORPUS_MANIFEST);
    REQUIRE_MESSAGE(f.good(), "cannot open " MALM_CORPUS_MANIFEST);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream ls(line);
        Entry e;
        std::string cap, steps, extra;
        REQUIRE_MESSAGE((ls >> e.name >> e.program >> e.stdin_file >> cap >> e.outcome >> steps >>
                         e.out_len >> e.sha),
                        "bad manifest line (expected 8 fields): " << line);
        REQUIRE_MESSAGE(!(ls >> extra),
                        "bad manifest line (extra token '" << extra << "'): " << line);
        if (cap != "-") {
            e.cap = std::stoull(cap);
        }
        if (steps != "-") {
            e.steps = std::stoull(steps);
        }
        entries.push_back(std::move(e));
    }
    return entries;
}

// Checks one entry; failures are reported and the caller goes on with the next entry.
void check_entry(const Entry& e) {
    INFO("corpus program " << e.name << " (" << e.program << ")");
    const std::vector<std::uint8_t> text = read_file(kDir + "/" + e.program);
    const LoadResult loaded = load_text(text, kClassicCells);

    const std::size_t at = e.outcome.find('@');
    const std::string kind = e.outcome.substr(0, at);
    const std::uint64_t n = at == std::string::npos ? 0 : std::stoull(e.outcome.substr(at + 1));

    if (kind == "E_LOAD") {
        CHECK_MESSAGE(!loaded.has_value(), e.name << " loaded but E_LOAD was expected");
        if (!loaded.has_value()) {
            CHECK_MESSAGE(loaded.error().offset == n, e.name << ": " << loaded.error().reason);
        }
        return;
    }
    if (!loaded.has_value()) {
        FAIL_CHECK(e.name << ": " << loaded.error().reason);
        return;
    }
    if (kind == "halt" && !e.steps) {
        FAIL_CHECK(e.name << ": a halt entry needs a step count");
        return;
    }

    StrIo io;
    if (e.stdin_file != "-") {
        const auto in = read_file(kDir + "/" + e.stdin_file);
        io.in.assign(in.begin(), in.end());
    }
    ClassicVm vm(loaded.value());
    const std::optional<std::uint64_t> cap = kind == "halt" ? std::optional(*e.steps + 1) : e.cap;
    const RunResult r = vm.run(io, cap);

    if (kind == "halt") {
        CHECK_MESSAGE(r.kind == RunResult::Kind::Halted, e.name << ": did not halt");
        CHECK_MESSAGE(vm.steps() == *e.steps, e.name << ": step count");
    } else if (kind == "limit") {
        CHECK_MESSAGE(r.kind == RunResult::Kind::StepLimit, e.name);
        CHECK(vm.steps() == e.cap.value_or(0));
    } else if (kind == "E_EXEC" || kind == "E_ENCRYPT") {
        const Error want = kind == "E_EXEC" ? Error::Exec : Error::Encrypt;
        if (r.kind != RunResult::Kind::Fault) {
            FAIL_CHECK(e.name << ": no fault");
        } else {
            CHECK_MESSAGE(r.fault->error == want, e.name << ": " << format_fault(*r.fault));
            CHECK_MESSAGE(r.fault->steps == n, e.name << ": " << format_fault(*r.fault));
        }
    } else {
        FAIL_CHECK(e.name << ": bad outcome " << e.outcome);
        return;
    }
    CHECK_MESSAGE(io.out.size() == e.out_len, e.name << ": output length");
    CHECK_MESSAGE(sha256_hex(io.out) == e.sha, e.name << ": output SHA-256");
}

} // namespace

TEST_CASE("sha256 known answers") {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(std::string(1000, 'a')) ==
          "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST_CASE("classic corpus programs produce their expected outcome and output") {
    const std::vector<Entry> entries = read_manifest();
    CHECK(!entries.empty());
    for (const Entry& e : entries) {
        check_entry(e);
    }
}
