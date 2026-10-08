#include "asm/diagnostics.hpp"

#include <algorithm>
#include <utility>

namespace malm::assembler {
namespace {

std::string where(const SourceLoc& loc, std::span<const SourceFile> files) {
    const std::string path = loc.file < files.size() ? files[loc.file].path : "<unknown>";
    return path + ':' + std::to_string(loc.line) + ':' + std::to_string(loc.col);
}

// "included from here" lines for every include level above `file`.
std::string chain_lines(std::uint32_t file, std::span<const SourceFile> files) {
    std::string out;
    // Bounded by the include depth limit; a malformed table cannot loop forever.
    for (std::size_t depth = 0; file < files.size() && files[file].included_from && depth < 1000;
         ++depth) {
        const SourceLoc& from = *files[file].included_from;
        out += where(from, files) + ": note: included from here\n";
        file = from.file;
    }
    return out;
}

} // namespace

Diagnostics::Key Diagnostics::key_of(const SourceLoc& loc) const {
    // The position in the root file, then the positions inside each nested include.
    Key k{{loc.line, loc.col}};
    std::uint32_t file = loc.file;
    for (std::size_t depth = 0; file < chains_.size() && chains_[file] && depth < 1000; ++depth) {
        k.emplace_back(chains_[file]->line, chains_[file]->col);
        file = chains_[file]->file;
    }
    std::reverse(k.begin(), k.end());
    return k;
}

void Diagnostics::find_worst() {
    worst_ = 0;
    for (std::size_t i = 1; i < entries_.size(); ++i) {
        if (before(entries_[worst_], entries_[i])) {
            worst_ = i;
        }
    }
}

Diagnostic& Diagnostics::error(SourceLoc loc, std::string message) {
    Entry e{key_of(loc), raised_++, Diagnostic{loc, std::move(message), {}}};
    if (entries_.size() < max_) {
        entries_.push_back(std::move(e));
        if (before(entries_[worst_], entries_.back())) {
            worst_ = entries_.size() - 1;
        }
        return entries_.back().d;
    }
    if (entries_.empty() || !before(e, entries_[worst_])) {
        scratch_ = Diagnostic{};
        return scratch_;
    }
    entries_[worst_] = std::move(e);
    const std::size_t at = worst_;
    find_worst();
    return entries_[at].d;
}

std::vector<Diagnostic> Diagnostics::items() const {
    std::vector<const Entry*> order;
    order.reserve(entries_.size());
    for (const Entry& e : entries_) {
        order.push_back(&e);
    }
    std::sort(order.begin(), order.end(),
              [](const Entry* a, const Entry* b) { return before(*a, *b); });
    std::vector<Diagnostic> out;
    out.reserve(order.size());
    for (const Entry* e : order) {
        out.push_back(e->d);
    }
    return out;
}

std::string format_diagnostic(const Diagnostic& d, std::span<const SourceFile> files) {
    std::string out = where(d.loc, files) + ": error: " + d.message + '\n';
    for (const Note& n : d.notes) {
        out += where(n.loc, files) + ": note: " + n.message + '\n';
        if (n.loc.file != d.loc.file) {
            out += chain_lines(n.loc.file, files); // tells two inclusions of one file apart
        }
    }
    out += chain_lines(d.loc.file, files);
    return out;
}

std::string format_diagnostics(const Diagnostics& ds, std::span<const SourceFile> files) {
    std::string out;
    for (const Diagnostic& d : ds.items()) {
        out += format_diagnostic(d, files);
    }
    if (ds.truncated()) {
        out += "too many errors; only the first " + std::to_string(Diagnostics::kMaxErrors) +
               " are shown\n";
    }
    return out;
}

} // namespace malm::assembler
