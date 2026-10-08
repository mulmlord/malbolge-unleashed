#include "asm/parser.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>

#include "asm/lexer.hpp"

namespace malm::assembler {
namespace {

constexpr std::array<std::pair<std::string_view, Op>, 8> kCommands{{
    {"Nop", Op::Nop},
    {"Jmp", Op::Jmp},
    {"MovD", Op::MovD},
    {"Opr", Op::Opr},
    {"Rot", Op::Rot},
    {"In", Op::In},
    {"Out", Op::Out},
    {"Hlt", Op::Hlt},
}};

std::optional<Op> command_of(std::string_view name) {
    for (const auto& [n, op] : kCommands) {
        if (n == name) {
            return op;
        }
    }
    return std::nullopt;
}

// docs/asm.md 4.2, named constants.
std::optional<Value> constant_of(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kConsts{{
        {"C0", "0t0"},
        {"C1", "...1"},
        {"C2", "...2"},
        {"C21", "...21"},
        {"C20", "...20"},
        {"EOF", "...2"},
    }};
    for (const auto& [n, text] : kConsts) {
        if (n == name) {
            return Value::parse(text);
        }
    }
    return std::nullopt;
}

bool is_reserved(std::string_view name) {
    return command_of(name) || name == "RNop" || constant_of(name);
}

constexpr unsigned kMaxParseDepth = 128;
const std::string kDepthMessage = "parentheses and unary minus are nested more than " +
                                  std::to_string(kMaxParseDepth) + " levels deep";
const std::string kHeightMessage =
    "expression is more than " + std::to_string(kMaxExprHeight) + " levels deep";

struct Shared {
    FileLoader& loader;
    Diagnostics& diags;
    Program& prog;
    const ParseLimits& limits;
    std::vector<std::string> stack; // identities of the files being parsed
    bool seen_section = false;
    bool seen_include = false;
    bool stopped = false; // a resource limit was hit: one error was reported, stop parsing
    unsigned includes = 0;
    std::size_t source_bytes = 0;
    std::size_t cells = 0;

    void limit_error(SourceLoc loc, std::string msg) {
        if (!stopped) {
            stopped = true;
            diags.error(loc, std::move(msg));
        }
    }
    std::string source_limit_message() const {
        return "source text exceeds the limit of " + std::to_string(limits.max_source_bytes) +
               " bytes";
    }
};

class FileParser {
  public:
    FileParser(Shared& sh, std::uint32_t id, std::string path, std::string_view text,
               bool is_include)
        : sh_(sh), dir_(std::filesystem::path(std::move(path)).parent_path()),
          is_include_(is_include), stream_(text, id, sh.diags) {}

    void run() {
        for (;;) {
            if (sh_.stopped) {
                return;
            }
            const std::size_t before = i_;
            const Token& t = tok();
            if (t.kind == Tok::Eof) {
                if (braced_) {
                    err(brace_loc_, "unterminated '{'");
                }
                close_block();
                return;
            }
            switch (t.kind) {
            case Tok::Directive:
                if (t.dir == Dir::Offset) {
                    handle_word_start();
                } else {
                    handle_directive();
                }
                break;
            case Tok::LBrace:
                handle_lbrace();
                break;
            case Tok::RBrace:
                if (braced_) {
                    close_block();
                } else if (stray_close_ok_) {
                    stray_close_ok_ = false; // the `}` of a block that `.CODE`/`.DATA` cut short
                } else {
                    err(t.loc, "unmatched '}'");
                }
                bump();
                break;
            default:
                handle_word_start();
                break;
            }
            if (i_ == before) {
                bump(); // every iteration makes progress
            }
        }
    }

