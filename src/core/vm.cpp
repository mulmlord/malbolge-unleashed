#include "core/vm.hpp"

#include <utility>

namespace malm::core {

Io::~Io() = default;
StepObserver::~StepObserver() = default;

std::string_view op_name(Op op) noexcept {
    switch (op) {
    case Op::Jmp:
        return "Jmp";
    case Op::Out:
        return "Out";
    case Op::In:
        return "In";
    case Op::Rot:
        return "Rot";
    case Op::MovD:
        return "MovD";
    case Op::Opr:
        return "Opr";
    case Op::Nop:
        return "Nop";
    case Op::Hlt:
        return "Hlt";
    }
    return "?";
}

std::string_view error_name(Error e) noexcept {
    switch (e) {
    case Error::Exec:
        return "E_EXEC";
    case Error::Encrypt:
        return "E_ENCRYPT";
    case Error::Width:
        return "E_WIDTH";
    case Error::Io:
        return "E_IO";
    }
    return "?";
}

std::string format_fault(const Fault& f) {
    std::string s(error_name(f.error));
    s += " at step " + std::to_string(f.steps) + ": C=" + f.c.to_string() +
         " D=" + f.d.to_string() + " A=" + f.a.to_string() + " n=" + std::to_string(f.n);
    if (f.overflow) {
        s += " (overflow)";
    }
    return s;
}

template <class M> Status BasicVm<M>::fail(Error e, const Regs& r, bool overflow) noexcept {
    fault_ =
        Fault{e, M::to_value(r.c), M::to_value(r.d), M::to_value(r.a), m_.n(), r.steps, overflow};
    return Status::Faulted;
}

template <class M>
template <class... Obs>
inline Status BasicVm<M>::step_inner(Regs& r, Io& io, Obs&... obs) {
    ++r.steps;

    // 1. Fetch check.
    const Word cell = m_.read(r.c, Port::C);
    if (!M::is_cell(cell)) {
        if constexpr (sizeof...(Obs) > 0) {
            (obs.before({r.steps, M::to_value(r.c), M::to_value(cell), false, Op::Nop,
                         M::to_value(r.d), M::to_value(m_.read(r.d)), M::to_value(r.a), m_.n()}),
             ...);
        }
        return fail(Error::Exec, r);
    }

    // 2. Decode.
    const Op op = decode(r.c94, M::cell_code(cell));
    if constexpr (sizeof...(Obs) > 0) {
        (obs.before({r.steps, M::to_value(r.c), M::to_value(cell), true, op, M::to_value(r.d),
                     M::to_value(m_.read(r.d)), M::to_value(r.a), m_.n()}),
         ...);
    }

    // 3. Execute. `target` is [C] afterwards: still the fetched cell (checked), unless the step
    // jumps or writes [D] with D == C; only those read and check it again (step 4's check).
    Word target = cell;
    switch (op) {
    case Op::Jmp:
        r.c = m_.read(r.d, Port::D);
        r.c94 = M::mod94(r.c);
        target = m_.read(r.c, Port::C);
        if (!M::is_cell(target)) {
            return fail(Error::Encrypt, r);
        }
        break;
    case Op::Out:
        if (!io.write_byte(static_cast<std::uint8_t>(M::mod256(r.a)))) {
            return fail(Error::Io, r);
        }
        break;
    case Op::In: {
        if (!io.flush()) {
            return fail(Error::Io, r);
        }
        int b = kEof;
        if (!eof_) {
            b = io.read_byte();
            if (b == kIoError) {
                return fail(Error::Io, r);
            }
        }
        if (b == kEof) {
            eof_ = true;
            r.a = M::eof();
        } else {
            r.a = M::from_byte(static_cast<std::uint8_t>(b));
        }
        break;
    }
    case Op::Rot:
        r.a = m_.rotr(m_.read(r.d, Port::D));
        m_.write(r.d, r.a, Port::D);
        if (r.d == r.c) {
            target = r.a;
            if (!M::is_cell(target)) {
                return fail(Error::Encrypt, r);
            }
        }
        break;
    case Op::MovD:
        r.d = m_.read(r.d, Port::D);
        break;
    case Op::Opr:
        r.a = M::crz(r.a, m_.read(r.d, Port::D));
        m_.write(r.d, r.a, Port::D);
        if (r.d == r.c) {
            target = r.a;
            if (!M::is_cell(target)) {
                return fail(Error::Encrypt, r);
            }
        }
        break;
    case Op::Nop:
        break;
    case Op::Hlt:
        halted_ = true;
        return Status::Halted;
    default: // decode only yields the 8 ops; no range check before the jump table
        std::unreachable();
    }

    // 4. Encrypt the cell at the current C.
    m_.write(r.c, M::encrypted(target), Port::C);

    // 5. Increment C and D.
    const std::optional<Word> nc = M::incremented(r.c);
    const std::optional<Word> nd = M::incremented(r.d);
    if (!nc || !nd) {
        return fail(Error::Width, r, true);
    }
    r.c = *nc;
    r.d = *nd;
    // The remainder rule is continuous under +1 except at the wrap to 0 (...222 has remainder 16
    // mod 94 in unleashed mode, as 59048 has in classic mode).
    r.c94 = r.c == Word{} || r.c94 == 93 ? 0 : r.c94 + 1;

    // 6. Growth rule.
    if (!m_.grow(r.c, r.d)) {
        return fail(Error::Width, r);
    }
    return Status::Continue;
}

template <class M> Status BasicVm<M>::finish(Io& io, Status s) {
    const bool flushed = io.flush();
    if (s == Status::Halted && !flushed) {
        halted_ = false;
        return fail(Error::Io, load_regs());
    }
    return s;
}

template <class M> StepResult BasicVm<M>::step(Io& io) {
    Status s;
    if (halted_) {
        s = Status::Halted;
    } else if (fault_) {
        s = Status::Faulted;
    } else {
        Regs r = load_regs();
        s = step_inner(r, io);
        store_regs(r);
        if (s != Status::Continue) {
            s = finish(io, s);
        }
    }
    return {s, s == Status::Faulted ? fault_ : std::nullopt};
}

template <class M>
template <class... Obs>
RunResult BasicVm<M>::run_impl(Io& io, std::optional<std::uint64_t> max_steps, Obs&... obs) {
    Status s = Status::Continue;
    if (halted_) {
        s = Status::Halted;
    } else if (fault_) {
        s = Status::Faulted;
    } else {
        const std::uint64_t limit = max_steps.value_or(~std::uint64_t{0});
        Regs r = load_regs();
        while (r.steps < limit) {
            s = step_inner(r, io, obs...);
            if (s != Status::Continue) {
                break;
            }
        }
        store_regs(r);
        if (s == Status::Continue) { // step limit
            const bool flushed = io.flush();
            if (!flushed) {
                s = fail(Error::Io, r);
            } else {
                return {RunResult::Kind::StepLimit, std::nullopt};
            }
        } else {
            s = finish(io, s);
        }
    }
    if (s == Status::Halted) {
        return {RunResult::Kind::Halted, std::nullopt};
    }
    return {RunResult::Kind::Fault, fault_};
}

template <class M> RunResult BasicVm<M>::run(Io& io, std::optional<std::uint64_t> max_steps) {
    return run_impl(io, max_steps);
}

template <class M>
RunResult BasicVm<M>::run(Io& io, StepObserver& obs, std::optional<std::uint64_t> max_steps) {
    return run_impl(io, max_steps, obs);
}

namespace {
// run_pure's I/O: In and Out never execute, so only a fault's flush reaches it.
struct NoIo final : Io {
    int read_byte() override { return kIoError; }
    bool write_byte(std::uint8_t) override { return false; }
    bool flush() override { return true; }
};
} // namespace

template <class M> RunResult BasicVm<M>::run_pure(std::optional<std::uint64_t> max_steps) {
    NoIo io;
    if (halted_ || fault_) {
        return run(io, max_steps);
    }
    const std::uint64_t limit = max_steps.value_or(~std::uint64_t{0});
    Regs r = load_regs();
    Status s = Status::Continue;
    bool before_io = false;
    while (r.steps < limit) {
        // The fetch and decode of the next step, which have no effects; step_inner repeats them.
        const Word cell = m_.read(r.c, Port::C);
        if (M::is_cell(cell)) {
            const Op op = decode(r.c94, M::cell_code(cell));
            if (op == Op::In || op == Op::Out || op == Op::Hlt) {
                before_io = true;
                break;
            }
        }
        s = step_inner(r, io);
        if (s != Status::Continue) {
            break;
        }
    }
    store_regs(r);
    if (s == Status::Faulted) {
        finish(io, s);
        return {RunResult::Kind::Fault, fault_};
    }
    return {before_io ? RunResult::Kind::BeforeIo : RunResult::Kind::StepLimit, std::nullopt};
}

template class BasicVm<UnleashedMachine>;
template class BasicVm<ClassicMachine>;

} // namespace malm::core
