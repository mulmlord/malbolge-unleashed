#include "core/memory.hpp"

#include <algorithm>

#include "core/ops.hpp"

namespace malm::core {

Memory::Memory(std::span<const std::uint8_t> program) {
    const std::size_t len = program.size();
    assert(len >= 2 && len <= pow3[38]);

    const auto cell = [&](std::size_t i) {
        assert(program[i] >= 33 && program[i] <= 126);
        return *Value::natural(program[i]);
    };

    // Fill pattern (spec 2.4): f(i) = crz(f(i-1), f(i-2)) for i >= L, P[i mod 6] = f(i).
    Value prev2 = cell(len - 2);
    Value prev1 = cell(len - 1);
    for (std::size_t i = len; i < len + 6; ++i) {
        const Value f = crz(prev1, prev2);
        fill_[i % 6] = f;
        prev2 = prev1;
        prev1 = f;
    }

    // Initial n = max(10, width(L-1) + 1).
    n_ = std::max(10U, Value::natural(len - 1)->width() + 1);
    assert(n_ <= static_cast<unsigned>(kMaxWidth));

    for (std::size_t i = 0; i < len; ++i) {
        Page* p = find_page(i / kPageCells);
        if (p == nullptr) {
            p = materialise(i / kPageCells);
        }
        p->cells[i % kPageCells] = cell(i);
    }
}

Memory::Page* Memory::find_page_slow(std::uint64_t idx) const {
    const auto it = pages_.find(idx);
    Page* page = it == pages_.end() ? nullptr : it->second.get();
    cache_[1] = cache_[0];
    cache_[0] = {idx, page};
    return page;
}

Memory::Page* Memory::materialise(std::uint64_t idx) {
    auto page = std::make_unique<Page>();
    // The remainder rule is continuous under +1 within a head, and pages never cross a head
    // change or the wrap, so the pattern index only needs the page's first address.
    unsigned r = Value::from_raw(idx * kPageCells).mod6();
    for (Value& c : page->cells) {
        c = fill_[r];
        r = r == 5 ? 0 : r + 1;
    }
    Page* raw = page.get();
    pages_[idx] = std::move(page);
    for (CacheEntry& e : cache_) {
        if (e.idx == idx) {
            e.page = raw;
        }
    }
    return raw;
}

} // namespace malm::core
