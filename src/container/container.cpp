#include "container/container.hpp"

#include <zstd.h>

#include <algorithm>
#include <cassert>
#include <memory>

namespace malm::container {
namespace {

// Level 9: a good ratio at negligible cost for programs this size. Tuning is tracked in #39.
constexpr int kZstdLevel = 9;

// Decompression and unpacking work in chunks of this size.
constexpr std::size_t kChunk = std::size_t{1} << 20;

// Largest zstd window the reader accepts (2^24 = 16 MiB; level 9 writes 2^21). Reader policy:
// the window is what zstd allocates, so a hostile frame header cannot ask for more.
constexpr int kZstdWindowLogMax = 24;

constexpr std::array<std::uint32_t, 256> kCrcTable = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) != 0 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        }
        t[i] = c;
    }
    return t;
}();

std::uint64_t packed_size(std::uint64_t cells) { return (cells * 3 + 7) / 8; }

void put_le(std::vector<std::uint8_t>& out, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

std::uint64_t get_le(std::span<const std::uint8_t> b, std::size_t off, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v |= static_cast<std::uint64_t>(b[off + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return v;
}

} // namespace

namespace {

// Streamable CRC state (before the final XOR).
std::uint32_t crc_update(std::uint32_t c, std::span<const std::uint8_t> data) noexcept {
    for (const std::uint8_t b : data) {
        c = kCrcTable[(c ^ b) & 0xFFu] ^ (c >> 8);
    }
    return c;
}

} // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept {
    return crc_update(0xFFFFFFFFu, data) ^ 0xFFFFFFFFu;
}

bool is_malm(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() >= kMagic.size() &&
           std::equal(kMagic.begin(), kMagic.end(), bytes.begin(),
                      [](char m, std::uint8_t b) { return static_cast<std::uint8_t>(m) == b; });
}

std::vector<std::uint8_t> write_malm(const Program& p, Codec codec) {
    const std::size_t n = p.cells.size();
    assert(n >= 2 && n <= max_cells(p.mode));

    // Pack: cell i occupies bits 3i..3i+2, LSB first.
    std::vector<std::uint8_t> packed;
    packed.reserve(static_cast<std::size_t>(packed_size(n)));
    std::uint32_t acc = 0;
    int nbits = 0;
    std::size_t pos_mod94 = 0;
    for (const std::uint8_t cell : p.cells) {
        const std::uint8_t s = normalize(pos_mod94, cell);
        assert(s != kNoSymbol);
        acc |= static_cast<std::uint32_t>(s & 7u) << nbits;
        nbits += 3;
        if (nbits >= 8) {
            packed.push_back(static_cast<std::uint8_t>(acc & 0xFFu));
            acc >>= 8;
            nbits -= 8;
        }
        pos_mod94 = pos_mod94 == 93 ? 0 : pos_mod94 + 1;
    }
    if (nbits > 0) {
        packed.push_back(static_cast<std::uint8_t>(acc & 0xFFu));
    }
    assert(packed.size() == packed_size(n));
    const std::uint32_t crc = crc32(packed);

    std::vector<std::uint8_t> payload;
    if (codec == Codec::Zstd) {
        payload.resize(ZSTD_compressBound(packed.size()));
        const std::size_t z =
            ZSTD_compress(payload.data(), payload.size(), packed.data(), packed.size(), kZstdLevel);
        assert(!ZSTD_isError(z));
        payload.resize(z);
    } else {
        payload = std::move(packed);
    }

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + payload.size());
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    put_le(out, kVersion, 2);
    out.push_back(static_cast<std::uint8_t>(p.mode));
    out.push_back(p.exit_a ? kFlagExitA : 0);
    out.push_back(static_cast<std::uint8_t>(codec));
    put_le(out, 0, 3);
    put_le(out, crc, 4);
    put_le(out, n, 8);
    put_le(out, payload.size(), 8);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

namespace {

using ReadResult = core::Result<Program, std::string>;

ReadResult fail(std::string msg) { return ReadResult::err(std::move(msg)); }

} // namespace

namespace {

// Consumes the packed byte stream: CRC, byte count and 3-bit symbols denormalized straight into
// `cells`. Holds the carry bits of a symbol that straddles two chunks.
class Unpacker {
  public:
    Unpacker(std::uint64_t cells, std::uint64_t packed_bytes, std::vector<std::uint8_t>& out)
        : cells_(cells), expect_(packed_bytes), out_(out) {
        out_.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(cells, kChunk)));
    }

    // False if the stream would exceed the expected packed size.
    bool feed(std::span<const std::uint8_t> chunk) {
        if (chunk.size() > expect_ - total_) {
            return false;
        }
        total_ += chunk.size();
        crc_ = crc_update(crc_, chunk);
        for (const std::uint8_t byte : chunk) {
            acc_ |= static_cast<std::uint32_t>(byte) << nbits_;
            nbits_ += 8;
            while (nbits_ >= 3 && produced_ < cells_) {
                out_.push_back(denormalize(pos_, static_cast<std::uint8_t>(acc_ & 7u)));
                acc_ >>= 3;
                nbits_ -= 3;
                ++produced_;
                pos_ = pos_ == 93 ? 0 : pos_ + 1;
            }
        }
        return true;
    }

    bool complete() const { return total_ == expect_ && produced_ == cells_; }
    std::uint64_t total() const { return total_; }
    std::uint32_t crc() const { return crc_ ^ 0xFFFFFFFFu; }
    // Bits left after the last cell are the padding; they must be zero.
    bool padding_zero() const { return acc_ == 0; }

  private:
    std::uint64_t cells_;
    std::uint64_t expect_;
    std::vector<std::uint8_t>& out_;
    std::uint64_t total_ = 0;
    std::uint64_t produced_ = 0;
    std::uint32_t crc_ = 0xFFFFFFFFu;
    std::uint32_t acc_ = 0;
    int nbits_ = 0;
    std::size_t pos_ = 0;
};

