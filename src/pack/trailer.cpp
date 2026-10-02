#include "pack/trailer.hpp"

#include <algorithm>

namespace malm::pack {
namespace {

void put_u64(std::uint8_t* p, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

std::uint64_t get_u64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

} // namespace

std::array<std::uint8_t, kTrailerSize> make_trailer(Trailer t) noexcept {
    std::array<std::uint8_t, kTrailerSize> out{};
    std::copy(kTrailerMagic.begin(), kTrailerMagic.end(), out.begin());
    put_u64(out.data() + 8, t.offset);
    put_u64(out.data() + 16, t.length);
    return out;
}

core::Result<std::optional<Trailer>, std::string> parse_trailer(std::span<const std::uint8_t> tail,
                                                                std::uint64_t file_size) {
    using R = core::Result<std::optional<Trailer>, std::string>;
    if (tail.size() != kTrailerSize ||
        !std::equal(kTrailerMagic.begin(), kTrailerMagic.end(), tail.begin())) {
        return R::ok(std::nullopt);
    }
    const Trailer t{get_u64(tail.data() + 8), get_u64(tail.data() + 16)};
    if (t.offset == 0) {
        return R::err("payload offset is 0");
    }
    // offset + length + 24 == file_size, without overflow.
    if (file_size < kTrailerSize || t.offset > file_size - kTrailerSize ||
        t.length != file_size - kTrailerSize - t.offset) {
        return R::err("payload offset and length do not match the file size");
    }
    return R::ok(t);
}

} // namespace malm::pack
