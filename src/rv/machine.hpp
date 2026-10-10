// The C++ reference emulator for RV32IM (docs/rv.md): a plain interpreter, written to be obviously
// correct rather than fast. It is the test oracle for rv-emu and rv-aot, and runs guest programs
// on the host's stdin and stdout through core::Io.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/vm.hpp"
#include "rv/elf.hpp"

namespace malm::rv {

// ecall numbers (a7), as in Linux RV32.
inline constexpr std::uint32_t kSysRead = 63;
inline constexpr std::uint32_t kSysWrite = 64;
inline constexpr std::uint32_t kSysExit = 93;
inline constexpr std::uint32_t kSysBrk = 214;

// Negated Linux errno values returned in a0.
inline constexpr std::uint32_t kEbadf = static_cast<std::uint32_t>(-9);
inline constexpr std::uint32_t kEfault = static_cast<std::uint32_t>(-14);
inline constexpr std::uint32_t kEnosys = static_cast<std::uint32_t>(-38);

// How a step ended the run. Fault: the program did something the emulator rejects, or host I/O
// failed (E_IO); pc still names the offending instruction and no register changed. Exit: pc names
// the ecall.
struct Stop {
    enum class Kind : std::uint8_t { Exit, Fault };
    Kind kind = Kind::Exit;
    int exit_code = 0;   // Exit: a0 & 255
    std::string message; // Fault: one-line reason, e.g. "illegal instruction 0x00000000"
};

class Machine {
  public:
    // Guest RAM [0, ram_size) zero-filled, then the image's segments copied in; pc = entry,
    // sp = ram_size, every other register 0. Precondition: `image` came from load_elf with the
    // same ram_size.
    Machine(const Image& image, std::uint32_t ram_size);

    // Executes one instruction. Returns a Stop when the program exits or faults.
    std::optional<Stop> step(core::Io& io);
    // Steps until the program exits or faults; flushes the output either way.
    Stop run(core::Io& io);

    std::uint32_t pc() const noexcept { return pc_; }
    std::uint32_t reg(unsigned i) const { return x_.at(i); }
    const std::array<std::uint32_t, 32>& regs() const noexcept { return x_; }
    std::uint32_t brk() const noexcept { return brk_; }
    std::uint64_t steps() const noexcept { return steps_; }
    std::span<const std::uint8_t> ram() const noexcept { return ram_; }
    // The 32-bit little-endian word at addr, or nullopt if it is not inside guest RAM.
    std::optional<std::uint32_t> peek32(std::uint32_t addr) const;

    // For tests: overwrite a register (x0 stays 0), like the state after some instructions.
    void set_reg(unsigned i, std::uint32_t value) { set(i, value); }

  private:
    bool in_ram(std::uint32_t addr, std::uint32_t size) const noexcept;
    std::uint32_t load(std::uint32_t addr, unsigned size) const;
    void store(std::uint32_t addr, unsigned size, std::uint32_t value);
    void set(unsigned rd, std::uint32_t value) noexcept;
    std::optional<Stop> ecall(core::Io& io);

    std::vector<std::uint8_t> ram_;
    std::array<std::uint32_t, 32> x_{};
    std::uint32_t pc_ = 0;
    std::uint32_t brk_ = 0;
    std::uint32_t brk_min_ = 0;
    std::uint64_t steps_ = 0;
};

} // namespace malm::rv
