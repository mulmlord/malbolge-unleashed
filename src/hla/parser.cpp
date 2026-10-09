#include "hla/parser.hpp"

#include <array>
#include <cstdio>
#include <optional>
#include <unordered_map>
#include <utility>

namespace malm::hla {
namespace {

enum class Tok {
    Ident,
    Number,
    Char,
    String,
    Comma,
    Colon,
    Equals,
    Minus,
    LBracket,
    RBracket,
    Newline,
    Eof
};

struct Token {
    Tok kind = Tok::Eof;
    SourceLoc loc;
    std::string text;       // Ident, String
    std::int64_t value = 0; // Number, Char
};

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

int digit_value(char c) {
    if (is_digit(c)) {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 99;
}

std::string show_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    if (u > 32 && u < 127) {
        return std::string(1, c);
    }
    char buf[8];
    std::snprintf(buf, sizeof buf, "\\x%02X", static_cast<unsigned>(u));
    return buf;
}

constexpr std::array<std::string_view, 17> kMnemonics = {
    "mov",  "jmp", "bz",     "bnz",  "in",    "out", "halt", "call",  "ret",
    "push", "pop", "switch", "load", "store", "reg", "ram",  "stack",
};

bool reserved(std::string_view s) {
    for (const auto m : kMnemonics) {
        if (m == s) {
            return true;
        }
    }
    return false;
}

// Immediates are 32-bit words written as signed or unsigned numbers.
constexpr std::int64_t kImmMin = -(std::int64_t{1} << 31);
constexpr std::int64_t kImmMax = (std::int64_t{1} << 32) - 1;

class Lexer {
  public:
    Lexer(std::string_view src, Diagnostics& diags) : s_(src), diags_(diags) {}

    Token next() {
        skip_blanks_and_comments();
        Token t;
        t.loc = here();
        if (i_ >= s_.size()) {
            t.kind = Tok::Eof;
            return t;
        }
        const char c = s_[i_];
        if (c == '\n') {
            adv();
            t.kind = Tok::Newline;
        } else if (is_alpha(c)) {
            t.kind = Tok::Ident;
            while (i_ < s_.size() && is_alnum(s_[i_])) {
                t.text += s_[i_];
                adv();
            }
        } else if (is_digit(c)) {
            number(t);
        } else if (c == '\'') {
            char_literal(t);
        } else if (c == '"') {
            string_literal(t);
        } else if (c == ',' || c == ':' || c == '=' || c == '-' || c == '[' || c == ']') {
            t.kind = c == ','   ? Tok::Comma
                     : c == ':' ? Tok::Colon
                     : c == '=' ? Tok::Equals
                     : c == '-' ? Tok::Minus
                     : c == '[' ? Tok::LBracket
                                : Tok::RBracket;
            adv();
        } else {
            diags_.error(t.loc, "unexpected character '" + show_char(c) + "'");
            adv();
            return next();
        }
        return t;
    }

  private:
    SourceLoc here() const { return SourceLoc{0, line_, col_}; }
    void adv() {
        if (s_[i_] == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        ++i_;
    }
    char peek(std::size_t k = 1) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }

