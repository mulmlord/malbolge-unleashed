// Minimal value-or-error type. std::expected is not used: libstdc++ 14's <expected> does not
// compile with clang-18.
#pragma once

#include <utility>
#include <variant>

namespace malm::core {

template <class T, class E> class Result {
  public:
    static Result ok(T v) { return Result(std::in_place_index<0>, std::move(v)); }
    static Result err(E e) { return Result(std::in_place_index<1>, std::move(e)); }

    bool has_value() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & { return std::get<0>(v_); }
    const T& value() const& { return std::get<0>(v_); }
    T&& value() && { return std::get<0>(std::move(v_)); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }

    E& error() & { return std::get<1>(v_); }
    const E& error() const& { return std::get<1>(v_); }

  private:
    template <std::size_t I, class U>
    Result(std::in_place_index_t<I> i, U&& u) : v_(i, std::forward<U>(u)) {}

    std::variant<T, E> v_;
};

} // namespace malm::core
