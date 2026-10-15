#include "container/container.hpp"

#include <zstd.h>

#include <algorithm>
#include <cassert>
#include <memory>
#include <new>
#include <thread>

namespace malm::container {
namespace {

// Codec 2 is written at level 19 with zstd's window for it (2^23), on up to kMaxWorkers threads;
// codec 1 at level 9, as before codec 2 existed. Measurements: docs/benchmarks.md (issue #39).
constexpr int kZstdLevel = 19;
constexpr int kZstdPackedLevel = 9;
// More workers do not help at level 19 (each job is a few windows long). zstd's output is the same
// for any number of workers from 1 on (tested), so a file does not depend on the machine.
constexpr unsigned kMaxWorkers = 8;

// Decompression and unpacking work in chunks of this size.
constexpr std::size_t kChunk = std::size_t{1} << 20;

// Largest zstd window the reader accepts (2^24 = 16 MiB; level 9 writes 2^21). Reader policy:
// the window is what zstd allocates, so a hostile frame header cannot ask for more.
constexpr int kZstdWindowLogMax = 24;

// Slicing-by-8 tables: kCrcTable[0] is the byte-wise table; kCrcTable[k][b] is the CRC state of
// byte b followed by k zero bytes, so 8 bytes cost 8 independent lookups.
constexpr std::array<std::array<std::uint32_t, 256>, 8> kCrcTable = [] {
    std::array<std::array<std::uint32_t, 256>, 8> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) != 0 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        }
        t[0][i] = c;
    }
    for (std::size_t k = 1; k < 8; ++k) {
        for (std::size_t i = 0; i < 256; ++i) {
            t[k][i] = t[0][t[k - 1][i] & 0xFFu] ^ (t[k - 1][i] >> 8);
        }
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
    std::size_t i = 0;
    for (; data.size() - i >= 8; i += 8) {
        const std::uint32_t lo = c ^ static_cast<std::uint32_t>(get_le(data, i, 4));
        const auto hi = static_cast<std::uint32_t>(get_le(data, i + 4, 4));
        c = kCrcTable[7][lo & 0xFFu] ^ kCrcTable[6][(lo >> 8) & 0xFFu] ^
            kCrcTable[5][(lo >> 16) & 0xFFu] ^ kCrcTable[4][lo >> 24] ^ kCrcTable[3][hi & 0xFFu] ^
            kCrcTable[2][(hi >> 8) & 0xFFu] ^ kCrcTable[1][(hi >> 16) & 0xFFu] ^
            kCrcTable[0][hi >> 24];
    }
    for (; i < data.size(); ++i) {
        c = kCrcTable[0][(c ^ data[i]) & 0xFFu] ^ (c >> 8);
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

namespace {

struct CCtxDeleter {
    void operator()(ZSTD_CCtx* c) const { ZSTD_freeCCtx(c); }
};

// One zstd frame of `src` (it declares its content size).
std::vector<std::uint8_t> zstd_compress(std::span<const std::uint8_t> src, int level,
                                        unsigned workers) {
    const std::unique_ptr<ZSTD_CCtx, CCtxDeleter> cctx(ZSTD_createCCtx());
    if (!cctx) {
        throw std::bad_alloc();
    }
    [[maybe_unused]] std::size_t r =
        ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, level);
    assert(!ZSTD_isError(r));
    r = ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_nbWorkers, static_cast<int>(workers));
    assert(!ZSTD_isError(r));
    std::vector<std::uint8_t> out(ZSTD_compressBound(src.size()));
    const std::size_t z =
        ZSTD_compress2(cctx.get(), out.data(), out.size(), src.data(), src.size());
    if (ZSTD_isError(z)) {
        throw std::bad_alloc(); // the bound fits, so only allocation can fail
    }
    out.resize(z);
    return out;
}

} // namespace

