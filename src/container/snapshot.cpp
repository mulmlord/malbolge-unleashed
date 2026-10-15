#include "container/snapshot.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <memory>
#include <optional>

#include "container/container.hpp"
#include "core/memory.hpp"
#include "core/tables.hpp"

namespace malm::container {
namespace {

using core::Memory;
using core::Value;

constexpr std::uint64_t kPageCells = Memory::kPageCells;
// Page indices are below 3^40 / 3^8.
constexpr std::uint64_t kPageLimit = core::pow3[core::kTrits - 8];

// Level 9, as for .malm payloads: snapshots of large programs are a few MB at most.
constexpr int kZstdLevel = 9;
constexpr std::size_t kChunk = std::size_t{1} << 20;
constexpr int kZstdWindowLogMax = 24;

// Run kinds: 0 the cells as loaded, 1..6 encrypted that many times, 7 literal values.
constexpr unsigned kMaxEncryptions = 6;
constexpr unsigned kLiteral = 7;
// The longest page body: its index and one literal run of 3^8 values, varints of at most 10
// bytes each. Bounds the decompressed body before anything is decompressed.
constexpr std::uint64_t kMaxPageBytes = 10 + 10 + 10 * kPageCells;

// kEncrypted[k][x]: the cell x (33..126) encrypted k times (spec 5, step 4).
constexpr auto kEncrypted = [] {
    std::array<std::array<std::uint8_t, 127>, kMaxEncryptions + 1> t{};
    for (std::uint32_t x = 33; x <= 126; ++x) {
        std::uint32_t y = x;
        for (unsigned k = 0; k <= kMaxEncryptions; ++k) {
            t[k][x] = static_cast<std::uint8_t>(y);
            y = core::encrypt(y);
        }
    }
    return t;
}();

bool is_cell(Value v) noexcept { return v.raw() >= 33 && v.raw() <= 126; }

// What address `addr` holds when the program is loaded (spec 2.4).
Value initial(std::uint64_t addr, std::span<const std::uint8_t> cells,
              const std::array<Value, 6>& fill) noexcept {
    return addr < cells.size() ? Value::from_raw(cells[addr]) : fill[addr % 6];
}

// The run kind of a cell that holds `v` and was loaded as `init`.
unsigned classify(Value init, Value v) noexcept {
    if (v == init) {
        return 0;
    }
    if (is_cell(init)) {
        for (unsigned k = 1; k <= kMaxEncryptions; ++k) {
            if (kEncrypted[k][init.raw()] == v.raw()) {
                return k;
            }
        }
    }
    return kLiteral;
}

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

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

unsigned initial_n(std::size_t cells) {
    return std::max(10U, Value::natural(cells - 1)->width() + 1);
}

// Walks a body (docs/snapshot.md), checking its structure and that it fits the program, and
// reports it to `sink`: page(idx) per page, then same(off, len), encrypted(off, len, k) and
// literal(off, v) for its runs, in order. Returns an error message or empty.
template <class Sink>
std::string walk(std::span<const std::uint8_t> body, std::uint64_t pages,
                 std::span<const std::uint8_t> cells, const std::array<Value, 6>& fill,
                 Sink& sink) {
    std::size_t pos = 0;
    const auto varint = [&]() -> std::optional<std::uint64_t> {
        std::uint64_t v = 0;
        for (unsigned shift = 0; pos < body.size() && shift < 64; shift += 7) {
            const std::uint8_t b = body[pos++];
            if (shift == 63 && b > 1) {
                return std::nullopt;
            }
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) {
                return v;
            }
        }
        return std::nullopt;
    };
    const std::uint64_t program_pages = (cells.size() + kPageCells - 1) / kPageCells;
    std::uint64_t next = 0; // the lowest index the next page can have
    for (std::uint64_t p = 0; p < pages; ++p) {
        const auto gap = varint();
        if (!gap) {
            return "truncated or malformed body at page " + std::to_string(p);
        }
        if (*gap >= kPageLimit - next) {
            return "page index out of range at page " + std::to_string(p);
        }
        const std::uint64_t idx = next + *gap;
        // The program's pages are materialised when it loads, and stay so.
        if (p < program_pages && idx != p) {
            return "page " + std::to_string(p) + " of the program is missing";
        }
        next = idx + 1;
        sink.page(idx);
        const std::uint64_t base = idx * kPageCells;
        for (std::uint64_t off = 0; off < kPageCells;) {
            const auto token = varint();
            if (!token) {
                return "truncated or malformed run in page " + std::to_string(idx);
            }
            const unsigned kind = static_cast<unsigned>(*token & 7);
            const std::uint64_t len = (*token >> 3) + 1;
            if (len > kPageCells - off) {
                return "run past the end of page " + std::to_string(idx);
            }
            if (kind == 0) {
                sink.same(off, len);
            } else if (kind == kLiteral) {
                for (std::uint64_t i = 0; i < len; ++i) {
                    const auto v = varint();
                    if (!v || *v >= core::pow3[core::kTrits]) {
                        return "malformed literal in page " + std::to_string(idx);
                    }
                    sink.literal(off + i, Value::from_raw(*v));
                }
            } else {
                // Only cells encrypt (spec 5). Program cells are; past the program, the fill
                // pattern repeats every 6 cells.
                const std::uint64_t first = std::max<std::uint64_t>(base + off, cells.size());
                const std::uint64_t end = base + off + len;
                for (std::uint64_t a = first; a < end && a < first + 6; ++a) {
                    if (!is_cell(fill[a % 6])) {
                        return "encrypted run over a non-cell in page " + std::to_string(idx);
                    }
                }
                sink.encrypted(off, len, kind);
            }
            off += len;
        }
    }
    if (pages < program_pages) {
        return "the snapshot has " + std::to_string(pages) + " pages, the program " +
               std::to_string(program_pages);
    }
    if (pos != body.size()) {
        return std::to_string(body.size() - pos) + " bytes of data after the last page";
    }
    return {};
}

struct NoSink {
    void page(std::uint64_t) {}
    void same(std::uint64_t, std::uint64_t) {}
    void encrypted(std::uint64_t, std::uint64_t, unsigned) {}
    void literal(std::uint64_t, Value) {}
};

// Writes the runs into `out`, a page that holds what the program loaded.
struct ApplyRuns {
    std::span<Value, kPageCells> out;

