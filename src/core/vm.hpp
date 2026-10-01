// The unleashed-mode virtual machine: step loop, I/O interface and errors (spec 5-7).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/memory.hpp"
#include "core/value.hpp"

namespace malm::core {

inline constexpr int kEof = -1;
inline constexpr int kIoError = -2;

// Byte-level I/O seen by the VM. The VM makes one virtual call per I/O operation.
struct Io {
    virtual ~Io();
    // Next input byte 0..255, kEof or kIoError.
    virtual int read_byte() = 0;
    // False on a write error (including EPIPE).
    virtual bool write_byte(std::uint8_t b) = 0;
    // False on a flush error.
    virtual bool flush() = 0;
};

enum class Error : std::uint8_t { Exec, Encrypt, Width, Io };

// "E_EXEC", "E_ENCRYPT", "E_WIDTH", "E_IO".
std::string_view error_name(Error e) noexcept;

// Diagnostic snapshot (spec 7): the state as it is when the error is detected. E_EXEC: the
// registers before the step. E_WIDTH from the growth rule: n before the failed growth. E_WIDTH
// from the step-5 increment: the registers before the increment, with `overflow` set.
struct Fault {
    Error error;
    Value c;
    Value d;
    Value a;
    unsigned n;
    std::uint64_t steps;
    bool overflow = false;
};

// e.g. "E_WIDTH at step 4258: C=... D=... A=... n=39 (overflow)".
std::string format_fault(const Fault& f);

enum class Status : std::uint8_t { Continue, Halted, Faulted };

struct StepResult {
    Status status;
    std::optional<Fault> fault; // set iff status == Faulted
};

struct RunResult {
    enum class Kind : std::uint8_t { Halted, Fault, StepLimit };
    Kind kind;
    std::optional<Fault> fault; // set iff kind == Fault
};

// Register file; used by Vm::set_registers.
struct Registers {
    Value a;
    Value c;
    Value d;
};

class Vm {
  public:
    // Preconditions: those of Memory (cells from load_text).
    explicit Vm(std::span<const std::uint8_t> cells) : mem_(cells) {}

    Value a() const noexcept { return a_; }
    Value c() const noexcept { return c_; }
    Value d() const noexcept { return d_; }
    unsigned n() const noexcept { return mem_.n(); }
    std::uint64_t steps() const noexcept { return steps_; }
    bool halted() const noexcept { return halted_; }
    const Memory& memory() const noexcept { return mem_; }

    // For tests and tools only: overwrites A, C and D. It does not re-establish the invariants
    // of spec 2.2 (width of C and D below n), so the caller owns the consequences.
    void set_registers(const Registers& r) noexcept {
        a_ = r.a;
        c_ = r.c;
        d_ = r.d;
    }

    // One step (spec 5). On Halted or Faulted the VM flushes `io`; a flush failure on halt turns
    // the halt into an E_IO fault. A stopped VM keeps returning its final result.
    StepResult step(Io& io);

    // Steps until halt or fault, or until `max_steps` steps have been executed in total (the
    // counter `steps()` is compared, not a per-call budget). Flushes `io` when it returns.
    RunResult run(Io& io, std::optional<std::uint64_t> max_steps = {});

  private:
    // The inner loop: no flush, no result objects. Faults are recorded in fault_.
    Status step_inner(Io& io);
    Status fail(Error e, Value a, Value c, Value d, bool overflow = false) noexcept;
    // Flushes after the loop stopped with `s`; returns the final status.
    Status finish(Io& io, Status s);

    Memory mem_;
    Value a_;
    Value c_;
    Value d_;
    std::uint64_t steps_ = 0;
    bool halted_ = false;
    bool eof_ = false;
    std::optional<Fault> fault_;
};

// Process exit status: a halt exits 0, or A mod 256 when `exit_a` is set; a fault exits 1. A
// step-limit stop has no spec status; it reports 1.
int exit_status(const RunResult& r, const Vm& vm, bool exit_a) noexcept;

} // namespace malm::core
