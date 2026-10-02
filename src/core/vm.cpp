#include "core/vm.hpp"

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

template <class M>
Status BasicVm<M>::fail(Error e, Word a, Word c, Word d, bool overflow) noexcept {
    fault_ = Fault{e, M::to_value(c), M::to_value(d), M::to_value(a), m_.n(), steps_, overflow};
    return Status::Faulted;
}

template <class M> template <class... Obs> Status BasicVm<M>::step_inner(Io& io, Obs&... obs) {
    ++steps_;

    // 1. Fetch check.
    const Word cell = m_.read(c_);
    if (!M::is_cell(cell)) {
        if constexpr (sizeof...(Obs) > 0) {
            (obs.before({steps_, M::to_value(c_), M::to_value(cell), false, Op::Nop,
                         M::to_value(d_), M::to_value(m_.read(d_)), M::to_value(a_), m_.n()}),
             ...);
        }
        return fail(Error::Exec, a_, c_, d_);
    }

    // 2. Decode.
    const Op op = decode(M::mod94(c_), M::cell_code(cell));
    if constexpr (sizeof...(Obs) > 0) {
        (obs.before({steps_, M::to_value(c_), M::to_value(cell), true, op, M::to_value(d_),
                     M::to_value(m_.read(d_)), M::to_value(a_), m_.n()}),
         ...);
    }

    // 3. Execute.
    switch (op) {
    case Op::Jmp:
        c_ = m_.read(d_);
        break;
    case Op::Out:
        if (!io.write_byte(static_cast<std::uint8_t>(M::mod256(a_)))) {
            return fail(Error::Io, a_, c_, d_);
        }
        break;
    case Op::In: {
        if (!io.flush()) {
            return fail(Error::Io, a_, c_, d_);
        }
        int b = kEof;
        if (!eof_) {
            b = io.read_byte();
            if (b == kIoError) {
                return fail(Error::Io, a_, c_, d_);
            }
        }
        if (b == kEof) {
            eof_ = true;
            a_ = M::eof();
        } else {
            a_ = M::from_byte(static_cast<std::uint8_t>(b));
        }
        break;
    }
    case Op::Rot:
        a_ = m_.rotr(m_.read(d_));
        m_.write(d_, a_);
        break;
    case Op::MovD:
        d_ = m_.read(d_);
        break;
    case Op::Opr:
        a_ = M::crz(a_, m_.read(d_));
        m_.write(d_, a_);
        break;
    case Op::Nop:
        break;
    case Op::Hlt:
        halted_ = true;
        return Status::Halted;
    }

    // 4. Encrypt the cell at the current C.
    const Word target = m_.read(c_);
    if (!M::is_cell(target)) {
        return fail(Error::Encrypt, a_, c_, d_);
    }
    m_.write(c_, M::encrypted(target));

    // 5. Increment C and D.
    const std::optional<Word> nc = M::incremented(c_);
    const std::optional<Word> nd = M::incremented(d_);
    if (!nc || !nd) {
        return fail(Error::Width, a_, c_, d_, true);
    }
    c_ = *nc;
    d_ = *nd;

    // 6. Growth rule.
    if (!m_.grow(c_, d_)) {
        return fail(Error::Width, a_, c_, d_);
    }
    return Status::Continue;
}

template <class M> Status BasicVm<M>::finish(Io& io, Status s) {
    const bool flushed = io.flush();
    if (s == Status::Halted && !flushed) {
        halted_ = false;
        return fail(Error::Io, a_, c_, d_);
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
        s = step_inner(io);
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
        while (steps_ < limit) {
            s = step_inner(io, obs...);
            if (s != Status::Continue) {
                break;
            }
        }
        if (s == Status::Continue) { // step limit
            const bool flushed = io.flush();
            if (!flushed) {
                s = fail(Error::Io, a_, c_, d_);
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

template class BasicVm<UnleashedMachine>;
template class BasicVm<ClassicMachine>;

} // namespace malm::core