    void same(std::uint64_t, std::uint64_t) {}
    void encrypted(std::uint64_t off, std::uint64_t len, unsigned k) {
        const auto& table = kEncrypted[k];
        for (std::uint64_t i = off; i < off + len; ++i) {
            out[i] = Value::from_raw(table[out[i].raw()]);
        }
    }
    void literal(std::uint64_t off, Value v) { out[off] = v; }
};

struct RestoreSink : ApplyRuns {
    Memory& mem;

    // Page 0 holds the program's first cells: always there, and the walk's first page.
    explicit RestoreSink(Memory& m) : ApplyRuns{m.page_for_write(0)}, mem(m) {}
    void page(std::uint64_t idx) { out = mem.page_for_write(idx); }
};

// Builds each page of the snapshot in a buffer and compares it with the VM's.
struct CompareSink {
    using Page = std::array<Value, kPageCells>;

    CompareSink(const Memory& m, std::span<const std::uint8_t> c)
        : mem(m), cells(c), vm_pages(m.page_indices()), buf(std::make_unique<Page>()), runs{*buf} {}

    const Memory& mem;
    std::span<const std::uint8_t> cells;
    std::vector<std::uint64_t> vm_pages; // ascending
    std::size_t next = 0;                // into vm_pages
    std::unique_ptr<Page> buf;
    ApplyRuns runs;
    std::optional<std::uint64_t> idx; // the page in buf
    std::string diff;