    // Registers and parses the root file.
    static void run_root(Shared& sh, const std::string& path) {
        sh.prog.files.push_back(SourceFile{path, std::nullopt});
        sh.diags.add_file(std::nullopt);
        const SourceLoc at{0, 1, 1};
        auto text = sh.loader.read(path, sh.limits.max_source_bytes);
        if (!text && text.error() == kSourceTooLarge) {
            sh.limit_error(at, sh.source_limit_message());
            return;
        }
        if (!text) {
            sh.diags.error(at, "cannot open '" + path + "': " + text.error());
            return;
        }
        if (text.value().size() > sh.limits.max_source_bytes) {
            sh.limit_error(at, sh.source_limit_message());
            return;
        }
        sh.source_bytes += text.value().size();
        sh.stack.push_back(sh.loader.identity(path));
        FileParser(sh, 0, path, text.value(), false).run();
    }

  private:
    Shared& sh_;
    std::filesystem::path dir_;
    bool is_include_;
    TokenStream stream_;
    std::deque<Token> win_; // a sliding window of tokens; win_[0] has absolute index base_
    std::size_t base_ = 0;
    std::size_t i_ = 0;           // absolute index of the current token
    std::uint32_t prev_line_ = 0; // line of the token before the current one (0: none)
    Tok prev_kind_ = Tok::Eof;
    bool stray_close_ok_ = false;

    std::optional<Section> section_;
    bool reported_no_section_ = false;

    bool open_ = false;
    bool braced_ = false;
    SourceLoc block_first_loc_; // location of the token that opened the current block
    bool block_err_ = false;
    SourceLoc brace_loc_;
    Block blk_;
    std::vector<Label> pending_;
    unsigned depth_ = 0;

    // ---- token access ----
    const Token& tok(std::size_t k = 0) {
        while (base_ + win_.size() <= i_ + k) {
            win_.push_back(stream_.next());
        }
        return win_[i_ + k - base_];
    }
    void bump() {
        const Token& t = tok();
        if (t.kind == Tok::Eof) {
            return;
        }
        prev_line_ = t.loc.line;
        prev_kind_ = t.kind;
        ++i_;
        while (i_ - base_ > 16) { // references to older tokens are never held
            win_.pop_front();
            ++base_;
        }
    }

    Diagnostic& err(SourceLoc loc, std::string msg) {
        block_err_ = true;
        return sh_.diags.error(loc, std::move(msg));
    }

    // A token that cannot continue the current construct: it ends the block (spec 2).
    bool is_block_end(const Token& t) const {
        switch (t.kind) {
        case Tok::Eof:
        case Tok::RBrace:
        case Tok::LBrace:
        case Tok::Directive:
            return true;
        default:
            // The first token of an unbraced block follows the blank line that opened it.
            return open_ && !braced_ && t.blank_before && t.loc != block_first_loc_;
        }
    }

    std::string found(const Token& t) const {
        if (t.kind != Tok::Eof && t.kind != Tok::Directive && t.kind != Tok::LBrace &&
            t.kind != Tok::RBrace && is_block_end(t)) {
            return "end of block";
        }
        return describe(t);
    }

    // Error recovery: skip the rest of the line, never over a boundary token.
    void skip_line() {
        const std::uint32_t line = tok().loc.line;
        while (!is_block_end(tok()) && tok().loc.line == line) {
            bump();
        }
    }

    // ---- blocks ----
    void open_block(bool braced, SourceLoc loc) {
        blk_ = Block{};
        blk_.section = *section_;
        blk_.braced = braced;
        blk_.loc = loc;
        pending_.clear();
        open_ = true;
        braced_ = braced;
        block_err_ = false;
        block_first_loc_ = tok().loc;
    }

    void close_block() {
        if (!open_) {
            return;
        }
        open_ = false;
        braced_ = false;
        if (!block_err_) {
            for (const Label& l : pending_) {
                sh_.diags.error(l.loc, "label '" + l.name + "' is not followed by a word");
            }
        }
        if (!blk_.cells.empty()) {
            sh_.prog.blocks.push_back(std::move(blk_));
        } else if (!block_err_ && pending_.empty()) {
            sh_.diags.error(blk_.loc, "empty block");
        }
        blk_ = Block{};
        pending_.clear();
    }