    void skip_blanks_and_comments() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                adv();
            } else if (c == ';' || c == '#' || (c == '/' && peek() == '/')) {
                while (i_ < s_.size() && s_[i_] != '\n') {
                    adv();
                }
            } else {
                return;
            }
        }
    }

    void number(Token& t) {
        t.kind = Tok::Number;
        int base = 10;
        if (s_[i_] == '0' && (peek() == 'x' || peek() == 'X' || peek() == 'b' || peek() == 'B')) {
            base = peek() == 'x' || peek() == 'X' ? 16 : 2;
            adv();
            adv();
        }
        std::string digits;
        while (i_ < s_.size() && is_alnum(s_[i_])) {
            digits += s_[i_];
            adv();
        }
        bool ok = !digits.empty();
        std::int64_t v = 0;
        for (const char d : digits) {
            const int x = digit_value(d);
            if (x >= base) {
                ok = false;
                break;
            }
            v = v * base + x;
            if (v > kImmMax) {
                diags_.error(t.loc, "number out of range (a 32-bit word)");
                return;
            }
        }
        if (!ok) {
            diags_.error(t.loc, "malformed number");
        }
        t.value = v;
    }

    // One character of a char or string literal, escapes decoded; nullopt after an error.
    std::optional<char> literal_char() {
        const SourceLoc loc = here();
        const char c = s_[i_];
        if (c != '\\') {
            if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) {
                diags_.error(loc, "character '" + show_char(c) + "' in a literal; use an escape");
                adv();
                return std::nullopt;
            }
            adv();
            return c;
        }
        adv();
        const char e = i_ < s_.size() ? s_[i_] : '\0';
        switch (e) {
        case 'n':
            adv();
            return '\n';
        case 'r':
            adv();
            return '\r';
        case 't':
            adv();
            return '\t';
        case '0':
            adv();
            return '\0';
        case '\\':
            adv();
            return '\\';
        case '\'':
            adv();
            return '\'';
        case '"':
            adv();
            return '"';
        case 'x': {
            adv();
            const int hi = digit_value(i_ < s_.size() ? s_[i_] : '\0');
            const int lo = digit_value(peek());
            if (hi < 16 && lo < 16) {
                adv();
                adv();
                return static_cast<char>(hi * 16 + lo);
            }
            break;
        }
        default:
            break;
        }
        diags_.error(loc, "invalid escape in a literal");
        if (i_ < s_.size() && s_[i_] != '\n') {
            adv();
        }
        return std::nullopt;
    }

    void char_literal(Token& t) {
        t.kind = Tok::Char;
        adv();
        if (i_ >= s_.size() || s_[i_] == '\n' || s_[i_] == '\'') {
            diags_.error(t.loc, "empty or unterminated character literal");
            return;
        }
        const auto c = literal_char();
        t.value = c ? static_cast<unsigned char>(*c) : 0;
        if (i_ >= s_.size() || s_[i_] != '\'') {
            diags_.error(t.loc, "unterminated character literal");
            while (i_ < s_.size() && s_[i_] != '\n') {
                adv();
            }
            return;
        }
        adv();
    }

    void string_literal(Token& t) {
        t.kind = Tok::String;
        adv();
        while (i_ < s_.size() && s_[i_] != '"' && s_[i_] != '\n') {
            if (const auto c = literal_char()) {
                t.text += *c;
            }
        }
        if (i_ >= s_.size() || s_[i_] != '"') {
            diags_.error(t.loc, "unterminated string");
            return;
        }
        adv();
        if (t.text.empty()) {
            diags_.error(t.loc, "empty string");
        }
    }

    std::string_view s_;
    Diagnostics& diags_;
    std::size_t i_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t col_ = 1;
};

std::string describe(const Token& t) {
    switch (t.kind) {
    case Tok::Ident:
        return "'" + t.text + "'";
    case Tok::Number:
        return "number";
    case Tok::Char:
        return "character literal";
    case Tok::String:
        return "string";
    case Tok::Comma:
        return "','";
    case Tok::Colon:
        return "':'";
    case Tok::Equals:
        return "'='";
    case Tok::Minus:
        return "'-'";
    case Tok::LBracket:
        return "'['";
    case Tok::RBracket:
        return "']'";
    case Tok::Newline:
        return "end of line";
    case Tok::Eof:
        return "end of file";
    }
    return "token";
}

struct SyntaxError {};

class Parser {
  public:
    Parser(std::string_view src, Diagnostics& diags) : lex_(src, diags), diags_(diags) {
        tok_ = lex_.next();
    }

