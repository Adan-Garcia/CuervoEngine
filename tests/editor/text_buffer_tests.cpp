#include <doctest.h>

#include "editor/text_buffer.hpp"

using namespace cuervo::editor;

namespace {
TextBuffer make(std::string_view t) {
    TextBuffer b;
    b.set_text(t);
    return b;
}
} // namespace

TEST_CASE("text buffer: load, line endings, bom, indent detection") {
    TextBuffer b = make("a\r\nb\r\n  c\r\n");
    CHECK(b.line_ending() == LineEnding::CrLf);
    CHECK(b.line_count() == 4);
    CHECK(b.text() == "a\nb\n  c\n");
    CHECK(b.text_for_save() == "a\r\nb\r\n  c\r\n");
    TextBuffer m = make("x\ny\r\nz\n");
    CHECK(m.line_ending() == LineEnding::Lf);
    TextBuffer bom = make("\xEF\xBB\xBFhi");
    CHECK(bom.has_bom());
    CHECK(bom.line(0) == "hi");
    CHECK(bom.text_for_save() == "\xEF\xBB\xBFhi");
    CHECK(make("a {\n  b {\n    c\n  }\n}\n").indent().width == 2);
    CHECK(make("a {\n    b\n}\n").indent().width == 4);
    CHECK(make("a {\n\tb\n\tc\n}\n").indent().tabs);
    CHECK(TextBuffer::normalize("a\r\nb\rc") == "a\nb\nc");
}

TEST_CASE("text buffer: graphemes and caret movement") {
    // e + combining acute, then a family emoji (ZWJ sequence), then a regional-indicator flag pair.
    const std::string s = "e\xCC\x81" "x" "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9" "\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8" "z";
    TextBuffer b = make(s);
    b.set_caret({0, 0});
    b.move(Move::Right, false);
    CHECK(b.primary().head.col == 3); // e + combining mark is one step
    b.move(Move::Right, false);
    CHECK(b.primary().head.col == 4);
    b.move(Move::Right, false);
    CHECK(b.primary().head.col == 4 + 11); // ZWJ family
    b.move(Move::Right, false);
    CHECK(b.primary().head.col == 4 + 11 + 8); // flag
    b.move(Move::Left, false);
    CHECK(b.primary().head.col == 15);
    b.move(Move::Left, false);
    b.move(Move::Left, false);
    CHECK(b.primary().head.col == 3);
    // Backspace removes a whole cluster.
    b.set_caret({0, 3});
    b.backspace();
    CHECK(b.line(0).starts_with("x"));
}

TEST_CASE("text buffer: cells, tabs and display columns") {
    CHECK(grapheme_cells("a") == 1);
    CHECK(grapheme_cells("\xE4\xB8\xAD") == 2);
    CHECK(display_col("\tab", 1, 4) == 4);
    CHECK(display_col("a\tb", 2, 4) == 4);
    CHECK(display_col("\xE4\xB8\xAD" "b", 3, 4) == 2);
    CHECK(col_at_display("\tab", 5, 4) == 2);
    CHECK(col_at_display("abc", 99, 4) == 3);
}

TEST_CASE("text buffer: vertical movement keeps the goal column") {
    TextBuffer b = make("abcdef\nab\nabcdef");
    b.set_caret({0, 5});
    b.move(Move::Down, false);
    CHECK(b.primary().head == TextPos{1, 2});
    b.move(Move::Down, false);
    CHECK(b.primary().head == TextPos{2, 5});
    b.move(Move::Up, true);
    CHECK(b.primary().anchor == TextPos{2, 5});
    CHECK(b.primary().head == TextPos{1, 2});
    b.set_caret({0, 0});
    b.move(Move::Up, false);
    CHECK(b.primary().head == TextPos{0, 0});
    b.move(Move::DocEnd, false);
    CHECK(b.primary().head == TextPos{2, 6});
}

TEST_CASE("text buffer: word movement and smart home") {
    TextBuffer b = make("  foo_bar + baz");
    b.set_caret({0, 0});
    b.move(Move::WordRight, false);
    CHECK(b.primary().head.col == 9);
    b.move(Move::WordRight, false);
    CHECK(b.primary().head.col == 11);
    b.move(Move::WordLeft, false);
    CHECK(b.primary().head.col == 10);
    b.set_caret({0, 7});
    b.move(Move::SmartHome, false);
    CHECK(b.primary().head.col == 2);
    b.move(Move::SmartHome, false);
    CHECK(b.primary().head.col == 0);
    CHECK(b.word_at({0, 5}) == TextRange{{0, 2}, {0, 9}});
}

