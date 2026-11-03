#include "pluma/lexer.hpp"

#include <algorithm>
#include <iterator>

namespace pluma {

namespace {

// LANGUAGE.md 1.4.
constexpr std::string_view keywords[] = {
    "alloc",  "as",     "await",  "break",   "class",   "const",  "continue", "defer",
    "else",   "emit",   "enum",   "extends", "false",   "final",  "fn",       "for",
    "foreach", "free",  "get",    "if",      "import",  "in",      "is",     "loop",     "manual",
    "match",  "new",    "null",   "pub",     "return",  "self",   "set",      "signal",
    "start",  "static", "step",   "struct",  "super",   "trait",  "true",     "unsafe",
    "var",    "weak",   "while",  "yield",
};

constexpr std::string_view number_suffixes[] = {
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64",
    "deg", // degrees: compiles to a float in radians (LANGUAGE.md 1.5)
};

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex_digit(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

} // namespace

std::string_view to_string(TokenKind kind) {
    switch (kind) {
#define PLUMA_TOKEN_NAME(name) case TokenKind::name: return #name;
        PLUMA_TOKEN_KINDS(PLUMA_TOKEN_NAME)
#undef PLUMA_TOKEN_NAME
    }
    return "?";
}

bool is_keyword(std::string_view text) {
    return std::find(std::begin(keywords), std::end(keywords), text) != std::end(keywords);
}

char Lexer::peek(std::size_t ahead) const {
    return pos_ + ahead < end_ ? source_[pos_ + ahead] : '\0';
}

char Lexer::advance() {
    char c = source_[pos_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (at_end() || source_[pos_] != expected) {
        return false;
    }
    advance();
    return true;
}

Token Lexer::make(TokenKind kind) const {
    Token t{kind, source_.substr(start_, pos_ - start_), start_line_, start_column_};
    t.offset = static_cast<std::uint32_t>(start_);
    t.length = static_cast<std::uint32_t>(pos_ - start_);
    t.space_before = space_before_;
    return t;
}

Token Lexer::error(std::string_view message) const {
    Token t{TokenKind::Error, message, start_line_, start_column_};
    t.offset = static_cast<std::uint32_t>(start_);
    t.length = static_cast<std::uint32_t>(std::max<std::size_t>(pos_ - start_, 1));
    t.space_before = space_before_;
    return t;
}

bool Lexer::skip_whitespace_and_comments(Token& error_token) {
    // Editors on Windows put a UTF-8 byte order mark at the start of a file.
    if (pos_ == 0 && source_.size() >= 3 && source_.compare(0, 3, "\xEF\xBB\xBF") == 0) pos_ = 3;
    for (;;) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
            space_before_ = true;
        } else if (c == '/' && peek(1) == '/') {
            while (!at_end() && peek() != '\n') advance();
            space_before_ = true;
        } else if (c == '/' && peek(1) == '*') {
            start_ = pos_;
            start_line_ = line_;
            start_column_ = column_;
            advance();
            advance();
            space_before_ = true;
            int depth = 1; // block comments nest
            while (depth > 0) {
                if (at_end()) {
                    error_token = error("unterminated block comment");
                    error_token.length = 2;
                    return false;
                }
                if (peek() == '/' && peek(1) == '*') {
                    advance();
                    advance();
                    ++depth;
                } else if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    --depth;
                } else {
                    advance();
                }
            }
        } else {
            return true;
        }
    }
}