    void run(Program& p) {
        while (tok_.kind != Tok::Eof) {
            try {
                line(p);
            } catch (const SyntaxError&) {
                while (tok_.kind != Tok::Newline && tok_.kind != Tok::Eof) {
                    tok_ = lex_.next();
                }
            }
            if (tok_.kind == Tok::Newline) {
                tok_ = lex_.next();
            }
        }
    }

  private:
    [[noreturn]] void fail(const SourceLoc& loc, std::string msg) {
        diags_.error(loc, std::move(msg));
        throw SyntaxError{};
    }
    [[noreturn]] void expected(std::string_view what) {
        fail(tok_.loc, "expected " + std::string(what) + ", found " + describe(tok_));
    }
    Token take() {
        Token t = std::move(tok_);
        tok_ = lex_.next();
        return t;
    }
    bool at_end() const { return tok_.kind == Tok::Newline || tok_.kind == Tok::Eof; }

    std::string ident(const Token& t) {
        if (t.text.size() > kMaxIdentLength) {
            fail(t.loc,
                 "identifier longer than " + std::to_string(kMaxIdentLength) + " characters");
        }
        return t.text;
    }

    void line(Program& p) {
        while (tok_.kind == Tok::Ident) {
            Token t = take();
            if (tok_.kind == Tok::Colon) {
                take();
                if (reserved(t.text)) {
                    fail(t.loc, "'" + t.text + "' is reserved and cannot be a label");
                }
                p.stmts.push_back(Stmt{Op::Label, t.loc, ident(t), {}});
                continue;
            }
            statement(p, t);
            break;
        }
        if (!at_end()) {
            expected("a statement");
        }
    }

    void statement(Program& p, const Token& m) {
        if (m.text == "reg") {
            declarations(p);
            return;
        }
        if (m.text == "ram") {
            ram(p);
            return;
        }
        if (m.text == "stack") {
            stack(p, m);
            return;
        }
        static const std::unordered_map<std::string_view, Op> ops = {
            {"mov", Op::Mov},   {"jmp", Op::Jmp},     {"bz", Op::Bz},     {"bnz", Op::Bnz},
            {"in", Op::In},     {"out", Op::Out},     {"halt", Op::Halt}, {"call", Op::Call},
            {"ret", Op::Ret},   {"push", Op::Push},   {"pop", Op::Pop},   {"switch", Op::Switch},
            {"load", Op::Load}, {"store", Op::Store},
        };
        const auto it = ops.find(m.text);
        if (it == ops.end()) {
            fail(m.loc, "unknown instruction '" + m.text + "'");
        }
        Stmt s{it->second, m.loc, {}, {}};
        if (!at_end()) {
            s.args.push_back(operand());
            while (tok_.kind == Tok::Comma) {
                take();
                s.args.push_back(operand());
            }
        }
        if (!at_end()) {
            expected("',' or end of line");
        }
        check_shape(s);
        p.stmts.push_back(std::move(s));
    }

    void declarations(Program& p) {
        while (true) {
            if (tok_.kind != Tok::Ident) {
                expected("a register name");
            }
            const Token t = take();
            if (reserved(t.text)) {
                fail(t.loc, "'" + t.text + "' is reserved and cannot be a register");
            }
            RegDecl r{ident(t), {}, t.loc};
            r.init.loc = t.loc;
            if (tok_.kind == Tok::Equals) {
                take();
                r.init = value("a register's initial value");
            }
            p.regs.push_back(std::move(r));
            if (tok_.kind != Tok::Comma) {
                return;
            }
            take();
        }
    }

