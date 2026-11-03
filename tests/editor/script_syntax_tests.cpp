#include <doctest.h>

#include <string>

#include "editor/script_syntax.hpp"

using namespace cuervo::editor;

namespace {
TextBuffer make(std::string_view t) {
    TextBuffer b;
    b.set_text(t);
    return b;
}
Syn at(const SyntaxCache& c, int line, std::uint32_t col) {
    for (const SynSpan& s : c.line(line).spans) if (col >= s.col && col < s.col + s.len) return s.kind;
    return Syn::Plain;
}
} // namespace

TEST_CASE("syntax: token kinds on one line") {
    TextBuffer b = make("var x: Ship = 12.5 // note\nfn f() { \"s\" }");
    SyntaxCache c;
    c.update(b);
    CHECK(c.size() == 2);
    CHECK(at(c, 0, 0) == Syn::Keyword);
    CHECK(at(c, 0, 4) == Syn::Plain);
    CHECK(at(c, 0, 7) == Syn::Type);
    CHECK(at(c, 0, 14) == Syn::Number);
    CHECK(at(c, 0, 21) == Syn::Comment);
    CHECK(at(c, 1, 0) == Syn::Keyword);
    CHECK(at(c, 1, 11) == Syn::String);
}

TEST_CASE("syntax: block comments and strings span lines") {
    TextBuffer b = make("a /* x\n y\n z */ b\nc");
    SyntaxCache c;
    c.update(b);
    CHECK(at(c, 0, 2) == Syn::Comment);
    CHECK(c.line(1).carry == Syn::Comment);
    CHECK(at(c, 1, 1) == Syn::Comment);
    CHECK(at(c, 2, 2) == Syn::Comment);
    CHECK(at(c, 2, 7) == Syn::Plain);
    CHECK(c.line(3).carry == Syn::Plain);
}

TEST_CASE("syntax: edits re-lex only dirty lines and settle") {
    std::string text;
    for (int i = 0; i < 200; ++i) text += "var a" + std::to_string(i) + " = " + std::to_string(i) + "\n";
    TextBuffer b = make(text);
    SyntaxCache c;
    c.update(b);
    CHECK(c.last_lexed() == 201);
    b.set_caret({100, 0});
    b.insert("// ");
    c.update(b);
    CHECK(c.last_lexed() <= 2);
    CHECK(at(c, 100, 0) == Syn::Comment);
    CHECK(at(c, 101, 0) == Syn::Keyword);
    // Opening a block comment recolors the lines below until it closes.
    b.set_caret({10, 0});
    b.insert("/* ");
    c.update(b);
    CHECK(at(c, 11, 0) == Syn::Comment);
    CHECK(at(c, 150, 0) == Syn::Comment);
    CHECK(c.line(150).carry == Syn::Comment);
    // Closing it again restores them.
    b.undo();
    c.update(b);
    CHECK(at(c, 11, 0) == Syn::Keyword);
    CHECK(at(c, 150, 0) == Syn::Keyword);
    // Inserting a line shifts without re-lexing the rest.
    b.set_caret({50, 0});
    b.insert("\n");
    c.update(b);
    CHECK(c.size() == 202);
    CHECK(c.last_lexed() <= 3);
    CHECK(at(c, 52, 0) == Syn::Keyword);
}

TEST_CASE("syntax: cache matches a full lex after random edits") {
    TextBuffer b = make("class A {\n  var x = 1\n  fn f() {\n    /* c */ x\n  }\n}\n");
    SyntaxCache inc;
    inc.update(b);
    const char* inserts[] = {"/*", "*/", "\"", "x ", "\n", "// ", "{", "}"};
    unsigned seed = 7;
    for (int i = 0; i < 60; ++i) {
        seed = seed * 1103515245u + 12345u;
        const int line = static_cast<int>((seed >> 8) % static_cast<unsigned>(b.line_count()));
        seed = seed * 1103515245u + 12345u;
        const int col = static_cast<int>((seed >> 8) % static_cast<unsigned>(b.line(line).size() + 1));
        b.set_caret({line, col});
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 8) % 5 == 0) b.backspace();
        else b.insert(inserts[(seed >> 8) % 8]);
        INFO("insert before update step ", i);
        inc.update(b);
        TextBuffer copy = make(b.text());
        SyntaxCache full;
        full.update(copy);
        for (int l = 0; l < b.line_count(); ++l) {
            const auto& x = inc.line(l).spans;
            const auto& y = full.line(l).spans;
            INFO("step ", i, " line ", l, " carry inc ", (int)inc.line(l).carry, " full ", (int)full.line(l).carry, " ncarets ", b.selections().size(), " caret ", b.primary().head.line, ":", b.primary().head.col, " text:\n", b.text());
            REQUIRE(x.size() == y.size());
            for (std::size_t k = 0; k < x.size(); ++k) {
                CHECK(x[k].col == y[k].col);
                CHECK(x[k].len == y[k].len);
                CHECK(x[k].kind == y[k].kind);
            }
        }
    }
}