Token Lexer::next() {
    for (;;) {
        Token comment_error;
        if (!skip_whitespace_and_comments(comment_error)) {
            return comment_error;
        }

        start_ = pos_;
        start_line_ = line_;
        start_column_ = column_;

        if (at_end()) {
            return make(TokenKind::Eof);
        }

        char c = advance();
        if (c == '\n') {
            space_before_ = true;
            if (last_was_newline_) {
                continue; // collapse blank lines
            }
            last_was_newline_ = true;
            return make(TokenKind::Newline);
        }
        last_was_newline_ = false;

        Token token = [&]() -> Token {
            if (c == 'r' && peek() == '"') {
                advance();
                return string(true);
            }
            if (is_alpha(c)) return identifier();
            if (is_digit(c)) return number();

            switch (c) {
                case '"': return string(false);
                case '#': return name();
                case '@': return make(TokenKind::At);
                case '{': return make(TokenKind::LeftBrace);
                case '}': return make(TokenKind::RightBrace);
                case '(': return make(TokenKind::LeftParen);
                case ')': return make(TokenKind::RightParen);
                case '[': return make(TokenKind::LeftBracket);
                case ']': return make(TokenKind::RightBracket);
                case ',': return make(TokenKind::Comma);
                case ';': return make(TokenKind::Semicolon);
                case '~': return make(TokenKind::Tilde);
                case '^': return make(match('=') ? TokenKind::CaretEqual : TokenKind::Caret);
                case '.':
                    if (match('.')) return make(match('=') ? TokenKind::DotDotEqual : TokenKind::DotDot);
                    return make(TokenKind::Dot);
                case ':': return make(match(':') ? TokenKind::ColonColon : TokenKind::Colon);
                case '?':
                    if (match('.')) return make(TokenKind::QuestionDot);
                    if (match('?')) return make(TokenKind::QuestionQuestion);
                    return make(TokenKind::Question);
                case '+': return make(match('=') ? TokenKind::PlusEqual : TokenKind::Plus);
                case '-':
                    if (match('>')) return make(TokenKind::Arrow);
                    return make(match('=') ? TokenKind::MinusEqual : TokenKind::Minus);
                case '*': return make(match('=') ? TokenKind::StarEqual : TokenKind::Star);
                case '/': return make(match('=') ? TokenKind::SlashEqual : TokenKind::Slash);
                case '%': return make(match('=') ? TokenKind::PercentEqual : TokenKind::Percent);
                case '=':
                    if (match('>')) return make(TokenKind::FatArrow);
                    return make(match('=') ? TokenKind::EqualEqual : TokenKind::Equal);
                case '!': return make(match('=') ? TokenKind::BangEqual : TokenKind::Bang);
                case '<':
                    if (match('<')) return make(match('=') ? TokenKind::LessLessEqual : TokenKind::LessLess);
                    return make(match('=') ? TokenKind::LessEqual : TokenKind::Less);
                case '>':
                    // `>>` stays two tokens; see token.hpp.
                    return make(match('=') ? TokenKind::GreaterEqual : TokenKind::Greater);
                case '&':
                    if (match('&')) return make(TokenKind::AmpAmp);
                    return make(match('=') ? TokenKind::AmpEqual : TokenKind::Amp);
                case '|':
                    if (match('|')) return make(TokenKind::PipePipe);
                    return make(match('=') ? TokenKind::PipeEqual : TokenKind::Pipe);
                default:
                    if (static_cast<unsigned char>(c) >= 0x80) {
                        // One diagnostic per character, not per UTF-8 byte.
                        while ((static_cast<unsigned char>(peek()) & 0xC0) == 0x80) advance();
                        return error("unexpected non-ASCII character (identifiers are ASCII; other text belongs in strings and comments)");
                    }
                    return error("unexpected character");
            }
        }();
        space_before_ = false;
        return token;
    }
}

Token Lexer::identifier() {
    while (is_alnum(peek())) advance();
    Token token = make(TokenKind::Identifier);
    if (is_keyword(token.text)) {
        token.kind = TokenKind::Keyword;
    }
    return token;
}

Token Lexer::number() {
    bool is_float = false;
    bool based = false;
    if (source_[start_] == '0' && (peek() == 'x' || peek() == 'X') && is_hex_digit(peek(1))) {
        advance();
        while (is_hex_digit(peek()) || peek() == '_') advance();
        based = true;
    } else if (source_[start_] == '0' && (peek() == 'b' || peek() == 'B') &&
               (peek(1) == '0' || peek(1) == '1')) {
        advance();
        while (peek() == '0' || peek() == '1' || peek() == '_') advance();
        if (is_digit(peek())) {
            while (is_alnum(peek())) advance();
            return error("invalid digit in a binary literal (only 0 and 1)");
        }
        based = true;
    } else {
        while (is_digit(peek()) || peek() == '_') advance();
        // A '.' followed by a digit is a fraction; `0..8` stays Int DotDot Int.
        if (peek() == '.' && is_digit(peek(1))) {
            is_float = true;
            advance();
            while (is_digit(peek()) || peek() == '_') advance();
        }
        if ((peek() == 'e' || peek() == 'E') &&
            (is_digit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && is_digit(peek(2))))) {
            is_float = true;
            advance();
            if (peek() == '+' || peek() == '-') advance();
            while (is_digit(peek())) advance();
        }
    }
    if (is_alpha(peek())) {
        std::size_t suffix_start = pos_;
        while (is_alnum(peek())) advance();
        std::string_view suffix = source_.substr(suffix_start, pos_ - suffix_start);
        bool known = std::find(std::begin(number_suffixes), std::end(number_suffixes), suffix) !=
                     std::end(number_suffixes);
        bool float_suffix = suffix[0] == 'f' || suffix == "deg";
        if (!known || (float_suffix && based)) {
            return error("invalid number literal");
        }
        if (float_suffix) {
            is_float = true;
        } else if (is_float) {
            return error("integer suffix on a float literal");
        }
    }
    return make(is_float ? TokenKind::Float : TokenKind::Int);
}

