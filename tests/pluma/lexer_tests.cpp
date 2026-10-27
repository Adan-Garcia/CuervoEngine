#include <doctest.h>

#include <string_view>
#include <vector>

#include "pluma/lexer.hpp"

using pluma::Lexer;
using pluma::Token;
using pluma::TokenKind;
using K = TokenKind;

namespace {

std::vector<TokenKind> kinds(std::string_view source) {
    std::vector<TokenKind> out;
    for (const Token& token : Lexer::tokenize(source)) {
        out.push_back(token.kind);
    }
    return out;
}

} // namespace

TEST_CASE("identifiers and keywords") {
    auto tokens = Lexer::tokenize("var speed fn _x2");
    REQUIRE(tokens.size() == 5);
    CHECK(tokens[0].kind == K::Keyword);
    CHECK(tokens[1].kind == K::Identifier);
    CHECK(tokens[1].text == "speed");
    CHECK(tokens[2].kind == K::Keyword);
    CHECK(tokens[3].text == "_x2");
}

TEST_CASE("numbers") {
    CHECK(kinds("42 3.5 1e9 2.5e-3 0xFF 1_000") ==
          std::vector{K::Int, K::Float, K::Float, K::Float, K::Int, K::Int, K::Eof});
    // Ranges must not be mistaken for floats.
    CHECK(kinds("0..8") == std::vector{K::Int, K::DotDot, K::Int, K::Eof});
    CHECK(kinds("12abc") == std::vector{K::Error, K::Eof});
}

TEST_CASE("strings") {
    auto tokens = Lexer::tokenize(R"("Score: {score}" "a\"b")");
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[0].kind == K::String);
    CHECK(tokens[0].text == R"("Score: {score}")");
    CHECK(tokens[1].text == "\"a\\\"b\""); // not a raw string: MSVC misparses R"(...\"...)" inside a macro argument

    CHECK(kinds("\"open") == std::vector{K::Error, K::Eof});
}

TEST_CASE("interned names") {
    auto tokens = Lexer::tokenize("Input.pressed(#shoot)");
    REQUIRE(tokens.size() == 7);
    CHECK(tokens[4].kind == K::Name);
    CHECK(tokens[4].text == "#shoot");
    CHECK(kinds("# x") == std::vector{K::Error, K::Identifier, K::Eof});
}

TEST_CASE("comments, including nested block comments") {
    CHECK(kinds("a // line comment\nb") == std::vector{K::Identifier, K::Newline, K::Identifier, K::Eof});
    CHECK(kinds("a /* outer /* inner */ still comment */ b") == std::vector{K::Identifier, K::Identifier, K::Eof});
    CHECK(kinds("a /* never closed") == std::vector{K::Identifier, K::Error, K::Eof});
}

TEST_CASE("braces, operators and punctuation") {
    CHECK(kinds("fn f(a: int) -> bool { return a >= 2 && !b }") ==
          std::vector{K::Keyword, K::Identifier, K::LeftParen, K::Identifier, K::Colon, K::Identifier,
                      K::RightParen, K::Arrow, K::Identifier, K::LeftBrace, K::Keyword, K::Identifier,
                      K::GreaterEqual, K::Int, K::AmpAmp, K::Bang, K::Identifier, K::RightBrace, K::Eof});
    CHECK(kinds("x?.y ?? z => w += 1") ==
          std::vector{K::Identifier, K::QuestionDot, K::Identifier, K::QuestionQuestion, K::Identifier,
                      K::FatArrow, K::Identifier, K::PlusEqual, K::Int, K::Eof});
}

TEST_CASE("newlines end statements; blank lines collapse") {
    CHECK(kinds("\n\na\n\n\nb\n") == std::vector{K::Identifier, K::Newline, K::Identifier, K::Newline, K::Eof});
}

TEST_CASE("positions are 1-based line and column") {
    auto tokens = Lexer::tokenize("a\n  bb");
    REQUIRE(tokens.size() == 4);
    CHECK(tokens[2].line == 2);
    CHECK(tokens[2].column == 3);
}

TEST_CASE("numeric suffixes, including deg") {
    CHECK(kinds("42u8 7i64 2.5f64 3f32") == std::vector{K::Int, K::Int, K::Float, K::Float, K::Eof});
    // `deg` makes a float (radians) from an int or float literal (LANGUAGE.md 1.5).
    CHECK(kinds("45deg 12.5deg") == std::vector{K::Float, K::Float, K::Eof});
    CHECK(kinds("0x10deg") == std::vector{K::Error, K::Eof});
    CHECK(kinds("1.5u8") == std::vector{K::Error, K::Eof});
    CHECK(kinds("3degx") == std::vector{K::Error, K::Eof});
}

TEST_CASE("'>>' stays two tokens so nested generics close") {
    auto tokens = Lexer::tokenize("a >> b >>= c");
    REQUIRE(tokens.size() == 8);
    CHECK(tokens[1].kind == K::Greater);
    CHECK(tokens[2].kind == K::Greater);
    CHECK_FALSE(tokens[2].space_before);
    CHECK(tokens[4].kind == K::Greater);
    CHECK(tokens[5].kind == K::GreaterEqual);
    CHECK_FALSE(tokens[5].space_before);
}

TEST_CASE("space_before drives the ? rule") {
    auto tokens = Lexer::tokenize("a? b ?c");
    REQUIRE(tokens.size() == 6);
    CHECK_FALSE(tokens[1].space_before); // a?
    CHECK(tokens[2].space_before);
    CHECK(tokens[3].space_before);       // ` ?`
    CHECK_FALSE(tokens[4].space_before); // ?c
}

TEST_CASE("offsets and lengths") {
    auto tokens = Lexer::tokenize("ab  \"x{y}\" #n");
    REQUIRE(tokens.size() == 4);
    CHECK(tokens[0].offset == 0);
    CHECK(tokens[0].length == 2);
    CHECK(tokens[1].offset == 4);
    CHECK(tokens[1].length == 6);
    CHECK(tokens[2].offset == 11);
    CHECK(tokens[2].length == 2);
}

TEST_CASE("raw and multiline strings are single tokens") {
    CHECK(kinds("r\"a\\n{b}\"") == std::vector{K::String, K::Eof});
    CHECK(kinds("\"\"\"\nline\n\"\"\"") == std::vector{K::String, K::Eof});
    CHECK(kinds("\"{\"nested\"}\"") == std::vector{K::String, K::Eof});
    CHECK(kinds("\"\"\"never closed") == std::vector{K::Error, K::Eof});
}