std::vector<std::uint8_t> write_malm(const Program& p, Codec codec) {
    const std::size_t n = p.cells.size();
    assert(n >= 2 && n <= max_cells(p.mode));

    // Pack: cell i occupies bits 3i..3i+2, LSB first. Codec 2 also keeps the symbols, one per byte.
    std::vector<std::uint8_t> packed;
    packed.reserve(static_cast<std::size_t>(packed_size(n)));
    std::vector<std::uint8_t> symbols;
    if (codec == Codec::Zstd) {
        symbols.reserve(n);
    }
    std::uint32_t acc = 0;
    int nbits = 0;
    std::size_t pos_mod94 = 0;
    for (const std::uint8_t cell : p.cells) {
        const std::uint8_t s = normalize(pos_mod94, cell);
        assert(s != kNoSymbol);
        if (codec == Codec::Zstd) {
            symbols.push_back(s);
        }
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
        packed = {}; // only the CRC needed it
        const unsigned workers = std::clamp(std::thread::hardware_concurrency(), 1u, kMaxWorkers);
        payload = zstd_compress(symbols, kZstdLevel, workers);
    } else if (codec == Codec::ZstdPacked) {
        payload = zstd_compress(packed, kZstdPackedLevel, 0);
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

// kDenorm[p][s] = denormalize(p mod 94, s). Rows 94..100 repeat rows 0..6, so the 8 cells of a
// group starting at any position p < 94 are rows p..p+7, without a wrap inside the group.
constexpr auto kDenorm = [] {
    std::array<std::array<std::uint8_t, 8>, 94 + 7> t{};
    for (std::size_t p = 0; p < t.size(); ++p) {
        for (std::uint8_t s = 0; s < 8; ++s) {
            t[p][s] = denormalize(p % 94, s);
        }
    }
    return t;
}();

// Consumes the packed byte stream: CRC, byte count and 3-bit symbols denormalized straight into
// `cells`. Holds the carry bits of a symbol that straddles two chunks.
class Unpacker {
  public:
    Unpacker(std::uint64_t cells, std::uint64_t packed_bytes, std::vector<std::uint8_t>& out)
        : cells_(cells), expect_(packed_bytes), out_(out) {
        out_.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(cells, kChunk)));
    }

    // Precondition: the stream stays within the expected packed size.
    void feed(std::span<const std::uint8_t> chunk) {
        assert(chunk.size() <= expect_ - total_);
        total_ += chunk.size();
        crc_ = crc_update(crc_, chunk);
        // The cells this chunk completes, written in place.
        const std::uint64_t n = std::min<std::uint64_t>(
            cells_ - produced_, (static_cast<std::uint64_t>(nbits_) + 8 * chunk.size()) / 3);
        const std::size_t start = out_.size();
        out_.resize(start + static_cast<std::size_t>(n));
        std::uint8_t* dst = out_.data() + start;
        std::uint8_t* const end = dst + n;
        produced_ += n;

        const auto feed_byte = [&](std::uint8_t byte) {
            acc_ |= static_cast<std::uint32_t>(byte) << nbits_;
            nbits_ += 8;
            while (nbits_ >= 3 && dst != end) {
                *dst++ = kDenorm[pos_][acc_ & 7u];
                acc_ >>= 3;
                nbits_ -= 3;
                pos_ = pos_ == 93 ? 0 : pos_ + 1;
            }
        };
        std::size_t i = 0;
        while (i < chunk.size() && nbits_ != 0) { // up to the next group boundary
            feed_byte(chunk[i++]);
        }
        // Whole groups: 3 bytes hold 8 cells.
        while (chunk.size() - i >= 3 && end - dst >= 8) {
            const std::uint32_t g = static_cast<std::uint32_t>(chunk[i]) |
                                    static_cast<std::uint32_t>(chunk[i + 1]) << 8 |
                                    static_cast<std::uint32_t>(chunk[i + 2]) << 16;
            for (std::size_t k = 0; k < 8; ++k) {
                dst[k] = kDenorm[pos_ + k][(g >> (3 * k)) & 7u];
            }
            dst += 8;
            i += 3;
            pos_ = pos_ >= 86 ? pos_ - 86 : pos_ + 8;
        }
        while (i < chunk.size()) {
            feed_byte(chunk[i++]);
        }
    }

    // Codec 2: one symbol per byte, denormalized into `cells` and packed for the CRC (which is that
    // of the packed bytes for every codec); bad_symbol() tells if a byte was above 7.
    // Precondition: the stream stays within the cell count.
    void feed_symbols(std::span<const std::uint8_t> chunk) {
        assert(chunk.size() <= cells_ - produced_);
        produced_ += chunk.size();
        const std::size_t start = out_.size();
        out_.resize(start + chunk.size());
        std::uint8_t* dst = out_.data() + start;
        std::uint64_t all = 0; // OR of the bytes, 8 at a time: a bit of 0xF8 set if one is above 7
        std::array<std::uint8_t, 3 * 1024 + 8> packed;
        std::size_t np = 0;
        const auto flush = [&] {
            crc_ = crc_update(crc_, std::span<const std::uint8_t>(packed.data(), np));
            total_ += np;
            np = 0;
        };
        const auto feed_one = [&](std::uint8_t s) {
            all |= s;
            s &= 7u;
            *dst++ = kDenorm[pos_][s];
            pos_ = pos_ == 93 ? 0 : pos_ + 1;
            acc_ |= static_cast<std::uint32_t>(s) << nbits_;
            nbits_ += 3;
            if (nbits_ >= 8) {
                packed[np++] = static_cast<std::uint8_t>(acc_);
                acc_ >>= 8;
                nbits_ -= 8;
            }
        };
        std::size_t i = 0;
        while (i < chunk.size() && nbits_ != 0) { // up to the next group boundary
            feed_one(chunk[i++]);
        }
        // Whole groups: 8 cells pack into 3 bytes.
        while (chunk.size() - i >= 8) {
            std::uint64_t v = get_le(chunk, i, 8);
            all |= v;
            v &= 0x0707070707070707u;
            for (std::size_t k = 0; k < 8; ++k) {
                dst[k] = kDenorm[pos_ + k][(v >> (8 * k)) & 7u];
            }
            // Gather the 3-bit fields: pairs into 6 bits, then 12, then 24.
            v = (v | v >> 5) & 0x003F003F003F003Fu;
            v = (v | v >> 10) & 0x00000FFF00000FFFu;
            const auto g = static_cast<std::uint32_t>((v | v >> 20) & 0xFFFFFFu);
            packed[np] = static_cast<std::uint8_t>(g);
            packed[np + 1] = static_cast<std::uint8_t>(g >> 8);
            packed[np + 2] = static_cast<std::uint8_t>(g >> 16);
            np += 3;
            dst += 8;
            i += 8;
            pos_ = pos_ >= 86 ? pos_ - 86 : pos_ + 8;
            if (np >= 3 * 1024) {
                flush();
            }
        }
        while (i < chunk.size()) {
            feed_one(chunk[i++]);
        }
        flush();
        bad_symbol_ = bad_symbol_ || (all & 0xF8F8F8F8F8F8F8F8u) != 0;
    }

    // Codec 2, after the last symbol: the last, partly filled byte (zero-padded).
    void finish_symbols() {
        if (nbits_ > 0) {
            const auto last = static_cast<std::uint8_t>(acc_);
            crc_ = crc_update(crc_, std::span<const std::uint8_t>(&last, 1));
            ++total_;
            acc_ = 0;
            nbits_ = 0;
        }
    }

    bool bad_symbol() const { return bad_symbol_; }
    bool complete() const { return total_ == expect_ && produced_ == cells_; }
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
    bool bad_symbol_ = false;
};

