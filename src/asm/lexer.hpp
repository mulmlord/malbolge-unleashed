#pragma once

// Lexer of the assembly language (docs/asm.md 2, 4.2).
//
// The whole file is tokenised up front. Lexical errors are reported to Diagnostics and lexing
// continues: a malformed literal still yields its token (with a placeholder value), an unexpected
// character yields none, so the parser sees a stream that is as close to intended as possible.
//
// Decisions beyond the text of the spec:
//  * `//` always starts a comment, so `A//B` is not a cycle with an empty command.
//  * `?-` is lexed greedily, `?-1` is `?-` followed by `1` (and then a missing-whitespace error).
//  * `:` and `/` also separate words, as do the characters the spec lists.
//  * Non-ASCII bytes, and control characters other than whitespace, are errors everywhere.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "asm/diagnostics.hpp"
#include "core/value.hpp"

namespace malm::assembler {

enum class Tok {
    Eof,
    Ident,     // text = name (may be a command, RNop or a named constant)
    Number,    // value; decimal => dec holds the number
    Char,      // value
    String,    // text = decoded bytes
    RPrefix,   // `R_X`: text = X
    UPrefix,   // `U_X`: text = X
    Directive, // dir
    Colon,
    Comma,
    LParen,
    RParen,
    At,
    LBrace,
    RBrace,
    Slash,
    Bang,
    Shl,
    Shr,
    Plus,
    Minus,
    Question,
    QuestionMinus,
};

enum class Dir { Code, Data, Width, Offset, Include };

inline constexpr std::size_t kMaxIdentLength = 100;

struct Token {
    Tok kind = Tok::Eof;
    SourceLoc loc;
    std::string text;
    core::Value value;
    std::uint64_t dec = 0;
    bool decimal = false;
    Dir dir = Dir::Code;
    // A blank line (spec 2: empty or whitespace-only, not a comment line, not inside /* */) lies
    // between the previous token and this one.
    bool blank_before = false;
    bool unterminated = false; // a string or character literal that lacks its closing quote
};

// Incremental lexer: tokens are produced on demand, so memory stays bounded for huge inputs.
// After the end of the input every call returns an Eof token. `source` must outlive the stream.
class TokenStream {
  public:
    TokenStream(std::string_view source, std::uint32_t file, Diagnostics& diags);
    ~TokenStream();
    TokenStream(const TokenStream&) = delete;
    TokenStream& operator=(const TokenStream&) = delete;
    Token next();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// All tokens of `source`, ending with Eof.
std::vector<Token> lex(std::string_view source, std::uint32_t file, Diagnostics& diags);

// "'name'", "end of file", "number", ... for "expected X, found Y" messages.
std::string describe(const Token& t);

} // namespace malm::assembler
