#include "asm/lexer.hpp"

#include <array>
#include <cstdio>
#include <deque>
#include <optional>
#include <utility>

namespace malm::assembler {
namespace {

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }
bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

// Characters that start a word; a word may not be followed directly by one (spec 2).
bool starts_word(char c) { return is_alnum(c) || c == '\'' || c == '"' || c == '.' || c == '?'; }

std::string show_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 32 && u < 127) {
        return std::string(1, c);
    }
    char buf[8];
    std::snprintf(buf, sizeof buf, "\\x%02X", static_cast<unsigned>(u));
    return buf;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

class Lexer {
  public:
    Lexer(std::string_view src, std::uint32_t file, Diagnostics& diags)
        : s_(src), file_(file), diags_(diags) {}

    Token next() {
        while (out_.empty()) {
            skip_trivia();
            if (i_ >= s_.size()) {
                out_.push_back(make(Tok::Eof, here()));
                break;
            }
            lex_token();
        }
        Token t = std::move(out_.front());
        out_.pop_front();
        return t;
    }

  private:
    std::string_view s_;
    std::uint32_t file_;
    Diagnostics& diags_;
    std::size_t i_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t col_ = 1;
    bool line_has_content_ = false;
    bool blank_seen_ = false;
    std::deque<Token> out_;

