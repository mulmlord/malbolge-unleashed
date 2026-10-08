#pragma once

// Recursive-descent parser (docs/asm.md 2-5, 8). Builds the AST from the token stream and expands
// `.INCLUDE` in place.
//
// Errors and recovery. A syntax error is reported, the rest of its line is skipped (never past a
// `{`, `}`, directive or blank-line block boundary), and parsing goes on in the same block, so
// independent errors on later lines are all reported. Labels waiting for a word when an error
// strikes stay pending and attach to the next word. The checks of check.hpp need an AST without
// holes, so callers run them only when the parse produced no errors (see analyze()).
//
// Parser-level (not check.hpp) errors are the ones local to the token stream: lexical and syntax
// errors, reserved words as labels, label without word, `.WIDTH` range and placement, offset
// placement, empty blocks and `.INCLUDE` (cycle, missing file, placement).

#include <cstddef>
#include <string>
#include <unordered_map>

#include "asm/ast.hpp"
#include "asm/diagnostics.hpp"
#include "core/result.hpp"

namespace malm::assembler {

// The error a loader returns when a file exceeds `max_bytes`; the parser turns it into the
// source-size limit error.
inline constexpr const char* kSourceTooLarge = "source too large";

// Source of included files; injectable so tests can use an in-memory file map.
class FileLoader {
  public:
    virtual ~FileLoader() = default;
    // The content of `path` (already resolved against the including file's directory), or an error
    // message such as "No such file" (shown as `cannot open 'path': <message>`). A file larger than
    // `max_bytes` is the error kSourceTooLarge.
    virtual core::Result<std::string, std::string> read(const std::string& path,
                                                        std::size_t max_bytes) = 0;
    // The identity of a file: two paths naming the same file have the same identity. Include cycle
    // detection compares identities. The default is the lexically normalised path.
    virtual std::string identity(const std::string& path);
};

// Reads regular files from the file system. identity() resolves symlinks the way the OS does.
class FsLoader final : public FileLoader {
  public:
    core::Result<std::string, std::string> read(const std::string& path,
                                                std::size_t max_bytes) override;
    std::string identity(const std::string& path) override;
};

// A fixed set of files keyed by their (lexically normalised) path.
class MemoryLoader final : public FileLoader {
  public:
    MemoryLoader() = default;
    explicit MemoryLoader(std::unordered_map<std::string, std::string> files)
        : files_(std::move(files)) {}
    core::Result<std::string, std::string> read(const std::string& path,
                                                std::size_t max_bytes) override;

  private:
    std::unordered_map<std::string, std::string> files_;
};

// Resource limits against untrusted input (docs/asm.md 8.3). Exceeding one is a single error and
// parsing stops. The defaults are the documented limits; tests lower them.
struct ParseLimits {
    std::size_t max_source_bytes = std::size_t{64} << 20; // summed over all files
    std::size_t max_cells = std::size_t{4} << 20;         // strings count one cell per byte
    unsigned max_include_depth = 64;                      // nested levels below the root file
    unsigned max_includes = 1000;                         // .INCLUDE directives followed
};

// Parses `path` (read through `loader`) and everything it includes.
Program parse_program(const std::string& path, FileLoader& loader, Diagnostics& diags,
                      const ParseLimits& limits = {});

} // namespace malm::assembler