    void report_no_section(SourceLoc loc) {
        if (!reported_no_section_) {
            reported_no_section_ = true;
            err(loc, "block outside any section: expected .CODE or .DATA first");
        }
    }

    void handle_lbrace() {
        const Token& t = tok();
        stray_close_ok_ = false;
        if (braced_) {
            err(t.loc, "braces do not nest");
            bump();
            return;
        }
        close_block();
        if (!section_) {
            report_no_section(t.loc);
            bump();
            while (tok().kind != Tok::Eof && tok().kind != Tok::RBrace &&
                   tok().kind != Tok::Directive) {
                bump(); // the block has no section: skip it, but not across a directive
            }
            if (tok().kind == Tok::RBrace) {
                bump();
            }
            return;
        }
        open_block(true, t.loc);
        brace_loc_ = t.loc;
        bump();
    }

    void handle_directive() {
        const Token t = tok();
        switch (t.dir) {
        case Dir::Code:
        case Dir::Data:
            if (braced_) {
                err(t.loc, "'" + t.text + "' inside braces; missing '}'");
                stray_close_ok_ = true;
            }
            close_block();
            section_ = t.dir == Dir::Code ? Section::Code : Section::Data;
            sh_.seen_section = true;
            bump();
            break;
        case Dir::Width:
            handle_width();
            break;
        case Dir::Include:
            handle_include();
            break;
        case Dir::Offset:
            break; // routed to handle_word_start
        }
    }

    void handle_width() {
        const Token w = tok();
        bump();
        const Token n = tok();
        if (n.kind != Tok::Number || !n.decimal) {
            err(n.loc, "'.WIDTH' needs a decimal number, found " + found(n));
            skip_line();
            return;
        }
        bump();
        if (is_include_) {
            err(w.loc, "'.WIDTH' is not allowed in an included file");
        } else if (sh_.seen_section || sh_.seen_include) {
            err(w.loc, "'.WIDTH' must appear before any section or .INCLUDE");
        } else if (sh_.prog.width) {
            err(w.loc, "duplicate '.WIDTH'")
                .notes.push_back(Note{sh_.prog.width->loc, "first '.WIDTH' is here"});
        } else if (n.dec < 10 || n.dec > 39) {
            err(n.loc, "width must be between 10 and 39");
        } else {
            sh_.prog.width = Width{static_cast<unsigned>(n.dec), w.loc};
        }
    }

    void handle_include() {
        const std::uint32_t before_line = prev_line_;
        const Token inc = tok();
        bump();
        if (braced_) {
            err(inc.loc, "'.INCLUDE' is not allowed inside braces");
            skip_line();
            return;
        }
        sh_.seen_include = true;
        const Token s = tok();
        if (s.kind != Tok::String) {
            err(s.loc, "'.INCLUDE' needs a string path, found " + found(s));
            skip_line();
            return;
        }
        bump();
        close_block();
        if (s.unterminated) {
            return; // the lexer reported it; do not open a file named by a broken literal
        }
        const bool alone = (before_line == 0 || before_line < inc.loc.line) &&
                           (tok().kind == Tok::Eof || tok().loc.line > s.loc.line);
        if (!alone) {
            err(inc.loc, "'.INCLUDE' must be on a line of its own");
        }
        if (s.text.empty() || s.text.find('\0') != std::string::npos) {
            err(s.loc, "invalid include path");
            return;
        }
        // The path as written, relative to the including file's directory.
        include_file((dir_ / s.text).generic_string(), s.loc, inc.loc);
    }