std::string_view Lexer::scan_interpolation(bool multiline) {
    int depth = 1;
    while (depth > 0) {
        if (at_end()) return "unterminated string";
        char c = peek();
        if (c == '\n' && !multiline) return "unterminated string (strings cannot span lines)";
        if (c == '"' || (c == 'r' && peek(1) == '"' && !is_alnum(pos_ > 0 ? source_[pos_ - 1] : ' '))) {
            bool raw = c == 'r';
            if (raw) advance();
            advance();
            bool inner_multi = peek() == '"' && peek(1) == '"';
            if (inner_multi) {
                advance();
                advance();
            }
            if (std::string_view err = scan_string_body(raw, inner_multi); !err.empty()) return err;
            continue;
        }
        if (c == '{') ++depth;
        if (c == '}') --depth;
        advance();
    }
    return {};
}

std::string_view Lexer::scan_string_body(bool raw, bool multiline) {
    for (;;) {
        if (at_end()) return "unterminated string";
        char c = peek();
        if (multiline) {
            if (c == '"' && peek(1) == '"' && peek(2) == '"') {
                advance();
                advance();
                advance();
                return {};
            }
        } else {
            if (c == '"') {
                advance();
                return {};
            }
            if (c == '\n') return "unterminated string (strings cannot span lines)";
        }
        if (raw) {
            advance();
            continue;
        }
        if (c == '\\') {
            advance(); // keep escapes raw; the parser decodes them
            if (!at_end() && (multiline || peek() != '\n')) advance();
            continue;
        }
        if (c == '{' && peek(1) == '{') {
            advance();
            advance();
            continue;
        }
        if (c == '{') {
            advance();
            if (std::string_view err = scan_interpolation(multiline); !err.empty()) return err;
            continue;
        }
        advance();
    }
}

Token Lexer::string(bool raw) {
    bool multiline = false;
    if (peek() == '"' && peek(1) == '"') {
        advance();
        advance();
        multiline = true;
    }
    if (std::string_view err = scan_string_body(raw, multiline); !err.empty()) {
        Token t = error(err);
        t.length = multiline ? 3 : (raw ? 2 : 1);
        return t;
    }
    return make(TokenKind::String);
}

Token Lexer::name() {
    if (!is_alpha(peek())) {
        return error("expected a name after '#'");
    }
    while (is_alnum(peek())) advance();
    return make(TokenKind::Name);
}

std::vector<Token> Lexer::tokenize_all() {
    std::vector<Token> tokens;
    for (;;) {
        Token token = next();
        tokens.push_back(token);
        if (token.kind == TokenKind::Eof) {
            return tokens;
        }
    }
}

std::vector<Token> Lexer::tokenize(std::string_view source) {
    Lexer lexer(source);
    return lexer.tokenize_all();
}

namespace {

// Scans a nested block comment from `i` with the given depth. Returns the
// position after the comment end, or line.size() with depth > 0 left in `depth`.
std::size_t scan_comment(std::string_view line, std::size_t i, std::uint16_t& depth) {
    while (i < line.size()) {
        if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '*') {
            ++depth;
            i += 2;
        } else if (line[i] == '*' && i + 1 < line.size() && line[i + 1] == '/') {
            i += 2;
            if (--depth == 0) return i;
        } else {
            ++i;
        }
    }
    return line.size();
}

