#include "asm/eval.hpp"

#include "core/ops.hpp"

namespace malm::assembler {

namespace {

const char* op_text(BinOp op) {
    switch (op) {
    case BinOp::Add:
        return "+";
    case BinOp::Sub:
        return "-";
    case BinOp::Shl:
        return "<<";
    case BinOp::Shr:
        return ">>";
    case BinOp::Crz:
        return "!";
    }
    return "?";
}

class Evaluator {
  public:
    Evaluator(unsigned n, const LabelAddress& address, Diagnostics& d)
        : n_(n), address_(address), d_(d) {}

    std::optional<Value> eval(const Expr& e) {
        switch (e.kind) {
        case Expr::Kind::Literal:
            return e.value;
        case Expr::Kind::Label:
            return checked(subtract(address_(e.name), *Value::natural(1)), e, "label");
        case Expr::Kind::Restore:
            return address_(e.name);
        case Expr::Kind::Neg: {
            const auto x = eval(e.kids[0]);
            return x ? checked(negate(*x), e, "unary '-'") : std::nullopt;
        }
        case Expr::Kind::Binary:
            return binary(e);
        }
        return std::nullopt;
    }

  private:
    unsigned n_;
    const LabelAddress& address_;
    Diagnostics& d_;

    std::optional<Value> checked(std::optional<Value> v, const Expr& e, const std::string& what) {
        if (!v) {
            d_.error(e.loc, "the result of " + what + " is out of range (wider than 39 trits)");
        }
        return v;
    }

    std::optional<Value> binary(const Expr& e) {
        const auto l = eval(e.kids[0]);
        const auto r = eval(e.kids[1]);
        if (!l || !r) {
            return std::nullopt;
        }
        const std::string what = std::string("'") + op_text(e.op) + "'";
        switch (e.op) {
        case BinOp::Add:
            return checked(add(*l, *r), e, what);
        case BinOp::Sub:
            return checked(subtract(*l, *r), e, what);
        case BinOp::Crz:
            return core::crz(*l, *r);
        case BinOp::Shl:
        case BinOp::Shr:
            return rotate(e, *l, *r);
        }
        return std::nullopt;
    }

    std::optional<Value> rotate(const Expr& e, Value x, Value k) {
        const std::string what = std::string("'") + op_text(e.op) + "'";
        const std::string n = "n = " + std::to_string(n_);
        if (k.head() != 0 || k.raw() > n_) {
            d_.error(e.kids[1].loc, "the count of " + what +
                                        " must be a natural number from 0 to the program width (" +
                                        n + ")");
            return std::nullopt;
        }
        if (!x.fits_width(n_)) {
            d_.error(e.kids[0].loc, "the operand of " + what + " has width " +
                                        std::to_string(x.width()) +
                                        ", wider than the program width (" + n + ")");
            return std::nullopt;
        }
        std::uint64_t right = k.raw();
        if (e.op == BinOp::Shl) {
            right = n_ - right;
        }
        for (std::uint64_t i = 0; i < right; ++i) {
            x = core::rotr(x, n_);
        }
        return x;
    }
};

} // namespace

std::optional<Value> evaluate(const Expr& e, unsigned n, const LabelAddress& address,
                              Diagnostics& diags) {
    return Evaluator(n, address, diags).eval(e);
}

} // namespace malm::assembler