TEST_CASE("text buffer: insert, delete and undo grouping") {
    TextBuffer b = make("");
    b.insert("h", true, 0);
    b.insert("i", true, 100);
    b.insert("!", true, 200);
    CHECK(b.text() == "hi!");
    CHECK(b.dirty());
    CHECK(b.undo()); // typing coalesced into one step
    CHECK(b.text() == "");
    CHECK_FALSE(b.dirty());
    CHECK(b.redo());
    CHECK(b.text() == "hi!");
    // A pause over one second starts a new group.
    b.insert("a", true, 5000);
    b.insert("b", true, 5100);
    b.insert("c", true, 7000);
    b.undo();
    CHECK(b.text() == "hi!ab");
    b.undo();
    CHECK(b.text() == "hi!");
    // A caret jump ends the group.
    b.insert("x", true, 9000);
    b.set_caret({0, 0});
    b.insert("y", true, 9100);
    CHECK(b.text() == "yhi!x");
    b.undo();
    CHECK(b.text() == "hi!x");
    b.undo();
    CHECK(b.text() == "hi!");
}

TEST_CASE("text buffer: redo is dropped by a new edit, saved marker follows undo") {
    TextBuffer b = make("one");
    b.set_caret({0, 3});
    b.insert(" two");
    b.mark_saved();
    CHECK_FALSE(b.dirty());
    b.insert(" three");
    CHECK(b.dirty());
    b.undo();
    CHECK_FALSE(b.dirty());
    b.undo();
    CHECK(b.dirty());
    b.redo();
    CHECK_FALSE(b.dirty());
    b.undo();
    b.insert("!");
    CHECK_FALSE(b.can_redo());
}

TEST_CASE("text buffer: multi-line insert, backspace joins lines, selection replace") {
    TextBuffer b = make("ab\ncd");
    b.set_caret({0, 1});
    b.insert("X\nY\r\nZ");
    CHECK(b.text() == "aX\nY\nZb\ncd");
    CHECK(b.primary().head == TextPos{2, 1});
    b.set_caret({1, 0});
    b.backspace();
    CHECK(b.text() == "aXY\nZb\ncd");
    b.set_selections({{{0, 1}, {1, 1}}});
    b.insert("-");
    CHECK(b.text() == "a-b\ncd");
    b.undo();
    CHECK(b.text() == "aXY\nZb\ncd");
    b.undo();
    b.undo();
    CHECK(b.text() == "ab\ncd");
}

TEST_CASE("text buffer: auto indent and braces") {
    TextBuffer b = make("fn f() {\n    x\n}");
    b.set_caret({0, 8});
    b.insert_newline();
    CHECK(b.line(1) == "    ");
    CHECK(b.primary().head == TextPos{1, 4});
    TextBuffer c = make("if a {}");
    c.set_caret({0, 6});
    c.insert_newline();
    CHECK(c.text() == "if a {\n    \n}");
    CHECK(c.primary().head == TextPos{1, 4});
    TextBuffer d = make("  a");
    d.set_caret({0, 3});
    d.insert_newline();
    CHECK(d.line(1) == "  ");
}

TEST_CASE("text buffer: indent, outdent, comment, move and duplicate lines") {
    TextBuffer b = make("a\nb\nc");
    b.set_selections({{{0, 0}, {1, 1}}});
    b.indent_lines(false);
    CHECK(b.text() == "    a\n    b\nc");
    CHECK(b.primary().head == TextPos{1, 5});
    b.indent_lines(true);
    CHECK(b.text() == "a\nb\nc");
    b.toggle_comment();
    CHECK(b.text() == "// a\n// b\nc");
    b.toggle_comment();
    CHECK(b.text() == "a\nb\nc");
    b.undo();
    b.undo();
    b.set_caret({1, 0});
    b.move_lines(-1);
    CHECK(b.text() == "b\na\nc");
    CHECK(b.primary().head.line == 0);
    b.move_lines(1);
    b.move_lines(1);
    CHECK(b.text() == "a\nc\nb");
    b.set_caret({0, 1});
    b.duplicate_line();
    CHECK(b.text() == "a\na\nc\nb");
    b.delete_line();
    CHECK(b.text() == "a\nc\nb");
}

TEST_CASE("text buffer: multi-caret edits are one undo step") {
    TextBuffer b = make("a\nb\nc");
    b.set_selections({{{0, 1}, {0, 1}}, {{1, 1}, {1, 1}}, {{2, 1}, {2, 1}}});
    b.insert("!");
    CHECK(b.text() == "a!\nb!\nc!");
    CHECK(b.selections().size() == 3);
    b.insert("\n");
    CHECK(b.text() == "a!\n\nb!\n\nc!\n");
    b.undo();
    CHECK(b.text() == "a!\nb!\nc!");
    b.undo();
    CHECK(b.text() == "a\nb\nc");
    // Overlapping selections merge.
    b.set_selections({{{0, 0}, {1, 1}}, {{1, 0}, {2, 1}}});
    CHECK(b.selections().size() == 1);
    CHECK(b.selected_text() == "a\nb\nc");
}

