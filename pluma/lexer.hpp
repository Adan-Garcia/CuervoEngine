#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "pluma/token.hpp"

namespace pluma {

// Turns source text into tokens. Tokens point into the source, which must
// outlive them.
//
// Strings are lexed as one token each, including any `{...}` interpolations
// (which may themselves contain strings); the parser decodes escapes and parses
// the interpolated expressions. Raw strings `r"..."` and multiline `"""..."""`
// strings are single tokens too. Newlines are tokens because they end
// statements; consecutive blank lines collapse into one.
// Carried between lines by Lexer::tokenize_line (editor highlighting). Only the
// constructs that can span lines need state: nested block comments and
// `"""` strings.
struct LineState {
    enum class Mode : std::uint8_t { Normal, BlockComment, MultiString };
    Mode mode = Mode::Normal;
    std::uint16_t depth = 0; // comment nesting, or `{` depth inside a string interpolation
    bool raw = false;        // MultiString: an r"""...""" string (no interpolation)
    bool operator==(const LineState&) const = default;
};

// One highlight token on a single line. `column` is a 0-based byte offset into
// the line. Comments are reported (kind is meaningless then); the whole-file
// lexer drops them. A string or comment spanning lines yields one segment per line.
struct LineToken {
    TokenKind kind = TokenKind::Eof;
    bool comment = false;
    std::uint32_t column = 0;
    std::uint32_t length = 0;
};

struct LineResult {
    std::vector<LineToken> tokens;
    LineState state; // state at the end of the line; feed it to the next line
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source), end_(source.size()) {}

    // Lex only source[begin, end), starting at the given position. Used for the
    // expressions inside string interpolations so their tokens keep real
    // offsets, lines and columns.
    Lexer(std::string_view source, std::size_t begin, std::size_t end, std::uint32_t line,
          std::uint32_t column)
        : source_(source), end_(end), pos_(begin), line_(line), column_(column) {}

    Token next();

    // Lex the whole input. The last token is always Eof.
    static std::vector<Token> tokenize(std::string_view source);
    std::vector<Token> tokenize_all();

    // Incremental, per-line lexing for highlighting. `line` is one line without
    // its terminator (a trailing "\n" / "\r\n" is tolerated). A caller that edits
    // line N re-lexes from N with the cached state of line N-1 and stops at the
    // first line whose returned `state` equals its cached one. Newline and Eof
    // tokens are not produced. Interpolations that span lines containing nested
    // `"""` strings are approximated; everything else matches `tokenize`.
    static LineResult tokenize_line(std::string_view line, LineState state = {});

private:
    char peek(std::size_t ahead = 0) const;
    char advance();
    bool match(char expected);
    bool at_end() const { return pos_ >= end_; }

    // Returns false (and sets `error`) if a block comment is unterminated.
    bool skip_whitespace_and_comments(Token& error);

    Token make(TokenKind kind) const;
    Token error(std::string_view message) const;
    Token identifier();
    Token number();
    Token string(bool raw);
    Token name();

    // Scans a string body after its opening quote(s). Returns an error message
    // or an empty view on success.
    std::string_view scan_string_body(bool raw, bool multiline);
    std::string_view scan_interpolation(bool multiline);

    std::string_view source_;
    std::size_t end_ = 0;
    std::size_t pos_ = 0;
    std::size_t start_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t column_ = 1;
    std::uint32_t start_line_ = 1;
    std::uint32_t start_column_ = 1;
    bool last_was_newline_ = true; // suppress leading newlines
    bool space_before_ = true;
};

bool is_keyword(std::string_view text);

} // namespace pluma
