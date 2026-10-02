// Shared by `malm run` and `malm export`: reading a program file (.malm or Malbolge text).
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "container/container.hpp"
#include "core/loader.hpp"

namespace malm {

struct LoadedProgram {
    std::vector<std::uint8_t> cells;
    bool classic = false;
    bool exit_a = false; // from the .malm header; always false for text
    bool from_malm = false;
};

// Reads a whole regular file. On failure prints `malm: ...` to `err` and returns false.
bool read_file(std::string_view path, std::vector<std::uint8_t>& bytes, std::ostream& err);

// Parses a --max-cells value: a positive decimal integer. Returns nullopt otherwise.
std::optional<std::size_t> parse_max_cells(std::string_view text);

// Checks a --max-cells value against the mode's format limit. Returns the message
// ("--max-cells N exceeds the format limit M") or an empty string if it is acceptable.
std::string max_cells_limit_error(bool classic, std::size_t max_cells);

// Encodes a loaded program as a .malm (the mode and EXIT_A of the program, OR-ed with `exit_a`).
std::vector<std::uint8_t> encode_malm(LoadedProgram&& prog, bool exit_a, container::Codec codec);

// Loads `path`, autodetecting .malm by its magic. For a .malm the header's mode decides;
// `classic_flag` (--classic) must agree with it. For text, `classic_flag` selects the mode and the
// loader's cell limit. `max_cells` is the reader's resource cap (--max-cells); it is combined
// with the format limit of the mode (the smaller one applies). Returns 0 on success, else the exit
// status (1: file/format error, 2: usage error such as --classic on an unleashed .malm) after
// printing a diagnostic to `err`.
int load_program(std::string_view path, bool classic_flag, std::ostream& err, LoadedProgram& out,
                 std::size_t max_cells = core::kDefaultMaxCells);

} // namespace malm