    void include_file(const std::string& path, SourceLoc at, SourceLoc site) {
        const ParseLimits& lim = sh_.limits;
        if (sh_.stack.size() > lim.max_include_depth) {
            err(at, "includes are nested more than " + std::to_string(lim.max_include_depth) +
                        " levels deep");
            return;
        }
        const std::string id = sh_.loader.identity(path);
        if (std::find(sh_.stack.begin(), sh_.stack.end(), id) != sh_.stack.end()) {
            err(at, "recursive include of '" + path + "'");
            return;
        }
        if (++sh_.includes > lim.max_includes) {
            sh_.limit_error(at, "more than " + std::to_string(lim.max_includes) +
                                    " includes in one program");
            return;
        }
        load_and_run(path, id, at, site);
    }

    void load_and_run(const std::string& path, const std::string& id, SourceLoc at,
                      std::optional<SourceLoc> site) {
        const std::size_t room = sh_.limits.max_source_bytes - sh_.source_bytes;
        auto text = sh_.loader.read(path, room);
        if (!text && text.error() == kSourceTooLarge) {
            sh_.limit_error(at, sh_.source_limit_message());
            return;
        }
        if (!text) {
            err(at, "cannot open '" + path + "': " + text.error());
            return;
        }
        if (text.value().size() > room) {
            sh_.limit_error(at, sh_.source_limit_message());
            return;
        }
        sh_.source_bytes += text.value().size();
        const auto fid = static_cast<std::uint32_t>(sh_.prog.files.size());
        sh_.prog.files.push_back(SourceFile{path, site});
        sh_.diags.add_file(site);
        sh_.stack.push_back(id);
        FileParser(sh_, fid, path, text.value(), site.has_value()).run();
        sh_.stack.pop_back();
    }

    // ---- words and cells ----
    static bool is_offset(const Token& t) {
        return t.kind == Tok::At || (t.kind == Tok::Directive && t.dir == Dir::Offset);
    }

    void handle_word_start() {
        const Token& t = tok();
        if (open_ && !braced_ && t.blank_before) {
            close_block();
        }
        if (!open_) {
            if (!section_) {
                report_no_section(t.loc);
                bump();
                skip_line();
                return;
            }
            open_block(false, t.loc);
        }
        if (is_offset(t)) {
            parse_offset();
        } else {
            parse_cell();
        }
    }

    void parse_offset() {
        const SourceLoc at = tok().loc;
        bump();
        const bool misplaced = !blk_.cells.empty() || blk_.offset || !pending_.empty();
        auto e = parse_expr();
        if (!e) {
            skip_line();
            return;
        }
        // `@expr {` pins the braced block that follows, also when it directly follows the last
        // line of an unbraced block (docs/asm.md 8.2).
        const bool brace_follows = !braced_ && tok().kind == Tok::LBrace && !tok().blank_before;
        if (misplaced && !brace_follows) {
            err(at, "offset directive must be at the start of a block");
            return;
        }
        if (misplaced) {
            close_block();
            open_block(true, at);
        }
        blk_.offset = Offset{std::move(*e), at};
        if (brace_follows) {
            blk_.braced = true;
            braced_ = true;
            brace_loc_ = tok().loc;
            bump();
        }
    }

    void parse_cell() {
        const bool starts_line = tok().loc.line != prev_line_ || blk_.cells.empty();
        for (;;) {
            const Token& t = tok();
            if (is_block_end(t)) {
                return;
            }
            const bool prefixed = t.kind == Tok::RPrefix || t.kind == Tok::UPrefix;
            if ((t.kind != Tok::Ident && !prefixed) || tok(1).kind != Tok::Colon) {
                break;
            }
            std::string name = t.text;
            if (prefixed) {
                name = std::string(t.kind == Tok::RPrefix ? "R_" : "U_") + name;
                err(t.loc, "label names must not start with 'R_' or 'U_'");
            } else if (is_reserved(name)) {
                err(t.loc, "'" + name + "' is a reserved word and cannot be a label");
            }
            pending_.push_back(Label{std::move(name), t.loc});
            bump();
            bump();
        }
        std::vector<Word> words;
        const bool ok =
            section_ == Section::Code ? parse_code_word(words) : parse_data_words(words);
        if (!ok) {
            skip_line();
            return;
        }
        if (!reserve_cells(words.size(), words.front().loc)) {
            return;
        }
        bool first = true;
        for (Word& w : words) {
            Cell c;
            c.labels = std::move(pending_);
            pending_.clear();
            c.word = std::move(w);
            c.starts_line = first && starts_line;
            first = false;
            blk_.cells.push_back(std::move(c));
        }
    }

