#pragma once

// Source locations and diagnostics of the assembler front end (docs/asm.md 9).
//
// A diagnostic is `file:line:col: error: message`, followed by `file:line:col: note: ...` lines.
// Diagnostics inside an included file are followed by one `included from here` note per include
// level (docs/asm.md 8.3).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace malm::assembler {

// `file` indexes Program::files (SourceFile). Lines and columns are 1-based; a column counts
// bytes.
struct SourceLoc {
    std::uint32_t file = 0;
    std::uint32_t line = 1;
    std::uint32_t col = 1;
    friend bool operator==(const SourceLoc&, const SourceLoc&) = default;
};

struct SourceFile {
    std::string path;                       // as written, shown in diagnostics
    std::optional<SourceLoc> included_from; // the .INCLUDE directive; none for the root file
};

struct Note {
    SourceLoc loc;
    std::string message;
};

struct Diagnostic {
    SourceLoc loc;
    std::string message;
    std::vector<Note> notes;
};

// Collects errors and keeps the first `max_errors` (1000) in source order: by position in the root
// file, an error inside an included file counting at its `.INCLUDE` line, ties in the order
// raised. Memory stays bounded however many errors are raised. The file table (needed for the
// include chain of an error) is registered with add_file(), in the order of SourceFile ids.
class Diagnostics {
  public:
    static constexpr std::size_t kMaxErrors = 1000;

    explicit Diagnostics(std::size_t max_errors = kMaxErrors) : max_(max_errors) {}

    void add_file(std::optional<SourceLoc> included_from) { chains_.push_back(included_from); }

    // The returned reference is valid until the next call; use it to add notes. An error that is
    // not kept refers to a scratch diagnostic.
    Diagnostic& error(SourceLoc loc, std::string message);

    bool has_errors() const noexcept { return raised_ > 0; }
    bool truncated() const noexcept { return raised_ > entries_.size(); }
    std::size_t raised() const noexcept { return raised_; }

    // The kept diagnostics in source order.
    std::vector<Diagnostic> items() const;

  private:
    using Key = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
    struct Entry {
        Key key;
        std::uint64_t seq;
        Diagnostic d;
    };
    static bool before(const Entry& a, const Entry& b) {
        return a.key != b.key ? a.key < b.key : a.seq < b.seq;
    }
    Key key_of(const SourceLoc& loc) const;
    void find_worst();

    std::size_t max_;
    std::vector<std::optional<SourceLoc>> chains_;
    std::vector<Entry> entries_;
    std::size_t worst_ = 0;
    std::size_t raised_ = 0;
    Diagnostic scratch_;
};

// `file:line:col: error: message`, its notes, and the include chains; every line ends in '\n'.
std::string format_diagnostic(const Diagnostic& d, std::span<const SourceFile> files);
// Formats items(), then a line when diagnostics were dropped.
std::string format_diagnostics(const Diagnostics& ds, std::span<const SourceFile> files);

} // namespace malm::assembler
