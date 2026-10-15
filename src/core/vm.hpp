// The virtual machine: step loop, I/O interface and errors (spec 5-7). One step loop serves both
// modes: BasicVm<M> is parameterised by a machine policy M (word type, memory, operations; see
// machine.hpp for unleashed mode and classic.hpp for classic mode, spec 8).
#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/classic.hpp"
#include "core/machine.hpp"
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

// "Jmp", "Out", "In", "Rot", "MovD", "Opr", "Nop", "Hlt".
std::string_view op_name(Op op) noexcept;

// What an observer sees of a step before it executes: the registers and n as they are at fetch
// time, [C] and [D] being the cells they point to. A step that fails the fetch check (E_EXEC) has
// no decoded op: `decoded` is false, `op` is meaningless and `cell` is the offending value. For a
// decoded step, `cell` is the cell code 33..126.
struct StepInfo {
    std::uint64_t step; // 1-based, as in Fault::steps
    Value c;
    Value cell; // [C]
    bool decoded;
    Op op;
    Value d;
    Value dval; // [D]
    Value a;
    unsigned n;
};

// Observer interface for run(io, obs, ...). One virtual call per hook and step.
struct StepObserver {
    virtual ~StepObserver();
    // Before the step executes (also for a step that then faults, including E_EXEC).
    virtual void before(const StepInfo& s) = 0;
};

struct StepResult {
    Status status;
    std::optional<Fault> fault; // set iff status == Faulted
};

struct RunResult {
    // BeforeIo: run_pure stopped before an In, Out or Hlt step.
    enum class Kind : std::uint8_t { Halted, Fault, StepLimit, BeforeIo };
    Kind kind;
    std::optional<Fault> fault; // set iff kind == Fault
};

// Register file; used by BasicVm::set_registers.
template <class W> struct BasicRegisters {
    W a;
    W c;
    W d;
};
using Registers = BasicRegisters<Value>;
using ClassicRegisters = BasicRegisters<ClassicWord>;

template <class M> class BasicVm {
  public:
    using Word = typename M::Word;

    // Preconditions: those of the machine's memory (cells from load_text).
    explicit BasicVm(std::span<const std::uint8_t> cells) : m_(cells) {}

    Word a() const noexcept { return a_; }
    Word c() const noexcept { return c_; }
    Word d() const noexcept { return d_; }
    unsigned n() const noexcept { return m_.n(); }
    std::uint64_t steps() const noexcept { return steps_; }
    bool halted() const noexcept { return halted_; }
    const auto& memory() const noexcept { return m_.memory(); }

    // For tests and tools only: overwrites A, C and D. Classic words are reduced mod 59049. It does
    // not re-establish the invariants of spec 2.2 (width of C and D below n), so the caller owns
    // the consequences.
    void set_registers(const BasicRegisters<Word>& r) noexcept {
        a_ = M::normalize(r.a);
        c_ = M::normalize(r.c);
        d_ = M::normalize(r.d);
        c94_ = M::mod94(c_);
    }

    // For tests and tools only: writes one memory cell directly, like set_registers without the
    // invariants of spec 2.2 (cell width below n) or of the loader (cells in 33..126).
    void poke(Word addr, Word v) { m_.write(M::normalize(addr), M::normalize(v)); }

    // For startup snapshots (container/snapshot.hpp), unleashed mode only: the memory a snapshot
    // writes its pages into, and the registers, step count and n it restores. Precondition: a VM
    // that has not run; restore() leaves it as the run_pure that made the snapshot did (not halted,
    // no fault, stdin not at EOF). Like set_registers, it does not check the invariants of spec 2.2
    // (the snapshot reader does).
    Memory& memory_for_restore() noexcept
        requires std::same_as<M, UnleashedMachine>
    {
        return m_.memory();
    }
    void restore(const BasicRegisters<Word>& r, std::uint64_t steps, unsigned n) noexcept
        requires std::same_as<M, UnleashedMachine>
    {
        set_registers(r);
        steps_ = steps;
        m_.memory().restore_n(n);
    }

    // One step (spec 5). On Halted or Faulted the VM flushes `io`; a flush failure on halt turns
    // the halt into an E_IO fault. A stopped VM keeps returning its final result.
    StepResult step(Io& io);

    // Steps until halt or fault, or until `max_steps` steps have been executed in total (the
    // counter `steps()` is compared, not a per-call budget). Flushes `io` when it returns.
    RunResult run(Io& io, std::optional<std::uint64_t> max_steps = {});
    // The same loop (one shared implementation), reporting every step to `obs`.
    RunResult run(Io& io, StepObserver& obs, std::optional<std::uint64_t> max_steps = {});

    // The steps that depend on the program alone: runs as run() does but stops before the first
    // step that would execute In, Out or Hlt (BeforeIo), which is then the next step. Until then
    // the program has read and written nothing, so the state does not depend on its input. Also
    // stops at a fault and at `max_steps`; never halts.
    RunResult run_pure(std::optional<std::uint64_t> max_steps = {});

  private:
    // The registers and step counter as the step loop works on them: a local copy that the
    // compiler keeps in machine registers (members could alias the memory cells the loop writes),
    // stored back to the members when the loop stops.
    struct Regs {
        Word a;
        Word c;
        Word d;
        std::uint32_t c94; // M::mod94(c), kept up to date instead of recomputed every step
        std::uint64_t steps;
    };
    Regs load_regs() const noexcept { return {a_, c_, d_, c94_, steps_}; }
    void store_regs(const Regs& r) noexcept {
        a_ = r.a;
        c_ = r.c;
        d_ = r.d;
        c94_ = r.c94;
        steps_ = r.steps;
    }

    // The inner loop: no flush, no result objects. Faults are recorded in fault_. The only
    // definition of the step order (spec 5). Always inlined into its loop, which keeps `r` in
    // registers.
    // With no observer (the plain path) the hooks are compiled out; with one, every hook runs
    // before the step executes.
    template <class... Obs> [[gnu::always_inline]] Status step_inner(Regs& r, Io& io, Obs&... obs);
    template <class... Obs>
    RunResult run_impl(Io& io, std::optional<std::uint64_t> max_steps, Obs&... obs);
    Status fail(Error e, const Regs& r, bool overflow = false) noexcept;
    // Flushes after the loop stopped with `s`; returns the final status.
    Status finish(Io& io, Status s);

    M m_;
    Word a_{};
    Word c_{};
    Word d_{};
    std::uint32_t c94_ = 0; // M::mod94(c_)
    std::uint64_t steps_ = 0;
    bool halted_ = false;
    bool eof_ = false;
    std::optional<Fault> fault_;
};

// The step loop is defined once in vm.cpp and instantiated for both machines.
extern template class BasicVm<UnleashedMachine>;
extern template class BasicVm<ClassicMachine>;

using Vm = BasicVm<UnleashedMachine>;
using ClassicVm = BasicVm<ClassicMachine>;

// Process exit status: a halt exits 0, or A mod 256 when `exit_a` is set; a fault exits 1. A
// step-limit stop has no spec status; it reports 1.
template <class M> int exit_status(const RunResult& r, const BasicVm<M>& vm, bool exit_a) noexcept {
    if (r.kind == RunResult::Kind::Halted) {
        return exit_a ? static_cast<int>(M::mod256(vm.a())) : 0;
    }
    return 1;
}

} // namespace malm::core
