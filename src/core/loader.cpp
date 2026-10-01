#include "core/loader.hpp"

#include "core/tables.hpp"

namespace malm::core {

LoadResult load_text(std::span<const std::uint8_t> bytes, std::size_t max_cells) {
    std::vector<std::uint8_t> cells;
    cells.reserve(bytes.size());
    std::uint32_t pos_mod94 = 0; // stored cells so far, mod 94
    for (std::size_t off = 0; off < bytes.size(); ++off) {
        const std::uint8_t x = bytes[off];
        if (x == ' ' || (x >= '\t' && x <= '\r')) { // space, \t \n \v \f \r
            continue;
        }
        if (x < 33 || x > 126) {
            return LoadResult::err(
                LoadError{off, "byte " + std::to_string(x) + " is outside 33..126"});
        }
        if (!is_loadable(pos_mod94, x)) {
            return LoadResult::err(LoadError{off, "byte " + std::to_string(x) +
                                                      " is not a valid instruction at cell " +
                                                      std::to_string(cells.size())});
        }
        if (cells.size() == max_cells) {
            return LoadResult::err(
                LoadError{off, "program is longer than " + std::to_string(max_cells) + " cells"});
        }
        cells.push_back(x);
        pos_mod94 = pos_mod94 == 93 ? 0 : pos_mod94 + 1;
    }
    if (cells.size() < 2) {
        return LoadResult::err(LoadError{bytes.size(), "program has fewer than 2 cells"});
    }
    return LoadResult::ok(std::move(cells));
}

} // namespace malm::core
