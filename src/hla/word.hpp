#pragma once

// 32-bit words and the constants the HLA lowering works with (docs/hla.md 4,
// docs/adr/0001-hla-word-representation.md).
//
// A word w is one cell: bit i is trit 4 + i, stored as 0 or 2 ("f02"); the trits 0..3, 36 and above
// and the head are 0. With A = C1, crz is the identity on such a cell, which is how a register is
// read into A without changing it. HLA programs run at width 38.
//
// Tables. The lowering dispatches on a byte through tables of the head-1 window (docs/hla.md 4.4):
//  * lane tables: the index of a register's byte lane (lane_index) has the lane's 8 bits as the
//    trits 0/1, the other bit trits 1, the trits 36, 37 and the head 1 and a tag in the trits 0..3
//    (each 0 or 1), so different tables never share an index; consecutive indices are 81 apart;
//  * the input table: In leaves a byte b (or EOF) in A, and crz(b, in_seed()) spreads it over 729
//    consecutive cells with the trit 35 = 2, which no lane index has. EOF lands in head 0.

#include <cstdint>
#include <optional>
#include <vector>

#include "core/value.hpp"

namespace malm::hla {

using core::Value;

inline constexpr unsigned kWidth = 38; // the program width of every HLA program
inline constexpr unsigned kBit0 = 4;   // trit of bit 0
inline constexpr unsigned kWordBits = 32;
inline constexpr unsigned kLanes = 4;

// A value from its low trits (trit 0 first) and its head; the trits above `low` equal the head.
Value make_value(const std::vector<unsigned>& low, unsigned head);

// The cell of w, and back (nullopt if `v` is not a word cell).
Value word(std::uint32_t w);
std::optional<std::uint32_t> word_value(Value v);

// The `a` with crz(a, from) == to, if one exists (a trit 0 in `from` cannot become 2).
std::optional<Value> crz_operand(Value from, Value to);

// Loading a constant into A without changing any cell (docs/asm.md 10.3): Rot of C0 or C1 (both
// are rotation fixed points), then Opr on inline constants, each a fixed point of the A it meets.
// At most four Oprs are needed for any value of width <= 38 with head 0 or 1.
struct Load {
    Value start;             // C0 or C1
    std::vector<Value> oprs; // the inline constants, in order
};
Load load(Value target);

// Lane tables (see above). The tag is 4 bits (trits 0..3).
Value lane_index(unsigned lane, unsigned tag, std::uint8_t byte);
// The constant K of the index sequence (docs/hla.md 4.4):
//   A := C1; Opr I; Opr I; Opr X; Opr I; A := K; Opr I; Opr I
// leaves I = lane_index(lane, tag, byte of X) and X unchanged.
Value lane_mask(unsigned lane, unsigned tag);

// The input table: the cell the dispatch crazies In's value into, and its value afterwards for a
// byte (0..255) or for EOF (-1).
Value in_seed();
Value in_index(int byte_or_eof);

} // namespace malm::hla
