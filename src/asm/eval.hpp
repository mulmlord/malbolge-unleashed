#pragma once

// Evaluation of assembly expressions (docs/asm.md 4.3), after layout. Values are exact 3-adic
// values; every operator either yields the exact result or reports an error.
//
//  * a label leaf X evaluates to addr(X) - 1, R_X to addr(X)
//  * `!` is crz(left, right), with the left operand in the A role (spec 3.2)
//  * `x >> k` is rotr applied k times over the program width n, `x << k` is `x >> (n - k)`;
//    k must be a natural 0..n and x must have width <= n

#include <functional>
#include <optional>
#include <string>

#include "asm/ast.hpp"
#include "asm/diagnostics.hpp"

namespace malm::assembler {

// The address of a label. Precondition of evaluate(): every label of the expression resolves (the
// static checks guarantee it).
using LabelAddress = std::function<Value(const std::string& name)>;

// Evaluates `e` for program width `n` (10..39). Errors are reported at the offending operator or
// leaf, and the result is then nullopt.
std::optional<Value> evaluate(const Expr& e, unsigned n, const LabelAddress& address,
                              Diagnostics& diags);

} // namespace malm::assembler