// Scans the body of a `"""` string from `i`. Returns the position after the
// closing quotes, or line.size() when the string continues on the next line.
std::size_t scan_multi_body(std::string_view line, std::size_t i, bool raw, std::uint16_t& interp,
                            bool& closed) {
    closed = false;
    while (i < line.size()) {
        char c = line[i];
        if (interp == 0) {
            if (c == '"' && line.compare(i, 3, "\"\"\"") == 0) {
                closed = true;
                return i + 3;
            }
            if (raw) { ++i; continue; }
            if (c == '\\') { i += 2; continue; }
            if (c == '{' && i + 1 < line.size() && line[i + 1] == '{') { i += 2; continue; }
            if (c == '{') ++interp;
            ++i;
        } else {
            if (c == '{') {
                ++interp;
            } else if (c == '}') {
                --interp;
            } else if (c == '"') { // nested single-line string inside the interpolation
                ++i;
                while (i < line.size() && line[i] != '"') i += line[i] == '\\' ? 2 : 1;
            }
            ++i;
        }
    }
    return line.size();
}

void add_gap_comments(std::string_view line, std::size_t from, std::size_t to,
                      std::vector<LineToken>& out) {
    for (std::size_t i = from; i < to;) {
        if (line[i] == '/' && i + 1 < to && line[i + 1] == '/') {
            out.push_back({TokenKind::Eof, true, static_cast<std::uint32_t>(i),
                           static_cast<std::uint32_t>(to - i)});
            return;
        }
        if (line[i] == '/' && i + 1 < to && line[i + 1] == '*') {
            std::uint16_t depth = 0;
            std::size_t e = scan_comment(line.substr(0, to), i, depth);
            out.push_back({TokenKind::Eof, true, static_cast<std::uint32_t>(i),
                           static_cast<std::uint32_t>(e - i)});
            i = e;
        } else {
            ++i;
        }
    }
}

} // namespace

LineResult Lexer::tokenize_line(std::string_view line, LineState state) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    LineResult result;
    std::size_t pos = 0; // where Normal lexing resumes
    auto seg = [&](TokenKind k, bool comment, std::size_t b, std::size_t e) {
        result.tokens.push_back(
            {k, comment, static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(e - b)});
    };
    if (state.mode == LineState::Mode::BlockComment) {
        std::uint16_t depth = state.depth;
        pos = scan_comment(line, 0, depth);
        if (pos > 0) seg(TokenKind::Eof, true, 0, pos);
        if (depth > 0) {
            result.state = {LineState::Mode::BlockComment, depth, false};
            return result;
        }
    } else if (state.mode == LineState::Mode::MultiString) {
        std::uint16_t interp = state.depth;
        bool closed = false;
        pos = scan_multi_body(line, 0, state.raw, interp, closed);
        if (pos > 0) seg(TokenKind::String, false, 0, pos);
        if (!closed) {
            result.state = {LineState::Mode::MultiString, interp, state.raw};
            return result;
        }
    }

    Lexer lexer(line, pos, line.size(), 1, static_cast<std::uint32_t>(pos + 1));
    std::size_t prev_end = pos;
    for (;;) {
        Token t = lexer.next();
        if (t.kind == TokenKind::Eof) {
            add_gap_comments(line, prev_end, line.size(), result.tokens);
            break;
        }
        if (t.kind == TokenKind::Newline) continue;
        add_gap_comments(line, prev_end, t.offset, result.tokens);
        std::size_t b = t.offset;
        if (t.kind == TokenKind::Error) {
            if (t.text == "unterminated block comment") {
                std::uint16_t depth = 0;
                scan_comment(line, b, depth);
                seg(TokenKind::Eof, true, b, line.size());
                result.state = {LineState::Mode::BlockComment, depth, false};
                return result;
            }
            if (t.length == 3 && t.text.starts_with("unterminated string")) {
                bool raw = line[b] == 'r';
                std::uint16_t interp = 0;
                bool closed = false;
                scan_multi_body(line, b + (raw ? 4 : 3), raw, interp, closed);
                seg(TokenKind::String, false, b, line.size());
                result.state = {LineState::Mode::MultiString, interp, raw};
                return result;
            }
            // Other errors (an unterminated one-line string eats to the line end).
            seg(TokenKind::Error, false, b, std::max<std::size_t>(b + t.length, lexer.pos_));
            prev_end = std::max<std::size_t>(b + t.length, lexer.pos_);
            continue;
        }
        seg(t.kind, false, b, b + t.length);
        prev_end = b + t.length;
    }
    return result;
}

} // namespace pluma
