#include "container/sha256.hpp"

#include <bit>
#include <cstddef>

namespace malm::container {
namespace {

constexpr std::array<std::uint32_t, 64> kK = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void compress(std::array<std::uint32_t, 8>& h, const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> w;
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = std::uint32_t{block[4 * i]} << 24 | std::uint32_t{block[4 * i + 1]} << 16 |
               std::uint32_t{block[4 * i + 2]} << 8 | std::uint32_t{block[4 * i + 3]};
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, hh] = h;
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t t1 = hh + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                                 ((e & f) ^ (~e & g)) + kK[i] + w[i];
        const std::uint32_t t2 =
            (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

} // namespace

Sha256 sha256(std::span<const std::uint8_t> data) noexcept {
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::size_t i = 0;
    for (; i + 64 <= data.size(); i += 64) {
        compress(h, data.data() + i);
    }
    // The last partial block, the 1 bit, zeros and the length in bits: one or two blocks.
    std::array<std::uint8_t, 128> tail{};
    const std::size_t rest = data.size() - i;
    for (std::size_t j = 0; j < rest; ++j) {
        tail[j] = data[i + j];
    }
    tail[rest] = 0x80;
    const std::size_t blocks = rest < 56 ? 1 : 2;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    for (std::size_t j = 0; j < 8; ++j) {
        tail[blocks * 64 - 1 - j] = static_cast<std::uint8_t>(bits >> (8 * j));
    }
    for (std::size_t b = 0; b < blocks; ++b) {
        compress(h, tail.data() + 64 * b);
    }
    Sha256 out;
    for (std::size_t j = 0; j < 8; ++j) {
        for (std::size_t k = 0; k < 4; ++k) {
            out[4 * j + k] = static_cast<std::uint8_t>(h[j] >> (24 - 8 * k));
        }
    }
    return out;
}

std::string to_hex(const Sha256& h) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (const std::uint8_t b : h) {
        s += kDigits[b >> 4];
        s += kDigits[b & 15];
    }
    return s;
}

} // namespace malm::container