    // `ram name[size] [= value, ...]`; regions follow each other from byte address 0.
    void ram(Program& p) {
        if (tok_.kind != Tok::Ident) {
            expected("a RAM region name");
        }
        const Token t = take();
        if (reserved(t.text)) {
            fail(t.loc, "'" + t.text + "' is reserved and cannot be a RAM region");
        }
        RamDecl r{ident(t), 0, ram_words_ * 4, {}, t.loc};
        if (tok_.kind != Tok::LBracket) {
            expected("'['");
        }
        take();
        if (tok_.kind != Tok::Number) {
            expected("the size in words");
        }
        const Token size = take();
        if (size.value < 1 || size.value > kMaxRamWords - ram_words_) {
            fail(size.loc,
                 "RAM regions hold 1 to " + std::to_string(kMaxRamWords) + " words in total");
        }
        r.size = static_cast<std::uint32_t>(size.value);
        if (tok_.kind != Tok::RBracket) {
            expected("']'");
        }
        take();
        if (tok_.kind == Tok::Equals) {
            take();
            while (true) {
                r.init.push_back(value("a RAM word's initial value"));
                if (tok_.kind != Tok::Comma) {
                    break;
                }
                take();
            }
            if (r.init.size() > r.size) {
                fail(r.init[r.size].loc, "more initial values than words in '" + r.name + "'");
            }
        }
        if (!at_end()) {
            expected("'=', ',' or end of line");
        }
        ram_words_ += r.size;
        p.rams.push_back(std::move(r));
    }

    void stack(Program& p, const Token& m) {
        if (tok_.kind != Tok::Number) {
            expected("the stack size in words");
        }
        const Token size = take();
        if (size.value < 1 || size.value > kMaxStackFrames) {
            fail(size.loc, "the stack holds 1 to " + std::to_string(kMaxStackFrames) + " words");
        }
        if (p.stack) {
            diags_.error(m.loc, "the stack size is already declared")
                .notes.push_back({p.stack->loc, "declared here"});
            throw SyntaxError{};
        }
        p.stack = StackDecl{static_cast<std::uint32_t>(size.value), m.loc};
    }

    // A value: a number, a label or a RAM region (names are checked later).
    Operand value(std::string_view what) {
        const Operand o = operand();
        if (o.kind == Operand::Kind::String || o.mem) {
            fail(o.loc, std::string(what) + " must be a number, a label or a RAM region");
        }
        return o;
    }

    Operand operand() {
        if (tok_.kind == Tok::LBracket) {
            const SourceLoc loc = take().loc;
            if (tok_.kind == Tok::LBracket || tok_.kind == Tok::String) {
                expected("a register, a RAM region or an address");
            }
            Operand o = operand();
            o.loc = loc;
            o.mem = true;
            if (tok_.kind != Tok::RBracket) {
                expected("']'");
            }
            take();
            return o;
        }
        Operand o;
        o.loc = tok_.loc;
        if (tok_.kind == Tok::Ident) {
            o.kind = Operand::Kind::Name;
            o.name = ident(take());
            return o;
        }
        if (tok_.kind == Tok::String) {
            o.kind = Operand::Kind::String;
            o.text = take().text;
            return o;
        }
        bool neg = false;
        if (tok_.kind == Tok::Minus) {
            take();
            neg = true;
        }
        if (tok_.kind != Tok::Number && tok_.kind != Tok::Char) {
            expected("an operand");
        }
        o.kind = Operand::Kind::Imm;
        o.imm = neg ? -take().value : take().value;
        if (o.imm < kImmMin) {
            fail(o.loc, "number out of range (a 32-bit word)");
        }
        return o;
    }