    // Counts `n` more cells against the program limit; false (after the one error) when over.
    bool reserve_cells(std::size_t n, SourceLoc at) {
        if (n > sh_.limits.max_cells - sh_.cells) {
            sh_.limit_error(at, "program exceeds the limit of " +
                                    std::to_string(sh_.limits.max_cells) + " cells");
            return false;
        }
        sh_.cells += n;
        return true;
    }

    bool parse_code_word(std::vector<Word>& out) {
        Word w;
        const Token first = tok();
        w.loc = first.loc;
        if (first.kind == Tok::Ident && first.text == "RNop") {
            bump();
            if (tok().kind == Tok::Slash && !is_block_end(tok())) {
                err(first.loc, "'RNop' cannot be part of a cycle");
                return false;
            }
            w.kind = Word::Kind::RNop;
            out.push_back(std::move(w));
            return true;
        }
        for (bool more = true; more;) {
            const Token t = tok();
            std::optional<Op> op;
            if (t.kind == Tok::Ident && t.text == "RNop" && !w.ops.empty()) {
                err(t.loc, "'RNop' cannot be part of a cycle");
                return false;
            }
            if (t.kind == Tok::Ident && !is_block_end(t)) {
                op = command_of(t.text);
            }
            if (!op) {
                std::string msg = "expected a command name, found " + found(t);
                if (t.kind == Tok::Ident && !is_block_end(t) && tok(1).kind == Tok::Ident &&
                    command_of(tok(1).text)) {
                    msg += " (missing ':' after a label?)";
                }
                err(t.loc, std::move(msg));
                return false;
            }
            w.ops.push_back(*op);
            bump();
            more = tok().kind == Tok::Slash && !is_block_end(tok());
            if (more) {
                bump();
            }
        }
        w.kind = w.ops.size() == 1 ? Word::Kind::Command : Word::Kind::Cycle;
        out.push_back(std::move(w));
        return true;
    }

    bool starts_expression(const Token& t) const {
        switch (t.kind) {
        case Tok::Number:
        case Tok::Char:
        case Tok::Ident:
        case Tok::RPrefix:
        case Tok::LParen:
        case Tok::Minus:
            return !is_block_end(t);
        default:
            return false;
        }
    }

    // `?`, `?-` or an expression.
    bool parse_simple_data_word(Word& w) {
        const Token t = tok();
        w.loc = t.loc;
        if (t.kind == Tok::Question || t.kind == Tok::QuestionMinus) {
            bump();
            w.kind = t.kind == Tok::Question ? Word::Kind::Unknown : Word::Kind::Unused;
            return true;
        }
        if (t.kind == Tok::Ident && (t.text == "RNop" || command_of(t.text))) {
            std::string msg = "'" + t.text + "' is a command and is only allowed in a .CODE block";
            if (prev_kind_ == Tok::Ident && prev_line_ == t.loc.line) {
                msg += " (missing ':' after a label?)";
            }
            err(t.loc, std::move(msg));
            return false;
        }
        if (!starts_expression(t)) {
            err(t.loc, "expected a data word, found " + found(t));
            return false;
        }
        auto e = parse_expr();
        if (!e) {
            return false;
        }
        w.kind = Word::Kind::Expr;
        w.expr = Box<Expr>(std::move(*e));
        return true;
    }