struct DCtxDeleter {
    void operator()(ZSTD_DCtx* d) const { ZSTD_freeDCtx(d); }
};

// Streams the single zstd frame `payload` into `un`. Returns an error message or empty.
std::string stream_zstd(std::span<const std::uint8_t> payload, std::uint64_t expect, Unpacker& un) {
    const unsigned long long content = ZSTD_getFrameContentSize(payload.data(), payload.size());
    if (content == ZSTD_CONTENTSIZE_ERROR) {
        return "zstd: payload is not a valid zstd frame";
    }
    if (content == ZSTD_CONTENTSIZE_UNKNOWN) {
        return "zstd: frame does not declare its content size";
    }
    if (content != expect) {
        return "zstd: frame content size " + std::to_string(content) +
               " does not match the expected " + std::to_string(expect);
    }
    // The payload must be exactly one frame: no hidden data, skippable or extra frames after it.
    const std::size_t frame = ZSTD_findFrameCompressedSize(payload.data(), payload.size());
    if (ZSTD_isError(frame)) {
        return std::string("zstd: invalid frame: ") + ZSTD_getErrorName(frame);
    }
    if (frame != payload.size()) {
        return "zstd: payload has " + std::to_string(payload.size() - frame) +
               " bytes of data after the first frame";
    }

    const std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx(ZSTD_createDCtx());
    if (!dctx) {
        return "zstd: cannot create a decompression context";
    }
    ZSTD_DCtx_setParameter(dctx.get(), ZSTD_d_windowLogMax, kZstdWindowLogMax);
    std::vector<std::uint8_t> buf(kChunk);
    ZSTD_inBuffer in{payload.data(), payload.size(), 0};
    for (;;) {
        ZSTD_outBuffer out{buf.data(), buf.size(), 0};
        const std::size_t ret = ZSTD_decompressStream(dctx.get(), &out, &in);
        if (ZSTD_isError(ret)) {
            return std::string("zstd: decompression failed: ") + ZSTD_getErrorName(ret);
        }
        if (!un.feed(std::span<const std::uint8_t>(buf.data(), out.pos))) {
            return "zstd: frame produces more data than its declared content size";
        }
        if (ret == 0) {
            break; // frame complete
        }
        if (in.pos == in.size && out.pos < out.size) {
            return "zstd: truncated frame";
        }
    }
    if (in.pos != in.size || un.total() != expect) {
        return "zstd: decompressed " + std::to_string(un.total()) + " bytes, expected " +
               std::to_string(expect);
    }
    return {};
}

} // namespace

