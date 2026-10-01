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

class Memory {
  public:
    // Preconditions (asserted; the loader enforces them with E_LOAD): 2 <= L <= 3^38 and every
    // cell in 33..126.
    explicit Memory(std::span<const std::uint8_t> program);

    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    // The cache holds raw Page* into pages_, so a move must take it along and clear the source.
    // A moved-from Memory is valid but unspecified (empty cache; it can be assigned to or used).
    Memory(Memory&& o) noexcept
        : fill_(o.fill_), n_(o.n_), pages_(std::move(o.pages_)),
          cache_(std::exchange(o.cache_, {})) {
        o.pages_.clear();
    }
    Memory& operator=(Memory&& o) noexcept {
        if (this != &o) {
            fill_ = o.fill_;
            n_ = o.n_;
            pages_ = std::move(o.pages_);
            o.pages_.clear();
            cache_ = std::exchange(o.cache_, {});
        }
        return *this;
    }
    ~Memory() = default;

    // P[r] for r = address mod 6 (spec 2.4).
    const std::array<Value, 6>& fill_pattern() const noexcept { return fill_; }

    // Any raw address is accepted; addressability is not enforced here (spec 2.2).
    Value read(Value addr) const {
        const Page* p = find_page(addr.raw() / kPageCells);
        if (p == nullptr) {
            // Note for #37: mod6() == raw() % 6 for every Value (proved), a cheaper lookup.
            return fill_[addr.mod6()];
        }
        return p->cells[addr.raw() % kPageCells];
    }

    void write(Value addr, Value v) {
        const std::uint64_t idx = addr.raw() / kPageCells;
        Page* p = find_page(idx);
        if (p == nullptr) {
            p = materialise(idx);
        }
        p->cells[addr.raw() % kPageCells] = v;
    }

    // Number of materialised pages (tests and stats).
    std::size_t page_count() const noexcept { return pages_.size(); }

    unsigned n() const noexcept { return n_; }

    // n' = max(n, width(c)+1, width(d)+1). Returns false and leaves n unchanged if n' > 39 (the
    // VM maps that to E_WIDTH). n never decreases.
    [[nodiscard]] bool grow_for(Value c, Value d) noexcept {
        if (c.fits_width(n_ - 1) && d.fits_width(n_ - 1)) {
            return true;
        }
        const unsigned grown = std::max({n_, c.width() + 1, d.width() + 1});
        if (grown > static_cast<unsigned>(kMaxWidth)) {
            return false;
        }
        n_ = grown;
        return true;
    }

  private:
    // 3^8 divides 3^39 and 3^40, so a page never straddles a head change or the ...222 -> 0 wrap.
    static constexpr std::uint64_t kPageCells = 6561;

    // 8 bytes per cell: programs of hundreds of MB of cells need GBs of RAM. Compaction is the
    // M6 perf ticket (#37).
    struct Page {
        std::array<Value, kPageCells> cells;
    };

    struct CacheEntry {
        std::uint64_t idx = ~std::uint64_t{0}; // no page has this index (max is 3^40 / 3^8)
        Page* page = nullptr;                  // null caches "absent"
    };

    // MRU cache of the last two pages (C and D usually hit different ones). Absent pages are
    // cached too, so repeated reads there skip the hash lookup; materialise() updates them.
    Page* find_page(std::uint64_t idx) const {
        if (cache_[0].idx == idx) {
            return cache_[0].page;
        }
        if (cache_[1].idx == idx) {
            std::swap(cache_[0], cache_[1]);
            return cache_[0].page;
        }
        return find_page_slow(idx);
    }

    Page* find_page_slow(std::uint64_t idx) const;
    Page* materialise(std::uint64_t idx);

    std::array<Value, 6> fill_{};
    unsigned n_ = 10;
    std::unordered_map<std::uint64_t, std::unique_ptr<Page>> pages_;
    mutable std::array<CacheEntry, 2> cache_{};
};

} // namespace malm::core