    void page(std::uint64_t i) {
        check();
        if (!diff.empty()) {
            idx.reset(); // the rest of the walk only finishes the walk
            return;
        }
        if (next == vm_pages.size() || vm_pages[next] != i) {
            const std::uint64_t other = next == vm_pages.size() ? i : std::min(i, vm_pages[next]);
            diff = "page " + std::to_string(other) + " is materialised in only one of them";
            return;
        }
        ++next;
        idx = i;
        for (std::uint64_t k = 0; k < kPageCells; ++k) {
            (*buf)[k] = initial(i * kPageCells + k, cells, mem.fill_pattern());
        }
    }
    void same(std::uint64_t, std::uint64_t) {}
    void encrypted(std::uint64_t off, std::uint64_t len, unsigned k) {
        if (idx) {
            runs.encrypted(off, len, k);
        }
    }
    void literal(std::uint64_t off, Value v) {
        if (idx) {
            runs.literal(off, v);
        }
    }

    // Compares the page in buf.
    void check() {
        if (!idx || !diff.empty()) {
            return;
        }
        const auto vm = mem.page(*idx);
        for (std::uint64_t k = 0; k < kPageCells; ++k) {
            if (vm[k] != (*buf)[k]) {
                diff = "cell " + Value::from_raw(*idx * kPageCells + k).to_string() + " is " +
                       vm[k].to_string() + ", the snapshot has " + (*buf)[k].to_string();
                return;
            }
        }
        idx.reset();
    }
};

struct DCtxDeleter {
    void operator()(ZSTD_DCtx* d) const { ZSTD_freeDCtx(d); }
};

// Decompresses the single zstd frame `payload` into `out`, at most `max` bytes. Memory grows only
// as data is produced. Returns an error message or empty.
std::string decompress(std::span<const std::uint8_t> payload, std::uint64_t max,
                       std::vector<std::uint8_t>& out) {
    const unsigned long long content = ZSTD_getFrameContentSize(payload.data(), payload.size());
    if (content == ZSTD_CONTENTSIZE_ERROR) {
        return "zstd: payload is not a valid zstd frame";
    }
    if (content == ZSTD_CONTENTSIZE_UNKNOWN) {
        return "zstd: frame does not declare its content size";
    }
    if (content > max) {
        return "zstd: frame content size " + std::to_string(content) +
               " is too large for its pages";
    }
    const std::size_t frame = ZSTD_findFrameCompressedSize(payload.data(), payload.size());
    if (ZSTD_isError(frame) || frame != payload.size()) {
        return "zstd: the payload is not exactly one frame";
    }
    const std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx(ZSTD_createDCtx());
    if (!dctx) {
        return "zstd: cannot create a decompression context";
    }
    ZSTD_DCtx_setParameter(dctx.get(), ZSTD_d_windowLogMax, kZstdWindowLogMax);
    ZSTD_inBuffer in{payload.data(), payload.size(), 0};
    for (;;) {
        const std::size_t have = out.size();
        out.resize(have + kChunk);
        ZSTD_outBuffer o{out.data() + have, kChunk, 0};
        const std::size_t ret = ZSTD_decompressStream(dctx.get(), &o, &in);
        out.resize(have + o.pos);
        if (ZSTD_isError(ret)) {
            return std::string("zstd: decompression failed: ") + ZSTD_getErrorName(ret);
        }
        if (out.size() > content) {
            return "zstd: frame produces more data than its declared content size";
        }
        if (ret == 0) {
            break;
        }
        if (in.pos == in.size && o.pos < o.size) {
            return "zstd: truncated frame";
        }
    }
    if (out.size() != content) {
        return "zstd: decompressed " + std::to_string(out.size()) + " bytes, expected " +
               std::to_string(content);
    }
    return {};
}

} // namespace