    SourceLoc here() const { return SourceLoc{file_, line_, col_}; }
    bool eof() const { return i_ >= s_.size(); }
    char cur() const { return i_ < s_.size() ? s_[i_] : '\0'; }
    char peek(std::size_t k = 1) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }

    void adv() {
        if (i_ >= s_.size()) {
            return;
        }
        if (s_[i_] == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        ++i_;
    }

    void err(SourceLoc loc, std::string msg) { diags_.error(loc, std::move(msg)); }

    Token make(Tok kind, SourceLoc loc) {
        Token t;
        t.kind = kind;
        t.loc = loc;
        t.blank_before = blank_seen_;
        blank_seen_ = false;
        line_has_content_ = true;
        return t;
    }

    void push(Tok kind, SourceLoc loc, std::string text = {}) {
        Token t = make(kind, loc);
        t.text = std::move(text);
        out_.push_back(std::move(t));
    }

    void skip_trivia() {
        while (!eof()) {
            const char c = cur();
            if (c == '\n') {
                if (!line_has_content_) {
                    blank_seen_ = true;
                }
                adv();
                line_has_content_ = false;
            } else if (is_blank(c)) {
                adv();
            } else if (c == ';' || c == '#' || (c == '/' && peek() == '/')) {
                line_has_content_ = true;
                while (!eof() && cur() != '\n') {
                    adv();
                }
            } else if (c == '/' && peek() == '*') {
                line_has_content_ = true; // every line a block comment touches is non-blank
                const SourceLoc start = here();
                adv();
                adv();
                bool closed = false;
                while (!eof()) {
                    if (cur() == '*' && peek() == '/') {
                        adv();
                        adv();
                        closed = true;
                        break;
                    }
                    adv();
                }
                if (!closed) {
                    err(start, "unterminated block comment");
                }
            } else {
                break;
            }
        }
    }

    // After a word-like token: the next character must not start another word.
    void check_separated() {
        if (!eof() && starts_word(cur())) {
            err(here(), "missing whitespace between tokens");
        }
    }

    void lex_token() {
        const SourceLoc loc = here();
        const char c = cur();
        if (is_alpha(c)) {
            lex_ident(loc);
        } else if (is_digit(c)) {
            lex_number(loc);
        } else if (c == '.') {
            if (peek() == '.' && peek(2) == '.') {
                lex_value_literal(loc);
            } else {
                lex_directive(loc);
            }
        } else if (c == '\'') {
            lex_char(loc);
        } else if (c == '"') {
            lex_string(loc);
        } else if (c == '?') {
            adv();
            if (cur() == '-') {
                adv();
                push(Tok::QuestionMinus, loc);
            } else {
                push(Tok::Question, loc);
            }
            check_separated();
        } else if (c == '<' && peek() == '<') {
            adv();
            adv();
            push(Tok::Shl, loc);
        } else if (c == '>' && peek() == '>') {
            adv();
            adv();
            push(Tok::Shr, loc);
        } else {
            const std::optional<Tok> single = [&]() -> std::optional<Tok> {
                switch (c) {
                case ':':
                    return Tok::Colon;
                case ',':
                    return Tok::Comma;
                case '(':
                    return Tok::LParen;
                case ')':
                    return Tok::RParen;
                case '@':
                    return Tok::At;
                case '{':
                    return Tok::LBrace;
                case '}':
                    return Tok::RBrace;
                case '/':
                    return Tok::Slash;
                case '!':
                    return Tok::Bang;
                case '+':
                    return Tok::Plus;
                case '-':
                    return Tok::Minus;
                default:
                    return std::nullopt;
                }
            }();
            adv();
            if (single) {
                push(*single, loc);
            } else {
                err(loc, "unexpected character '" + show_char(c) + "'");
            }
        }
    }

    void lex_ident(SourceLoc loc) {
        const std::size_t start = i_;
        while (!eof() && is_alnum(cur())) {
            adv();
        }
        std::string text(s_.substr(start, i_ - start));
        Tok kind = Tok::Ident;
        std::string name = text;
        if (text.size() >= 2 && (text[0] == 'R' || text[0] == 'U') && text[1] == '_') {
            kind = text[0] == 'R' ? Tok::RPrefix : Tok::UPrefix;
            name = text.substr(2);
            if (name.empty() || is_digit(name[0])) {
                err(loc, "'" + text.substr(0, 2) + "' must be followed by a label name");
            }
        }
        if (name.size() > kMaxIdentLength) {
            err(loc, "identifier longer than 100 characters");
        }
        push(kind, loc, std::move(name));
        check_separated();
    }

    void push_number(SourceLoc loc, core::Value v, std::string text, bool decimal = false,
                     std::uint64_t dec = 0) {
        Token t = make(Tok::Number, loc);
        t.value = v;
        t.text = std::move(text);
        t.decimal = decimal;
        t.dec = dec;
        out_.push_back(std::move(t));
        check_separated();
    }

    // `digits` follow the notation prefix; head is 0 for `0t`, else the head digit.
    void finish_trit_literal(SourceLoc loc, unsigned head, std::string_view digits,
                             std::string spelling) {
        if (digits.empty()) {
            err(loc, "trit literal needs at least one digit");
            push_number(loc, core::Value{}, std::move(spelling));
            return;
        }
        for (const char d : digits) {
            if (d < '0' || d > '2') {
                err(loc, "invalid trit digit '" + show_char(d) + "' in literal");
                push_number(loc, core::Value{}, std::move(spelling));
                return;
            }
        }
        const std::string text = head == 0 ? "0t" + std::string(digits)
                                           : "..." + std::to_string(head) + std::string(digits);
        const auto v = core::Value::parse(text);
        if (!v) {
            err(loc, "literal wider than 39 trits");
        }
        push_number(loc, v.value_or(core::Value{}), std::move(spelling));
    }

    void lex_number(SourceLoc loc) {
        const std::size_t start = i_;
        const char first = cur();
        if (first >= '0' && first <= '2' && peek() == 't') {
            adv();
            adv();
            const std::size_t ds = i_;
            while (!eof() && is_alnum(cur())) {
                adv();
            }
            finish_trit_literal(loc, static_cast<unsigned>(first - '0'), s_.substr(ds, i_ - ds),
                                std::string(s_.substr(start, i_ - start)));
            return;
        }
        std::uint64_t v = 0;
        bool too_big = false;
        while (!eof() && is_digit(cur())) {
            const auto d = static_cast<std::uint64_t>(cur() - '0');
            if (v > (core::pow3[core::kMaxWidth] - d) / 10) {
                too_big = true;
            } else {
                v = v * 10 + d;
            }
            adv();
        }
        const std::string spelling(s_.substr(start, i_ - start));
        const auto val = too_big ? std::nullopt : core::Value::natural(v);
        if (!val) {
            err(loc, "decimal literal too large (the limit is 3^39 - 1)");
        }
        push_number(loc, val.value_or(core::Value{}), spelling, true, val ? v : 0);
    }

    void lex_value_literal(SourceLoc loc) {
        const std::size_t start = i_;
        adv();
        adv();
        adv();
        if (cur() < '0' || cur() > '2') {
            err(loc, "trit literal needs at least one digit");
            while (!eof() && is_alnum(cur())) {
                adv(); // the malformed rest belongs to this literal
            }
            push_number(loc, core::Value{}, "...");
            return;
        }
        const char head = cur();
        adv();
        const std::size_t ds = i_;
        while (!eof() && is_alnum(cur())) {
            adv();
        }
        const std::string_view digits = s_.substr(ds, i_ - ds);
        const auto h = static_cast<unsigned>(head - '0');
        if (digits.empty()) {
            // `...h` alone: the value with no low trits.
            push_number(loc, *core::Value::parse(std::string("...") + head),
                        std::string(s_.substr(start, i_ - start)));
            return;
        }
        finish_trit_literal(loc, h, digits, std::string(s_.substr(start, i_ - start)));
    }

    void lex_directive(SourceLoc loc) {
        const std::size_t start = i_;
        adv();
        while (!eof() && is_alnum(cur())) {
            adv();
        }
        const std::string text(s_.substr(start, i_ - start));
        static constexpr std::array<std::pair<std::string_view, Dir>, 5> kDirs{{
            {".CODE", Dir::Code},
            {".DATA", Dir::Data},
            {".WIDTH", Dir::Width},
            {".OFFSET", Dir::Offset},
            {".INCLUDE", Dir::Include},
        }};
        for (const auto& [name, dir] : kDirs) {
            if (text == name) {
                Token t = make(Tok::Directive, loc);
                t.dir = dir;
                t.text = text;
                out_.push_back(std::move(t));
                check_separated();
                return;
            }
        }
        err(loc, "unknown directive '" + text + "'");
    }

    // Reads the body of a char or string literal up to the closing `quote`; the opening quote is
    // already consumed. Returns the decoded bytes; closed says whether the quote was found.
    std::string lex_literal_body(char quote, bool& closed) {
        std::string bytes;
        closed = false;
        while (!eof() && cur() != '\n') {
            const char c = cur();
            if (c == quote) {
                adv();
                closed = true;
                break;
            }
            if (c == '\\') {
                const SourceLoc eloc = here();
                adv();
                if (eof() || cur() == '\n') {
                    break;
                }
                const char e = cur();
                adv();
                switch (e) {
                case 'n':
                    bytes += '\n';
                    break;
                case 'r':
                    bytes += '\r';
                    break;
                case 't':
                    bytes += '\t';
                    break;
                case '0':
                    bytes += '\0';
                    break;
                case '\\':
                case '\'':
                case '"':
                    bytes += e;
                    break;
                case 'x': {
                    const int h = hex_value(cur());
                    const int l = hex_value(peek());
                    if (h < 0 || l < 0) {
                        err(eloc, "invalid \\x escape: two hexadecimal digits expected");
                        for (int k = 0; k < 2 && is_alnum(cur()); ++k) {
                            adv(); // the malformed digits belong to the escape
                        }
                        bytes += '\0';
                    } else {
                        adv();
                        adv();
                        bytes += static_cast<char>(h * 16 + l);
                    }
                    break;
                }
                default:
                    err(eloc, "unknown escape sequence '\\" + show_char(e) + "'");
                    bytes += '\0';
                    break;
                }
                continue;
            }
            const auto u = static_cast<unsigned char>(c);
            if (u < 32 || u >= 127) {
                err(here(), "control or non-ASCII character in literal; use an escape");
            }
            bytes += c;
            adv();
        }
        return bytes;
    }

    void lex_char(SourceLoc loc) {
        adv();
        bool closed = false;
        const std::string bytes = lex_literal_body('\'', closed);
        if (!closed) {
            err(loc, "unterminated character literal");
        } else if (bytes.empty()) {
            err(loc, "empty character literal");
        } else if (bytes.size() > 1) {
            err(loc, "character literal must contain exactly one character");
        }
        const auto v = core::Value::natural(
            bytes.empty() ? 0 : static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[0])));
        Token t = make(Tok::Char, loc);
        t.unterminated = !closed;
        t.value = *v;
        out_.push_back(std::move(t));
        check_separated();
    }

    void lex_string(SourceLoc loc) {
        adv();
        bool closed = false;
        std::string bytes = lex_literal_body('"', closed);
        if (!closed) {
            err(loc, "unterminated string literal");
        }
        Token t = make(Tok::String, loc);
        t.text = std::move(bytes);
        t.unterminated = !closed;
        out_.push_back(std::move(t));
        check_separated();
    }
};

} // namespace

