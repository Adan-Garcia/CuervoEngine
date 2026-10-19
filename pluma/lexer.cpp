#include "pluma/lexer.hpp"

#include <algorithm>
#include <iterator>

namespace pluma {

namespace {

// LANGUAGE.md 1.4.
constexpr std::string_view keywords[] = {
    "alloc",  "as",     "await",  "break",   "class",   "const",  "continue", "defer",
    "else",   "emit",   "enum",   "extends", "false",   "final",  "fn",       "for",
    "foreach", "free",  "get",    "if",      "in",      "is",     "loop",     "manual",
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
                default: return error("unexpected character");
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

} // namespace pluma