std::vector<std::uint8_t> write_snapshot(const core::Vm& vm, const SnapshotProgram& prog) {
    assert(!vm.halted());
    const Memory& mem = vm.memory();
    const auto& fill = mem.fill_pattern();
    const std::vector<std::uint64_t> pages = mem.page_indices();

    std::vector<std::uint8_t> body;
    std::vector<std::uint64_t> literals;
    std::uint64_t next = 0;
    for (const std::uint64_t idx : pages) {
        put_varint(body, idx - next);
        next = idx + 1;
        const auto page = mem.page(idx);
        unsigned kind = 0;
        std::uint64_t start = 0;
        const auto flush = [&](std::uint64_t end) {
            put_varint(body, (end - start - 1) << 3 | kind);
            for (const std::uint64_t v : literals) {
                put_varint(body, v);
            }
            literals.clear();
        };
        for (std::uint64_t i = 0; i < kPageCells; ++i) {
            const unsigned k = classify(initial(idx * kPageCells + i, prog.cells, fill), page[i]);
            if (k != kind && i > start) {
                flush(i);
                start = i;
            }
            kind = k;
            if (k == kLiteral) {
                literals.push_back(page[i].raw());
            }
        }
        flush(kPageCells);
    }

    std::vector<std::uint8_t> payload(ZSTD_compressBound(body.size()));
    const std::size_t z =
        ZSTD_compress(payload.data(), payload.size(), body.data(), body.size(), kZstdLevel);
    assert(!ZSTD_isError(z));
    payload.resize(z);

    std::vector<std::uint8_t> out;
    out.reserve(kSnapshotHeaderSize + payload.size());
    out.insert(out.end(), kSnapshotMagic.begin(), kSnapshotMagic.end());
    put_le(out, kSnapshotVersion, 2);
    out.push_back(0); // unleashed mode
    out.push_back(0);
    put_le(out, crc32(body), 4);
    out.insert(out.end(), prog.file_hash.begin(), prog.file_hash.end());
    put_le(out, prog.cells.size(), 8);
    put_le(out, vm.steps(), 8);
    put_le(out, vm.a().raw(), 8);
    put_le(out, vm.c().raw(), 8);
    put_le(out, vm.d().raw(), 8);
    out.push_back(static_cast<std::uint8_t>(vm.n()));
    put_le(out, 0, 7);
    put_le(out, pages.size(), 8);
    put_le(out, payload.size(), 8);
    assert(out.size() == kSnapshotHeaderSize);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

core::Result<Snapshot, std::string> Snapshot::read(std::span<const std::uint8_t> bytes,
                                                   const SnapshotProgram& prog) {
    using R = core::Result<Snapshot, std::string>;
    const auto fail = [](std::string msg) { return R::err(std::move(msg)); };
    if (bytes.size() < kSnapshotMagic.size() ||
        !std::equal(kSnapshotMagic.begin(), kSnapshotMagic.end(), bytes.begin(),
                    [](char m, std::uint8_t b) { return static_cast<std::uint8_t>(m) == b; })) {
        return fail("bad magic (not a snapshot)");
    }
    if (bytes.size() < kSnapshotHeaderSize) {
        return fail("truncated header");
    }
    if (const std::uint64_t v = get_le(bytes, 8, 2); v != kSnapshotVersion) {
        return fail("unsupported snapshot version " + std::to_string(v));
    }
    if (bytes[10] != 0) {
        return fail("unknown mode " + std::to_string(bytes[10]));
    }
    if (bytes[11] != 0 || !std::all_of(bytes.begin() + 89, bytes.begin() + 96,
                                       [](std::uint8_t b) { return b == 0; })) {
        return fail("reserved header bytes are not zero");
    }
    if (!std::equal(prog.file_hash.begin(), prog.file_hash.end(), bytes.begin() + 16)) {
        return fail("made for another program file (SHA-256 differs)");
    }
    if (get_le(bytes, 48, 8) != prog.cells.size()) {
        return fail("cell count differs from the program's");
    }
    Snapshot s;
    s.steps_ = get_le(bytes, 56, 8);
    const std::uint64_t a = get_le(bytes, 64, 8);
    const std::uint64_t c = get_le(bytes, 72, 8);
    const std::uint64_t d = get_le(bytes, 80, 8);
    s.n_ = bytes[88];
    s.pages_ = get_le(bytes, 96, 8);
    const std::uint64_t payload_len = get_le(bytes, 104, 8);
    if (s.n_ < initial_n(prog.cells.size()) || s.n_ > static_cast<unsigned>(core::kMaxWidth)) {
        return fail("n " + std::to_string(s.n_) + " out of range");
    }
    const std::uint64_t limit = core::pow3[core::kTrits];
    if (a >= limit || c >= limit || d >= limit) {
        return fail("register value out of range");
    }
    s.regs_ = {Value::from_raw(a), Value::from_raw(c), Value::from_raw(d)};
    // Spec 2.2: C and D are addressable.
    if (!s.regs_.c.fits_width(s.n_ - 1) || !s.regs_.d.fits_width(s.n_ - 1)) {
        return fail("C or D is wider than n allows");
    }
    if (s.pages_ > kPageLimit) {
        return fail("page count out of range");
    }
    if (payload_len != bytes.size() - kSnapshotHeaderSize) {
        return fail("payload length " + std::to_string(payload_len) + " but " +
                    std::to_string(bytes.size() - kSnapshotHeaderSize) +
                    " bytes follow the header");
    }
    const std::uint64_t max_body =
        s.pages_ > ~std::uint64_t{0} / kMaxPageBytes ? ~std::uint64_t{0} : s.pages_ * kMaxPageBytes;
    if (std::string e = decompress(bytes.subspan(kSnapshotHeaderSize), max_body, s.body_);
        !e.empty()) {
        return fail(std::move(e));
    }
    if (crc32(s.body_) != static_cast<std::uint32_t>(get_le(bytes, 12, 4))) {
        return fail("CRC mismatch (the file is corrupted)");
    }
    NoSink none;
    if (std::string e =
            walk(s.body_, s.pages_, prog.cells, Memory::fill_pattern_of(prog.cells), none);
        !e.empty()) {
        return fail(std::move(e));
    }
    return R::ok(std::move(s));
}

void Snapshot::restore(core::Vm& vm, std::span<const std::uint8_t> cells) const {
    assert(vm.steps() == 0);
    Memory& mem = vm.memory_for_restore();
    RestoreSink sink(mem);
    [[maybe_unused]] const std::string e = walk(body_, pages_, cells, mem.fill_pattern(), sink);
    assert(e.empty());
    vm.restore(regs_, steps_, n_);
}

std::string Snapshot::compare(const core::Vm& vm, std::span<const std::uint8_t> cells) const {
    if (vm.steps() != steps_) {
        return "the step count is " + std::to_string(vm.steps()) + ", the snapshot's " +
               std::to_string(steps_);
    }
    const auto reg = [](std::string_view name, Value v, Value want) {
        return std::string(name) + " is " + v.to_string() + ", the snapshot has " +
               want.to_string();
    };
    if (vm.a() != regs_.a) {
        return reg("A", vm.a(), regs_.a);
    }
    if (vm.c() != regs_.c) {
        return reg("C", vm.c(), regs_.c);
    }
    if (vm.d() != regs_.d) {
        return reg("D", vm.d(), regs_.d);
    }
    if (vm.n() != n_) {
        return "n is " + std::to_string(vm.n()) + ", the snapshot has " + std::to_string(n_);
    }
    const Memory& mem = vm.memory();
    CompareSink sink(mem, cells);
    [[maybe_unused]] const std::string e = walk(body_, pages_, cells, mem.fill_pattern(), sink);
    assert(e.empty());
    sink.check();
    if (sink.diff.empty() && sink.next != sink.vm_pages.size()) {
        return "page " + std::to_string(sink.vm_pages[sink.next]) +
               " is materialised in only one of them";
    }
    return sink.diff;
}

} // namespace malm::container