struct TokenStream::Impl {
    Lexer lexer;
    Impl(std::string_view s, std::uint32_t f, Diagnostics& d) : lexer(s, f, d) {}
};

TokenStream::TokenStream(std::string_view source, std::uint32_t file, Diagnostics& diags)
    : impl_(std::make_unique<Impl>(source, file, diags)) {}
TokenStream::~TokenStream() = default;
Token TokenStream::next() { return impl_->lexer.next(); }

std::vector<Token> lex(std::string_view source, std::uint32_t file, Diagnostics& diags) {
    TokenStream ts(source, file, diags);
    std::vector<Token> out;
    do {
        out.push_back(ts.next());
    } while (out.back().kind != Tok::Eof);
    return out;
}

std::string describe(const Token& t) {
    switch (t.kind) {
    case Tok::Eof:
        return "end of file";
    case Tok::Ident:
        return "'" + t.text + "'";
    case Tok::Number:
        return "number";
    case Tok::Char:
        return "character literal";
    case Tok::String:
        return "string";
    case Tok::RPrefix:
        return "'R_" + t.text + "'";
    case Tok::UPrefix:
        return "'U_" + t.text + "'";
    case Tok::Directive:
        return "'" + t.text + "'";
    case Tok::Colon:
        return "':'";
    case Tok::Comma:
        return "','";
    case Tok::LParen:
        return "'('";
    case Tok::RParen:
        return "')'";
    case Tok::At:
        return "'@'";
    case Tok::LBrace:
        return "'{'";
    case Tok::RBrace:
        return "'}'";
    case Tok::Slash:
        return "'/'";
    case Tok::Bang:
        return "'!'";
    case Tok::Shl:
        return "'<<'";
    case Tok::Shr:
        return "'>>'";
    case Tok::Plus:
        return "'+'";
    case Tok::Minus:
        return "'-'";
    case Tok::Question:
        return "'?'";
    case Tok::QuestionMinus:
        return "'?-'";
    }
    return "token";
}

} // namespace malm::assembler