struct DCtxDeleter {
    void operator()(ZSTD_DCtx* d) const { ZSTD_freeDCtx(d); }
};

// Streams the single zstd frame `payload`, whose content must be `expect` bytes, into `sink`, a
// callable that takes each chunk and returns an error message or empty. Returns the same.
template <class Sink>
std::string stream_zstd(std::span<const std::uint8_t> payload, std::uint64_t expect, Sink&& sink) {
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
    std::uint64_t total = 0;
    for (;;) {
        ZSTD_outBuffer out{buf.data(), buf.size(), 0};
        const std::size_t ret = ZSTD_decompressStream(dctx.get(), &out, &in);
        if (ZSTD_isError(ret)) {
            return std::string("zstd: decompression failed: ") + ZSTD_getErrorName(ret);
        }
        if (out.pos > expect - total) {
            return "zstd: frame produces more data than its declared content size";
        }
        total += out.pos;
        if (std::string e = sink(std::span<const std::uint8_t>(buf.data(), out.pos)); !e.empty()) {
            return e;
        }
        if (ret == 0) {
            break; // frame complete
        }
        if (in.pos == in.size && out.pos < out.size) {
            return "zstd: truncated frame";
        }
    }
    if (in.pos != in.size || total != expect) {
        return "zstd: decompressed " + std::to_string(total) + " bytes, expected " +
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
    if (codec_byte > 2) {
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
    } else if (codec == Codec::ZstdPacked) {
        // stream_zstd never passes more than `expect` bytes.
        const auto sink = [&](std::span<const std::uint8_t> chunk) {
            un.feed(chunk);
            return std::string();
        };
        if (std::string e = stream_zstd(payload, expect, sink); !e.empty()) {
            return fail(std::move(e));
        }
    } else {
        const auto sink = [&](std::span<const std::uint8_t> chunk) {
            un.feed_symbols(chunk); // at most `cells` bytes in all, as above
            return un.bad_symbol() ? std::string("zstd: a byte of the symbols is above 7")
                                   : std::string();
        };
        if (std::string e = stream_zstd(payload, cells, sink); !e.empty()) {
            return fail(std::move(e));
        }
        un.finish_symbols();
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
