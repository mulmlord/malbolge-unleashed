#include "core/memory.hpp"

#include <algorithm>

#include "core/ops.hpp"

namespace malm::core {

namespace {

Value program_cell(std::span<const std::uint8_t> program, std::size_t i) {
    assert(program[i] >= 33 && program[i] <= 126);
    return *Value::natural(program[i]);
}

} // namespace

std::array<Value, 6> Memory::fill_pattern_of(std::span<const std::uint8_t> program) {
    const std::size_t len = program.size();
    assert(len >= 2);
    // Fill pattern (spec 2.4): f(i) = crz(f(i-1), f(i-2)) for i >= L, P[i mod 6] = f(i).
    std::array<Value, 6> fill{};
    Value prev2 = program_cell(program, len - 2);
    Value prev1 = program_cell(program, len - 1);
    for (std::size_t i = len; i < len + 6; ++i) {
        const Value f = crz(prev1, prev2);
        fill[i % 6] = f;
        prev2 = prev1;
        prev1 = f;
    }
    return fill;
}

Memory::Memory(std::span<const std::uint8_t> program) : fill_(fill_pattern_of(program)) {
    const std::size_t len = program.size();
    assert(len >= 2 && len <= pow3[38]);
    const auto cell = [&](std::size_t i) { return program_cell(program, i); };

    for (std::uint64_t phase = 0; phase < 2; ++phase) {
        auto page = std::make_shared<Page>();
        for (std::size_t i = 0; i < kPageCells; ++i) {
            page->cells[i] = fill_[(3 * phase + i) % 6];
        }
        fill_pages_[phase] = std::move(page);
    }

    // Initial n = max(10, width(L-1) + 1).
    set_n(std::max(10U, Value::natural(len - 1)->width() + 1));
    assert(n_ <= static_cast<unsigned>(kMaxWidth));

    for (std::size_t i = 0; i < len; i += kPageCells) {
        Page* p = materialise(i / kPageCells);
        const std::size_t end = std::min(len, i + kPageCells);
        for (std::size_t j = i; j < end; ++j) {
            p->cells[j - i] = cell(j);
        }
    }
}

bool Memory::grow_slow(Value c, Value d) noexcept {
    const unsigned grown = std::max({n_, c.width() + 1, d.width() + 1});
    if (grown > static_cast<unsigned>(kMaxWidth)) {
        return false;
    }
    set_n(grown);
    return true;
}

Memory::Page* Memory::find_page_slow(std::uint64_t idx) const {
    const auto it = pages_.find(idx);
    Page* page = it == pages_.end() ? fill_page(idx) : it->second.get();
    cache_[idx % kCacheSlots] = {idx, page};
    return page;
}

std::uint64_t Memory::seek(Cursor& cur, Value addr) const {
    const std::uint64_t idx = addr.raw() / kPageCells;
    cur.base = idx * kPageCells;
    cur.page = find_page(idx);
    cur.fill = cur.page == fill_page(idx);
    return addr.raw() - cur.base;
}

std::vector<std::uint64_t> Memory::page_indices() const {
    std::vector<std::uint64_t> idx;
    idx.reserve(pages_.size());
    for (const auto& [i, page] : pages_) {
        idx.push_back(i);
    }
    std::ranges::sort(idx);
    return idx;
}

std::span<Value, Memory::kPageCells> Memory::page_for_write(std::uint64_t idx) {
    Page* p = find_page(idx);
    if (p == fill_page(idx)) {
        p = materialise(idx);
    }
    return p->cells;
}

Memory::Page* Memory::materialise(std::uint64_t idx) {
    auto page = std::make_unique<Page>(*fill_page(idx));
    Page* raw = page.get();
    pages_[idx] = std::move(page);
    CacheEntry& e = cache_[idx % kCacheSlots];
    if (e.idx == idx) {
        e.page = raw;
    }
    for (Cursor& cur : cursors_) {
        if (cur.base == idx * kPageCells) {
            cur.page = raw;
            cur.fill = false;
        }
    }
    return raw;
}

} // namespace malm::core
