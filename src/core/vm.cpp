#include "core/vm.hpp"

#include "core/ops.hpp"
#include "core/tables.hpp"

namespace malm::core {

Io::~Io() = default;

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

Status Vm::fail(Error e, Value a, Value c, Value d, bool overflow) noexcept {
    fault_ = Fault{e, c, d, a, mem_.n(), steps_, overflow};
    return Status::Faulted;
}

namespace {
constexpr bool is_cell_value(std::uint64_t raw) noexcept { return raw >= 33 && raw <= 126; }
} // namespace

Status Vm::step_inner(Io& io) {
    ++steps_;

    // 1. Fetch check.
    const Value cell = mem_.read(c_);
    if (!is_cell_value(cell.raw())) {
        return fail(Error::Exec, a_, c_, d_);
    }

    // 2. Decode.
    const Op op = decode(c_.mod94(), static_cast<std::uint32_t>(cell.raw()));

    // 3. Execute.
    switch (op) {
    case Op::Jmp:
        c_ = mem_.read(d_);
        break;
    case Op::Out:
        if (!io.write_byte(static_cast<std::uint8_t>(a_.mod(256)))) {
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
            a_ = Value::from_raw(pow3[kTrits] - 1); // ...222
        } else {
            a_ = Value::from_raw(static_cast<std::uint64_t>(b));
        }
        break;
    }
    case Op::Rot:
        a_ = rotr(mem_.read(d_), mem_.n());
        mem_.write(d_, a_);
        break;
    case Op::MovD:
        d_ = mem_.read(d_);
        break;
    case Op::Opr:
        a_ = crz(a_, mem_.read(d_));
        mem_.write(d_, a_);
        break;
    case Op::Nop:
        break;
    case Op::Hlt:
        halted_ = true;
        return Status::Halted;
    }

    // 4. Encrypt the cell at the current C.
    const Value target = mem_.read(c_);
    if (!is_cell_value(target.raw())) {
        return fail(Error::Encrypt, a_, c_, d_);
    }
    mem_.write(c_, Value::from_raw(encrypt(static_cast<std::uint32_t>(target.raw()))));

    // 5. Increment C and D.
    const std::optional<Value> nc = c_.incremented();
    const std::optional<Value> nd = d_.incremented();
    if (!nc || !nd) {
        return fail(Error::Width, a_, c_, d_, true);
    }
    c_ = *nc;
    d_ = *nd;

    // 6. Growth rule.
    if (!mem_.grow_for(c_, d_)) {
        return fail(Error::Width, a_, c_, d_);
    }
    return Status::Continue;
}

Status Vm::finish(Io& io, Status s) {
    const bool flushed = io.flush();
    if (s == Status::Halted && !flushed) {
        halted_ = false;
        return fail(Error::Io, a_, c_, d_);
    }
    return s;
}

StepResult Vm::step(Io& io) {
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

RunResult Vm::run(Io& io, std::optional<std::uint64_t> max_steps) {
    Status s = Status::Continue;
    if (halted_) {
        s = Status::Halted;
    } else if (fault_) {
        s = Status::Faulted;
    } else {
        const std::uint64_t limit = max_steps.value_or(~std::uint64_t{0});
        while (steps_ < limit) {
            s = step_inner(io);
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

int exit_status(const RunResult& r, const Vm& vm, bool exit_a) noexcept {
    if (r.kind == RunResult::Kind::Halted) {
        return exit_a ? static_cast<int>(vm.a().mod(256)) : 0;
    }
    return 1;
}

} // namespace malm::core