    bool parse_data_words(std::vector<Word>& out) {
        const Token t = tok();
        if (t.kind == Tok::String) {
            return parse_string(out);
        }
        Word w;
        if (t.kind == Tok::UPrefix) {
            bump();
            const Token y = tok();
            if (y.kind != Tok::Ident || is_reserved(y.text) || tok(1).kind == Tok::Colon ||
                is_block_end(y)) {
                err(y.loc, "expected a label operand after 'U_" + t.text + "', found " + found(y));
                return false;
            }
            bump();
            w.kind = Word::Kind::UseRNop;
            w.loc = t.loc;
            w.use = UseRef{NameRef{t.text, t.loc}, NameRef{y.text, y.loc}};
            out.push_back(std::move(w));
            return true;
        }
        if (!parse_simple_data_word(w)) {
            return false;
        }
        out.push_back(std::move(w));
        return true;
    }

    bool parse_string(std::vector<Word>& out) {
        const Token s = tok();
        bump();
        if (s.text.empty()) {
            err(s.loc, "empty string");
            return false;
        }
        std::optional<Word> sep;
        if (tok().kind == Tok::Comma && !is_block_end(tok()) && !tok().blank_before) {
            bump();
            Word w;
            if (!parse_simple_data_word(w)) {
                return false;
            }
            sep = std::move(w);
        }
        const std::size_t count = s.text.size() * (sep ? 2 : 1) - (sep ? 1 : 0);
        if (count > sh_.limits.max_cells - sh_.cells) {
            sh_.limit_error(s.loc, "program exceeds the limit of " +
                                       std::to_string(sh_.limits.max_cells) + " cells");
            return false;
        }
        out.reserve(count);
        for (std::size_t k = 0; k < s.text.size(); ++k) {
            Word w;
            w.kind = Word::Kind::Expr;
            w.loc = s.loc;
            w.expr =
                Expr::literal(*Value::natural(static_cast<unsigned char>(s.text[k])), s.loc, true);
            out.push_back(std::move(w));
            if (sep && k + 1 < s.text.size()) {
                out.push_back(*sep);
            }
        }
        return true;
    }

    // ---- expressions (docs/asm.md 4.3) ----
    struct DepthGuard {
        unsigned& d;
        explicit DepthGuard(unsigned& depth) : d(depth) { ++d; }
        ~DepthGuard() { --d; }
    };

    std::optional<Expr> parse_expr() { return parse_level(0); }

    static std::optional<BinOp> op_at_level(Tok k, int level) {
        switch (level) {
        case 0:
            return k == Tok::Plus    ? std::optional(BinOp::Add)
                   : k == Tok::Minus ? std::optional(BinOp::Sub)
                                     : std::nullopt;
        case 1:
            return k == Tok::Shl   ? std::optional(BinOp::Shl)
                   : k == Tok::Shr ? std::optional(BinOp::Shr)
                                   : std::nullopt;
        default:
            return k == Tok::Bang ? std::optional(BinOp::Crz) : std::nullopt;
        }
    }

    std::optional<Expr> parse_level(int level) {
        if (level == 3) {
            return parse_unary();
        }
        auto lhs = parse_level(level + 1);
        if (!lhs) {
            return std::nullopt;
        }
        for (;;) {
            const Token t = tok();
            const auto op = op_at_level(t.kind, level);
            // A blank line ends an expression, also inside braces (docs/asm.md 2).
            if (!op || is_block_end(t) || t.blank_before) {
                return lhs;
            }
            bump();
            auto rhs = parse_level(level + 1);
            if (!rhs) {
                return std::nullopt;
            }
            Expr e = Expr::binary(*op, std::move(*lhs), std::move(*rhs), t.loc);
            if (e.height > kMaxExprHeight) {
                err(t.loc, kHeightMessage);
                return std::nullopt;
            }
            lhs = std::move(e);
        }
    }

