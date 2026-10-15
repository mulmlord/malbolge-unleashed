// Shared by the subcommands: reading a program file (.malm or Malbolge text), writing outputs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "container/container.hpp"
#include "container/sha256.hpp"
#include "core/loader.hpp"

namespace malm {

struct LoadedProgram {
    std::vector<std::uint8_t> cells;
    bool classic = false;
    bool exit_a = false; // from the .malm header; always false for text
    bool from_malm = false;
    std::optional<container::Sha256> file_hash; // of the file's bytes, if load_program was asked
};

// Reads a whole regular file. On failure prints `malm: ...` to `err` and returns false.
bool read_file(std::string_view path, std::vector<std::uint8_t>& bytes, std::ostream& err);

// True if both paths name the same existing file, or are spelled the same; "-" (stdout) is never
// the same file.
bool same_file(std::string_view a, std::string_view b);

// Writes `bytes` to `path`, or to `out` for "-". On failure prints a diagnostic and returns false.
bool write_output(std::string_view path, const std::vector<std::uint8_t>& bytes, std::ostream& out,
                  std::ostream& err);

// Purity check of an assembled program (spec 4): the program text passes the strict loader
// unchanged, and the .malm holds exactly these cells, unleashed, with the given EXIT_A flag, and
// nothing else. A failure is an assembler bug.
bool is_pure(const std::vector<std::uint8_t>& text, const std::vector<std::uint8_t>& malm,
             bool exit_a);

// Parses a positive decimal integer fitting 64 bits (step numbers and counts). Returns nullopt
// otherwise.
std::optional<std::uint64_t> parse_count(std::string_view text);

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
// with the format limit of the mode (the smaller one applies). With `hash_file`, out.file_hash is
// the SHA-256 of the file (the key of startup snapshots). Returns 0 on success, else the exit
// status (1: file/format error, 2: usage error such as --classic on an unleashed .malm) after
// printing a diagnostic to `err`.
int load_program(std::string_view path, bool classic_flag, std::ostream& err, LoadedProgram& out,
                 std::size_t max_cells = core::kDefaultMaxCells, bool hash_file = false);

} // namespace malm
