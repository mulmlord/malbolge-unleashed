#include "value.hpp"

namespace malm::core {

std::optional<Value> Value::parse(std::string_view text) noexcept {
    unsigned head = 0;
    if (text.starts_with("0t")) {
        text.remove_prefix(2);
        if (text.empty()) {
            return std::nullopt;
        }
    } else if (text.starts_with("...")) {
        text.remove_prefix(3);
        if (text.empty() || text.front() < '0' || text.front() > '2') {
            return std::nullopt;
        }
        head = static_cast<unsigned>(text.front() - '0');
        text.remove_prefix(1);
    } else {
        return std::nullopt;
    }
    for (const char c : text) {
        if (c < '0' || c > '2') {
            return std::nullopt;
        }
    }
    // Drop leading digits equal to the head; they are part of the repeating head.
    while (!text.empty() && static_cast<unsigned>(text.front() - '0') == head) {
        text.remove_prefix(1);
    }
    if (text.size() > static_cast<std::size_t>(kMaxWidth)) {
        return std::nullopt;
    }
    // raw = H + sum over i of (t_i - h) * 3^i, in wrapping unsigned arithmetic.
    std::uint64_t raw = head * ((pow3[kTrits] - 1) / 2);
    for (std::size_t k = 0; k < text.size(); ++k) {
        const auto i = text.size() - 1 - k;
        const auto d = static_cast<std::int64_t>(text[k] - '0') - static_cast<std::int64_t>(head);
        raw += static_cast<std::uint64_t>(d) * pow3[i];
    }
    return Value::from_raw(raw);
}

std::string Value::to_string() const {
    const unsigned h = head();
    const unsigned w = width();
    std::string out = h == 0 ? "0t" : "..." + std::string(1, static_cast<char>('0' + h));
    if (h == 0 && w == 0) {
        out += '0';
    }
    for (unsigned i = w; i > 0; --i) {
        out += static_cast<char>('0' + trit(i - 1));
    }
    return out;
}

} // namespace malm::core