ReadResult read_malm(std::span<const std::uint8_t> bytes, Limits limits) {
    if (!is_malm(bytes)) {
        return fail("bad magic (not a .malm file)");
    }
    if (bytes.size() < kHeaderSize) {
        return fail("truncated header (" + std::to_string(bytes.size()) + " of " +
                    std::to_string(kHeaderSize) + " bytes)");
    }
    const std::uint64_t version = get_le(bytes, 4, 2);
    if (version != kVersion) {
        return fail("unsupported format version " + std::to_string(version) +
                    " (this build reads " + std::to_string(kVersion) + ")");
    }
    const std::uint8_t mode_byte = bytes[6];
    if (mode_byte > 1) {
        return fail("unknown mode " + std::to_string(mode_byte));
    }
    const Mode mode = static_cast<Mode>(mode_byte);
    const std::uint8_t flags = bytes[7];
    if ((flags & ~kFlagExitA) != 0) {
        return fail("unknown flag bits set (flags byte " + std::to_string(flags) + ")");
    }
    const std::uint8_t codec_byte = bytes[8];
    if (codec_byte > 1) {
        return fail("unknown codec " + std::to_string(codec_byte));
    }
    const Codec codec = static_cast<Codec>(codec_byte);
    if (bytes[9] != 0 || bytes[10] != 0 || bytes[11] != 0) {
        return fail("reserved header bytes are not zero");
    }
    const std::uint32_t crc = static_cast<std::uint32_t>(get_le(bytes, 12, 4));
    const std::uint64_t cells = get_le(bytes, 16, 8);
    const std::uint64_t payload_len = get_le(bytes, 24, 8);

    if (cells < 2) {
        return fail("cell count " + std::to_string(cells) + " is below the minimum of 2");
    }
    if (cells > max_cells(mode)) {
        return fail("cell count " + std::to_string(cells) + " exceeds the " +
                    (mode == Mode::Classic ? "classic limit 59049" : "unleashed limit 3^38"));
    }
    // Reader policy, checked before anything is decompressed or allocated.
    if (cells > limits.max_cells) {
        return fail("program has " + std::to_string(cells) + " cells, above the limit " +
                    std::to_string(limits.max_cells) + " (use --max-cells)");
    }
    const std::uint64_t present = bytes.size() - kHeaderSize;
    if (payload_len > present) {
        return fail("payload length " + std::to_string(payload_len) + " but only " +
                    std::to_string(present) + " bytes follow the header (truncated file)");
    }
    if (payload_len < present) {
        return fail("payload length " + std::to_string(payload_len) + " but " +
                    std::to_string(present) + " bytes follow the header (trailing data)");
    }
    const std::span<const std::uint8_t> payload = bytes.subspan(kHeaderSize);
    const std::uint64_t expect = packed_size(cells);

    Program p;
    p.mode = mode;
    p.exit_a = (flags & kFlagExitA) != 0;
    Unpacker un(cells, expect, p.cells);
    if (codec == Codec::Packed) {
        if (payload_len != expect) {
            return fail("packed payload length " + std::to_string(payload_len) +
                        " does not match " + std::to_string(cells) + " cells (expected " +
                        std::to_string(expect) + ")");
        }
        for (std::size_t off = 0; off < payload.size(); off += kChunk) {
            un.feed(payload.subspan(off, std::min(kChunk, payload.size() - off)));
        }
    } else if (std::string e = stream_zstd(payload, expect, un); !e.empty()) {
        return fail(std::move(e));
    }
    if (!un.complete()) {
        return fail("internal error: unpacked " + std::to_string(p.cells.size()) + " of " +
                    std::to_string(cells) + " cells");
    }
    if (un.crc() != crc) {
        return fail("CRC mismatch (the file is corrupted)");
    }
    if (!un.padding_zero()) {
        return fail("nonzero padding bits after the last cell");
    }
    return ReadResult::ok(std::move(p));
}

} // namespace malm::container