TEST_CASE("text buffer: replace and replace_all as one group") {
    TextBuffer b = make("foo bar foo\nfoo");
    b.replace_all({{{{0, 8}, {0, 11}}, "quux"}, {{{0, 0}, {0, 3}}, "x\ny"}, {{{1, 0}, {1, 3}}, ""}});
    CHECK(b.text() == "x\ny bar quux\n");
    b.undo();
    CHECK(b.text() == "foo bar foo\nfoo");
    b.replace({{0, 4}, {0, 7}}, "BAR");
    CHECK(b.line(0) == "foo BAR foo");
}

TEST_CASE("text buffer: utf16 positions") {
    TextBuffer b = make("a\xF0\x9F\x98\x80" "b\n\xC3\xA9x");
    CHECK(b.to_utf16({0, 5}) == 3); // a + surrogate pair
    CHECK(b.from_utf16(0, 3) == TextPos{0, 5});
    CHECK(b.to_utf16({1, 2}) == 1); // e-acute is one unit
    CHECK(b.from_utf16(1, 2) == TextPos{1, 3});
    CHECK(b.from_utf16(0, 99) == TextPos{0, 6});
    CHECK(utf16_len("\xF0\x9F\x98\x80") == 2);
}

TEST_CASE("text buffer: line changes are reported for caches") {
    TextBuffer b = make("a\nb");
    (void)b.take_changes();
    b.set_caret({0, 1});
    b.insert("\n");
    auto c = b.take_changes();
    REQUIRE(c.size() == 1);
    CHECK(c[0].first == 0);
    CHECK(c[0].removed == 1);
    CHECK(c[0].added == 2);
    CHECK(b.take_changes().empty());
}

TEST_CASE("typing: auto-close, skip over, wrap a selection and delete a pair") {
    TextBuffer b;
    b.set_text("");
    b.type("(");
    CHECK(b.text() == "()");
    CHECK(b.primary().head == TextPos{0, 1});
    b.type("x");
    b.type(")"); // skips over the closer already there
    CHECK(b.text() == "(x)");
    CHECK(b.primary().head == TextPos{0, 3});
    b.set_text("foo");
    b.set_caret({0, 3});
    b.type("(");
    CHECK(b.text() == "foo()");
    b.backspace(); // opener right before its closer: both go
    CHECK(b.text() == "foo");
    b.set_text("ab()");
    b.set_caret({0, 1});
    b.type("("); // next char is a word char: no pairing
    CHECK(b.text() == "a(b()");
    b.set_text("say hi");
    b.select_range({{0, 4}, {0, 6}});
    b.type("\"");
    CHECK(b.text() == "say \"hi\"");
    b.set_text("");
    b.type("\"");
    CHECK(b.text() == "\"\"");
    b.type("\""); // skip
    CHECK(b.text() == "\"\"");
    CHECK(b.primary().head == TextPos{0, 2});
    b.set_text("x = ");
    b.set_caret({0, 4});
    b.type("\"");
    b.type("a");
    b.type("\"");
    CHECK(b.text() == "x = \"a\"");
    b.set_text("a");
    b.set_caret({0, 1});
    b.type("\""); // right after a word char: a plain quote, no pair
    CHECK(b.text() == "a\"");
}

TEST_CASE("typing: pairs on every caret, undo is one step per pair") {
    TextBuffer b;
    b.set_text("\n");
    b.set_selections({{{0, 0}, {0, 0}}, {{1, 0}, {1, 0}}});
    b.type("[");
    CHECK(b.text() == "[]\n[]");
    CHECK(b.undo());
    CHECK(b.text() == "\n");
}

TEST_CASE("brackets: match forward and backward, skip strings and comments") {
    TextBuffer b;
    b.set_text("fn f(a) {\n    g(\"}\", [1])  // }\n}\n");
    TextPos o, c;
    REQUIRE(b.match_bracket({0, 4}, o, c)); // before '('
    CHECK(o == TextPos{0, 4});
    CHECK(c == TextPos{0, 6});
    REQUIRE(b.match_bracket({0, 7}, o, c)); // after ')'
    CHECK(o == TextPos{0, 4});
    REQUIRE(b.match_bracket({0, 8}, o, c)); // on '{' spans lines, the } in the string and comment do not count
    CHECK(c == TextPos{2, 0});
    REQUIRE(b.match_bracket({2, 1}, o, c));
    CHECK(o == TextPos{0, 8});
    CHECK_FALSE(b.match_bracket({0, 0}, o, c));
    b.set_text("(((");
    CHECK_FALSE(b.match_bracket({0, 0}, o, c)); // unbalanced
}

TEST_CASE("enter between brackets opens an indented line and keeps the closer below") {
    TextBuffer b;
    b.set_text("f(");
    b.set_text("x = [\n]");
    b.set_text("x = []");
    b.set_caret({0, 5});
    b.insert_newline();
    CHECK(b.text() == "x = [\n    \n]");
    CHECK(b.primary().head == TextPos{1, 4});
}