    // The operand kinds of docs/hla.md 3; names are resolved later.
    void check_shape(const Stmt& s) {
        using K = Operand::Kind;
        const auto n = s.args.size();
        const auto kind = [&](std::size_t i) { return s.args[i].kind; };
        const auto plain = [&](std::size_t i) { return !s.args[i].mem; };
        const auto reg = [&](std::size_t i) { return kind(i) == K::Name && plain(i); };
        const auto val = [&](std::size_t i) { return kind(i) != K::String && plain(i); };
        const auto want = [&](bool ok, std::string_view form) {
            if (!ok) {
                fail(s.loc, "expected " + std::string(form));
            }
        };
        switch (s.op) {
        case Op::Mov:
            want(n == 2 && reg(0) && val(1), "'mov <reg>, <reg or value>'");
            break;
        case Op::Jmp:
            want(n == 1 && reg(0), "'jmp <label>' or 'jmp <reg>'");
            break;
        case Op::Call:
            want(n == 1 && reg(0), "'call <label>' or 'call <reg>'");
            break;
        case Op::Ret:
            want(n == 0, "'ret'");
            break;
        case Op::Push:
            want(n == 1 && val(0), "'push <reg or value>'");
            break;
        case Op::Pop:
            want(n == 1 && reg(0), "'pop <reg>'");
            break;
        case Op::Switch: {
            bool ok = n >= 2;
            for (std::size_t i = 0; i < n && ok; ++i) {
                ok = reg(i);
            }
            want(ok, "'switch <reg>, <label>, ...'");
            if (n - 1 > kMaxSwitchLabels) {
                fail(s.args[kMaxSwitchLabels + 1].loc,
                     "a switch has at most " + std::to_string(kMaxSwitchLabels) + " labels");
            }
            break;
        }
        case Op::Load:
            want(n == 2 && reg(0) && kind(1) != K::String && !plain(1),
                 "'load <reg>, [<address>]'");
            break;
        case Op::Store:
            want(n == 2 && kind(0) != K::String && !plain(0) && val(1),
                 "'store [<address>], <reg or value>'");
            break;
        case Op::Bz:
        case Op::Bnz:
            want(n == 2 && reg(0) && reg(1),
                 s.op == Op::Bz ? "'bz <reg>, <label>'" : "'bnz <reg>, <label>'");
            break;
        case Op::In:
            want((n == 1 || n == 2) && reg(0) && (n == 1 || reg(1)),
                 "'in <reg>' or 'in <reg>, <label>'");
            break;
        case Op::Out:
            want(n == 1 && plain(0), "'out <reg>', 'out <byte>' or 'out \"text\"'");
            break;
        case Op::Halt:
            want(n == 0 || (n == 1 && val(0)), "'halt', 'halt <byte>' or 'halt <reg>'");
            break;
        case Op::Label:
            break;
        }
        if ((s.op == Op::Out || s.op == Op::Halt) && n == 1 && kind(0) == K::Imm &&
            (s.args[0].imm < 0 || s.args[0].imm > 255)) {
            fail(s.args[0].loc, "a byte must be 0..255");
        }
    }

    Lexer lex_;
    Diagnostics& diags_;
    Token tok_;
    std::uint32_t ram_words_ = 0;
};

// Names (docs/hla.md 2): registers, labels and RAM regions share one namespace; every operand name
// must be defined, as a kind its position allows.
enum Kind : unsigned { kReg = 1, kLabel = 2, kRam = 4 };

std::string kind_word(unsigned kinds) {
    switch (kinds) {
    case kReg:
        return "register";
    case kLabel:
        return "label";
    case kRam:
        return "RAM region";
    default:
        return "name";
    }
}

std::string kind_noun(unsigned kinds) {
    switch (kinds) {
    case kReg:
        return "a register";
    case kLabel:
        return "a label";
    case kRam:
        return "a RAM region";
    case kReg | kLabel:
        return "a register or a label";
    case kReg | kRam:
        return "a register or a RAM region";
    case kLabel | kRam:
        return "a label or a RAM region";
    default:
        return "a name";
    }
}

// The kinds operand i of `s` may name.
unsigned allowed(const Stmt& s, std::size_t i) {
    switch (s.op) {
    case Op::Mov:
    case Op::Push:
        return i == 0 && s.op == Op::Mov ? kReg : kReg | kLabel | kRam;
    case Op::Jmp:
    case Op::Call:
        return kReg | kLabel;
    case Op::Bz:
    case Op::Bnz:
    case Op::In:
    case Op::Switch:
        return i == 0 ? kReg : kLabel;
    case Op::Load:
        return i == 0 ? kReg : kReg | kRam;
    case Op::Store:
        return i == 0 ? kReg | kRam : kReg | kLabel | kRam;
    default:
        return kReg;
    }
}

