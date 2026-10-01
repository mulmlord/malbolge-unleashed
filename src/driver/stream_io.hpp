// core::Io over iostreams: per-byte get/put, with stream state mapped to I/O errors.
#pragma once

#include <istream>
#include <ostream>

#include "core/vm.hpp"

namespace malm {

class StreamIo final : public core::Io {
  public:
    StreamIo(std::istream& in, std::ostream& out) : in_(in), out_(out) {}

    int read_byte() override {
        const std::istream::int_type ch = in_.get();
        if (std::istream::traits_type::eq_int_type(ch, std::istream::traits_type::eof())) {
            return in_.bad() ? core::kIoError : core::kEof;
        }
        return static_cast<int>(static_cast<unsigned char>(ch));
    }

    bool write_byte(std::uint8_t b) override {
        out_.put(static_cast<char>(b));
        return !out_.fail();
    }

    bool flush() override {
        out_.flush();
        return !out_.fail();
    }

  private:
    std::istream& in_;
    std::ostream& out_;
};

} // namespace malm
