#pragma once

#include <cstdint>
#include <string_view>

namespace pluma {

// Note: `>>`, `>>=` are never single tokens. The lexer emits `>` `>` (the second
// with `space_before == false`) so that nested generics like `List<List<int>>`
// close cleanly; the parser glues adjacent `>` `>` / `>` `>=` back into shifts.
#define PLUMA_TOKEN_KINDS(X)                                                     \
    /* literals and names */                                                    \
    X(Identifier) X(Keyword) X(Int) X(Float) X(String) X(Name)                 \
    /* grouping */                                                              \
    X(LeftBrace) X(RightBrace) X(LeftParen) X(RightParen)                      \
    X(LeftBracket) X(RightBracket)                                             \
    /* punctuation */                                                           \
    X(Comma) X(Dot) X(DotDot) X(DotDotEqual) X(Colon) X(ColonColon)            \
    X(Semicolon) X(At)                                                          \
    X(Question) X(QuestionDot) X(QuestionQuestion) X(Arrow) X(FatArrow)        \
    /* operators */                                                             \
    X(Plus) X(Minus) X(Star) X(Slash) X(Percent)                               \
    X(Equal) X(PlusEqual) X(MinusEqual) X(StarEqual) X(SlashEqual) X(PercentEqual) \
    X(AmpEqual) X(PipeEqual) X(CaretEqual) X(LessLessEqual)                    \
    X(EqualEqual) X(Bang) X(BangEqual)                                         \
    X(Less) X(LessEqual) X(Greater) X(GreaterEqual) X(LessLess)                \
    X(AmpAmp) X(PipePipe) X(Amp) X(Pipe) X(Caret) X(Tilde)                     \
    /* structure */                                                             \
    X(Newline) X(Eof) X(Error)

enum class TokenKind : std::uint8_t {
#define PLUMA_TOKEN_ENUM(name) name,
    PLUMA_TOKEN_KINDS(PLUMA_TOKEN_ENUM)
#undef PLUMA_TOKEN_ENUM
};

std::string_view to_string(TokenKind kind);

struct Token {
    TokenKind kind = TokenKind::Eof;
    // Source text of the token. For Error tokens, the error message instead.
    std::string_view text;
    std::uint32_t line = 1;   // 1-based
    std::uint32_t column = 1; // 1-based, in bytes
    std::uint32_t offset = 0; // byte offset of the token's first character
    std::uint32_t length = 0; // byte length in the source (also valid for Error tokens)
    // Whitespace, a comment or a line start precedes the token. Drives the `?`
    // spacing rule (LANGUAGE.md 1.6) and `>` `>` gluing.
    bool space_before = true;

    bool is(TokenKind k) const { return kind == k; }
    bool is_keyword(std::string_view word) const { return kind == TokenKind::Keyword && text == word; }
};

} // namespace pluma