void check_names(const Program& p, Diagnostics& diags) {
    struct Def {
        Kind kind;
        SourceLoc loc;
    };
    std::unordered_map<std::string, Def> defs;
    const auto define = [&](const std::string& name, Kind kind, const SourceLoc& loc) {
        const auto [it, fresh] = defs.emplace(name, Def{kind, loc});
        if (!fresh) {
            diags.error(loc, "'" + name + "' is already defined")
                .notes.push_back({it->second.loc, "first defined here"});
        }
    };
    for (const RegDecl& r : p.regs) {
        define(r.name, kReg, r.loc);
    }
    for (const RamDecl& r : p.rams) {
        define(r.name, kRam, r.loc);
    }
    for (const Stmt& s : p.stmts) {
        if (s.op == Op::Label) {
            define(s.name, kLabel, s.loc);
        }
    }
    const auto check = [&](const Operand& o, unsigned kinds) {
        if (o.kind != Operand::Kind::Name) {
            return;
        }
        const auto it = defs.find(o.name);
        if (it == defs.end()) {
            // A jump or call target is most likely a label.
            diags.error(o.loc, "unknown " + kind_word(kinds == (kReg | kLabel) ? kLabel : kinds) +
                                   " '" + o.name + "'");
        } else if (!(it->second.kind & kinds)) {
            diags
                .error(o.loc, "'" + o.name + "' is " + kind_noun(it->second.kind) + ", not " +
                                  kind_noun(kinds))
                .notes.push_back({it->second.loc, "defined here"});
        }
    };
    for (const RegDecl& r : p.regs) {
        check(r.init, kLabel | kRam);
    }
    for (const RamDecl& r : p.rams) {
        for (const Operand& o : r.init) {
            check(o, kLabel | kRam);
        }
    }
    for (const Stmt& s : p.stmts) {
        for (std::size_t i = 0; i < s.args.size(); ++i) {
            check(s.args[i], allowed(s, i));
        }
    }
    // Constant addresses: a word of a declared region.
    for (const Stmt& s : p.stmts) {
        for (const Operand& o : s.args) {
            if (!o.mem || o.kind != Operand::Kind::Imm) {
                continue;
            }
            const std::uint32_t a = imm_word(o.imm);
            bool inside = false;
            for (const RamDecl& r : p.rams) {
                inside = inside || (a >= r.base && (a - r.base) / 4 < r.size);
            }
            if (a % 4 != 0 || !inside) {
                diags.error(o.loc, "the address " + std::to_string(a) +
                                       " is not a word of a RAM region (a multiple of 4 inside a "
                                       "'ram' declaration)");
            }
        }
    }
}

} // namespace

Program parse(std::string_view source, const std::string& path, Diagnostics& diags) {
    Program p;
    p.files.push_back(SourceFile{path, std::nullopt});
    diags.add_file(std::nullopt);
    Parser(source, diags).run(p);
    if (!diags.has_errors()) {
        check_names(p, diags);
    }
    return p;
}

Program parse_file(const std::string& path, assembler::FileLoader& loader, Diagnostics& diags) {
    auto text = loader.read(path, kMaxSourceBytes);
    if (!text) {
        Program p;
        p.files.push_back(SourceFile{path, std::nullopt});
        diags.add_file(std::nullopt);
        const std::string why = text.error() == assembler::kSourceTooLarge
                                    ? "source larger than 64 MiB"
                                    : "cannot open '" + path + "': " + text.error();
        diags.error(SourceLoc{0, 1, 1}, why);
        return p;
    }
    return parse(text.value(), path, diags);
}

} // namespace malm::hla
