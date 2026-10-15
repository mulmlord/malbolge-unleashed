// Sparse unleashed-mode memory (spec 2.1-2.4): pages of 3^8 cells filled lazily from the fill
// pattern P[0..5], plus the memory-size rule n (spec 2.2). The classic flat memory is in
// classic.hpp.
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>

#include "core/value.hpp"

namespace malm::core {

// The register a memory access goes through (spec 5): [C] for fetch and encryption, [D] for the
// operand. Unleashed memory keeps a page cursor per register; classic memory ignores it.
enum class Port : std::uint8_t { C, D };

class Memory {
  public:
    // Preconditions (asserted; the loader enforces them with E_LOAD): 2 <= L <= 3^38 and every
    // cell in 33..126.
    explicit Memory(std::span<const std::uint8_t> program);

    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    // The cache holds raw Page* into pages_, so a move must take it along and clear the source.
    // A moved-from Memory is valid but unspecified (empty cache; it can be assigned to or used):
    // it keeps its fill pattern and shares the fill pages.
    Memory(Memory&& o) noexcept
        : fill_(o.fill_), fill_pages_(o.fill_pages_), n_(o.n_), head0_limit_(o.head0_limit_),
          head2_limit_(o.head2_limit_), pages_(std::move(o.pages_)),
          cache_(std::exchange(o.cache_, {})), cursors_(std::exchange(o.cursors_, {})) {
        o.pages_.clear();
    }
    Memory& operator=(Memory&& o) noexcept {
        if (this != &o) {
            fill_ = o.fill_;
            fill_pages_ = o.fill_pages_;
            n_ = o.n_;
            head0_limit_ = o.head0_limit_;
            head2_limit_ = o.head2_limit_;
            pages_ = std::move(o.pages_);
            o.pages_.clear();
            cache_ = std::exchange(o.cache_, {});
            cursors_ = std::exchange(o.cursors_, {});
        }
        return *this;
    }
    ~Memory() = default;

    // P[r] for r = address mod 6 (spec 2.4).
    const std::array<Value, 6>& fill_pattern() const noexcept { return fill_; }

    // Any raw address is accepted; addressability is not enforced here (spec 2.2).
    Value read(Value addr) const {
        return find_page(addr.raw() / kPageCells)->cells[addr.raw() % kPageCells];
    }

    void write(Value addr, Value v) {
        const std::uint64_t idx = addr.raw() / kPageCells;
        Page* p = find_page(idx);
        if (p == fill_page(idx)) { // never written: other absent pages share it
            p = materialise(idx);
        }
        p->cells[addr.raw() % kPageCells] = v;
    }

    // The same through the port's cursor: while the register stays in one page, an access is one
    // subtraction and comparison, without the page cache. Results are those of read and write.
    Value read(Value addr, Port port) const {
        Cursor& cur = cursors_[static_cast<std::size_t>(port)];
        std::uint64_t off = addr.raw() - cur.base;
        if (off >= kPageCells) [[unlikely]] {
            off = seek(cur, addr);
        }
        return cur.page->cells[off];
    }

    void write(Value addr, Value v, Port port) {
        Cursor& cur = cursors_[static_cast<std::size_t>(port)];
        std::uint64_t off = addr.raw() - cur.base;
        if (off >= kPageCells) [[unlikely]] {
            off = seek(cur, addr);
        }
        if (cur.fill) [[unlikely]] {
            materialise(cur.base / kPageCells); // updates cur
        }
        cur.page->cells[off] = v;
    }

    // Number of materialised pages (tests and stats).
    std::size_t page_count() const noexcept { return pages_.size(); }

    unsigned n() const noexcept { return n_; }

    // n' = max(n, width(c)+1, width(d)+1). Returns false and leaves n unchanged if n' > 39 (the
    // VM maps that to E_WIDTH). n never decreases.
    [[nodiscard]] bool grow_for(Value c, Value d) noexcept {
        if (fits(c) && fits(d)) {
            return true;
        }
        return grow_slow(c, d);
    }

  private:
    // width(v) <= n - 1. Head 0 and head 2 values are decided by one comparison each: their trits
    // from n - 1 up are all 0 (raw < 3^(n-1)) or all 2 (raw >= 3^40 - 3^(n-1)).
    bool fits(Value v) const noexcept {
        return v.raw() < head0_limit_ || v.raw() >= head2_limit_ || v.fits_width(n_ - 1);
    }
    bool grow_slow(Value c, Value d) noexcept;
    void set_n(unsigned n) noexcept {
        n_ = n;
        head0_limit_ = pow3[n - 1];
        head2_limit_ = pow3[kTrits] - pow3[n - 1];
    }

    // 3^8 divides 3^39 and 3^40, so a page never straddles a head change or the ...222 -> 0 wrap.
    static constexpr std::uint64_t kPageCells = 6561;

    // 8 bytes per cell: programs of hundreds of MB of cells need GBs of RAM.
    struct Page {
        std::array<Value, kPageCells> cells;
    };

    // A page that is not materialised reads as the fill pattern from its first address on. The
    // pattern index of an address is raw() % 6 (Value::mod6() == raw() % 6 for every value: the
    // remainder rule's offset for heads 1 and 2 is a multiple of 6), and 3^8 = 3 (mod 6), so pages
    // start in one of two phases: fill_pages_[idx % 2] starts at P[3 * (idx % 2)].
    static_assert(kPageCells % 6 == 3);
    Page* fill_page(std::uint64_t idx) const noexcept { return fill_pages_[idx % 2].get(); }

    struct CacheEntry {
        std::uint64_t idx = ~std::uint64_t{0}; // no page has this index (max is 3^40 / 3^8)
        Page* page = nullptr;                  // the page, or its fill page if not materialised
    };

    // Direct-mapped page cache behind the cursors and the plain read and write: a hit is one
    // comparison. Absent pages are cached too (as their fill page), so repeated reads there skip
    // the hash lookup; materialise() updates the slot.
    static constexpr std::size_t kCacheSlots = 64;
    Page* find_page(std::uint64_t idx) const {
        const CacheEntry& e = cache_[idx % kCacheSlots];
        if (e.idx == idx) {
            return e.page;
        }
        return find_page_slow(idx);
    }

    Page* find_page_slow(std::uint64_t idx) const;

    // A port's page: cells base .. base + 3^8 - 1 are page->cells. `fill` marks a shared fill page
    // (the page is not materialised), which must not be written; materialise() updates it.
    struct Cursor {
        std::uint64_t base = std::uint64_t{1} << 63; // no address is within 3^8 of it
        Page* page = nullptr;
        bool fill = false;
    };

    // Points `cur` at the page of `addr`; returns the offset of addr in it.
    std::uint64_t seek(Cursor& cur, Value addr) const;

    Page* materialise(std::uint64_t idx);

    std::array<Value, 6> fill_{};
    std::array<std::shared_ptr<Page>, 2> fill_pages_;
    unsigned n_ = 10;
    std::uint64_t head0_limit_ = pow3[9];                // 3^(n-1), see fits()
    std::uint64_t head2_limit_ = pow3[kTrits] - pow3[9]; // 3^40 - 3^(n-1)
    std::unordered_map<std::uint64_t, std::unique_ptr<Page>> pages_;
    mutable std::array<CacheEntry, kCacheSlots> cache_{};
    mutable std::array<Cursor, 2> cursors_{}; // indexed by Port
};

} // namespace malm::core
