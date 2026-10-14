// The semantics of RV32IM guest instructions in HLA (docs/rv.md 8): the "macros" that rv-emu
// (emu.hpp) expands for its decoded operands and rv-aot (aot/aot.hpp) for each instruction of a
// program. HLA has no macros of its own, so they are C++ functions that write HLA statements.
//
// A guest register is an HLA register: x1 .. x31 (rv-emu reads x0 as the constant 0). HLA's
// arithmetic is RV32IM's, so an ALU instruction is one statement. Memory accesses, the system
// calls and the faults are subroutines of a shared runtime (runtime()), entered by a jump with
// the return address in a link register (`mov lk, L` and `jmp lk` cost 60 steps, HLA's call and
// ret 330). Their interface registers:
//   ea   the byte address of a load or store
//   b    the value of a store
//   r    the result of a load
//   lkm  the return address of a memory subroutine, lke of the system call
// The subroutines use the scratch registers t, u, n, wa, w0, w1, hv, lkd and lkn and leave every
// other register alone.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rv/elf.hpp"

namespace malm::rv::sem {

// HLA source text under construction.
class Writer {
  public:
    void stmt(std::string_view s);
    void label(std::string_view name);
    void comment(std::string_view text);
    void blank();
    // `switch reg, labels...` (HLA looks at the low byte of reg).
    void switch_on(std::string_view reg, const std::vector<std::string>& labels);
    // A label that no other call returned, `prefix` followed by a number.
    std::string fresh(std::string_view prefix);
    // `mov link, ret; jmp target`, then the label ret.
    void call(std::string_view link, std::string_view target, std::string_view ret);
    const std::string& text() const noexcept { return text_; }

  private:
    std::string text_;
    unsigned next_ = 0;
};

// Why a guest program stopped: the value of the register `stop` (0 while it runs).
enum class StopCode : std::uint32_t {
    Running = 0,
    Illegal = 1, // illegal instruction (insn)
    Ebreak = 2,
    MisalignedJump = 3, // a taken jump or branch to tg, which is not 4-aligned
    LoadFault = 4,      // a load from ea not entirely inside guest RAM
    StoreFault = 5,     // a store to ea not entirely inside guest RAM
    FetchFault = 6,     // pc is outside guest RAM
    Exit = 7,           // exit(a0)
    Untranslated = 8,   // rv-aot: pc is inside guest RAM but not the start of translated code
};

// The labels of the runtime's stops; each sets `stop` and halts with status 1 (an exit: a0 & 255).
// pc names the faulting instruction, as in the reference emulator (docs/rv.md 5).
inline constexpr std::string_view kFaultIllegal = "f_ill";
inline constexpr std::string_view kFaultEbreak = "f_ebreak";
inline constexpr std::string_view kFaultMisaligned = "f_misaligned";
inline constexpr std::string_view kFaultLoad = "f_load";
inline constexpr std::string_view kFaultStore = "f_store";
inline constexpr std::string_view kFaultFetch = "f_fetch";

struct Config {
    std::uint32_t ram_size = 0; // guest RAM [0, ram_size), a multiple of 16
    // A test harness: a stop sets `stop` and continues at the label `fetch` instead of halting, so
    // one VM can run case after case (tests/rv/emu_test.cpp).
    bool harness = false;
};

// `ram mem[ram_size / 4]`: guest RAM at HLA address 0, with the segments of `image` (if any) as
// little-endian initial words.
void declare_ram(Writer& w, std::uint32_t ram_size, const Image* image);

// The runtime's registers: `reg` declarations of the interface and scratch registers above, the
// program break (brk, brkmin) and stop.
void declare_runtime(Writer& w, std::uint32_t brk);

// Continues if `reg` is a byte address inside guest RAM, else jumps to `fault`. Uses t.
void check_in_ram(Writer& w, const Config& c, std::string_view reg, std::string_view fault);

enum class Alu : std::uint8_t {
    Add,
    Sub,
    Sll,
    Slt,
    Sltu,
    Xor,
    Srl,
    Sra,
    Or,
    And,
    Mul,
    Mulh,
    Mulhsu,
    Mulhu,
    Div,
    Divu,
    Rem,
    Remu
};

// d := x op y; y is a register or a number (the shifts use its low 5 bits).
void alu(Writer& w, Alu op, std::string_view d, std::string_view x, std::string_view y);

enum class Cond : std::uint8_t { Eq, Ne, Lt, Ge, Ltu, Geu };

// Continues at `taken` if `x cond y` (registers), else with the next statement. Uses t.
void branch(Writer& w, Cond c, std::string_view x, std::string_view y, std::string_view taken);

enum class Width : std::uint8_t { Byte = 1, Half = 2, Word = 4 };

// r := the `width` bytes at ea (little-endian, any alignment), sign-extended if `sign`; a load
// outside guest RAM stops with LoadFault. Continues at `ret`. Without `checked` the caller knows
// that the bytes are inside guest RAM: no check, so it cannot stop.
void load(Writer& w, Width width, bool sign, std::string_view ret, bool checked = true);
// The `width` low bytes of b go to ea; outside guest RAM: StoreFault. Continues at `ret`.
void store(Writer& w, Width width, std::string_view ret, bool checked = true);
// The system call of a7 (docs/rv.md 4): a0 := the result, or the program exits; the register
// dump (255) writes pc and x1 .. x31 to stdout. Continues at `ret`.
void ecall(Writer& w, std::string_view ret);

// The subroutines and stops: ld_b, ld_h, ld_w, st_b, st_h, st_w, ecall and the fault labels.
void runtime(Writer& w, const Config& c);

} // namespace malm::rv::sem