    std::optional<Expr> parse_unary() {
        const Token t = tok();
        DepthGuard guard(depth_);
        if (depth_ > kMaxParseDepth) {
            err(t.loc, kDepthMessage);
            return std::nullopt;
        }
        if (t.kind == Tok::Minus && !is_block_end(t)) {
            bump();
            auto x = parse_unary();
            if (!x) {
                return std::nullopt;
            }
            Expr e = Expr::neg(std::move(*x), t.loc);
            if (e.height > kMaxExprHeight) {
                err(t.loc, kHeightMessage);
                return std::nullopt;
            }
            return e;
        }
        return parse_primary();
    }

    std::optional<Expr> parse_primary() {
        const Token t = tok();
        if (is_block_end(t)) {
            err(t.loc, "expected an expression, found " + found(t));
            return std::nullopt;
        }
        switch (t.kind) {
        case Tok::Number:
            bump();
            return Expr::literal(t.value, t.loc);
        case Tok::Char:
            bump();
            return Expr::literal(t.value, t.loc, true);
        case Tok::RPrefix:
            bump();
            return Expr::restore(t.text, t.loc);
        case Tok::Ident:
            if (t.text == "RNop" || command_of(t.text)) {
                err(t.loc, "'" + t.text + "' is a command and is only allowed in a .CODE block");
                return std::nullopt;
            }
            bump();
            if (const auto c = constant_of(t.text)) {
                return Expr::literal(*c, t.loc);
            }
            return Expr::label(t.text, t.loc);
        case Tok::LParen: {
            bump();
            auto e = parse_expr();
            if (!e) {
                return std::nullopt;
            }
            if (tok().kind != Tok::RParen || is_block_end(tok())) {
                err(tok().loc, "expected ')', found " + found(tok()));
                return std::nullopt;
            }
            bump();
            return e;
        }
        case Tok::Question:
        case Tok::QuestionMinus:
        case Tok::String:
        case Tok::UPrefix:
            err(t.loc, describe(t) + " is a whole word and cannot be part of an expression");
            return std::nullopt;
        default:
            err(t.loc, "expected an expression, found " + found(t));
            return std::nullopt;
        }
    }
};

} // namespace

std::string FileLoader::identity(const std::string& path) {
    return std::filesystem::path(path).lexically_normal().generic_string();
}

core::Result<std::string, std::string> FsLoader::read(const std::string& path,
                                                      std::size_t max_bytes) {
    using R = core::Result<std::string, std::string>;
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) {
        return R::err(ec.message());
    }
    if (std::filesystem::is_directory(status)) {
        return R::err("Is a directory");
    }
    if (!std::filesystem::is_regular_file(status)) {
        return R::err("not a regular file");
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (!ec && size > max_bytes) {
        return R::err(kSourceTooLarge);
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return R::err(std::strerror(errno));
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return R::err("read error");
    }
    if (text.size() > max_bytes) {
        return R::err(kSourceTooLarge); // the file grew while it was read
    }
    return R::ok(std::move(text));
}

std::string FsLoader::identity(const std::string& path) {
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return FileLoader::identity(path);
    }
    return canon.generic_string();
}

core::Result<std::string, std::string> MemoryLoader::read(const std::string& path,
                                                          std::size_t max_bytes) {
    using R = core::Result<std::string, std::string>;
    const auto it = files_.find(FileLoader::identity(path));
    if (it == files_.end()) {
        return R::err("no such file");
    }
    if (it->second.size() > max_bytes) {
        return R::err(kSourceTooLarge);
    }
    return R::ok(it->second);
}

Program parse_program(const std::string& path, FileLoader& loader, Diagnostics& diags,
                      const ParseLimits& limits) {
    Program prog;
    Shared sh{loader, diags, prog, limits, {}};
    FileParser::run_root(sh, path);
    return prog;
}

} // namespace malm::assembler
