// SHA-256 (FIPS 180-4): the key that ties a startup snapshot to its program file (snapshot.hpp).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace malm::container {

using Sha256 = std::array<std::uint8_t, 32>;

Sha256 sha256(std::span<const std::uint8_t> data) noexcept;

// Lower-case hex, 64 digits.
std::string to_hex(const Sha256& h);

} // namespace malm::container
