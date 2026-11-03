#include <doctest.h>

#include "editor/script_find.hpp"

using namespace cuervo::editor;

namespace {
TextBuffer make(const char* t) { TextBuffer b; b.set_text(t); return b; }
}

TEST_CASE("find: plain, case and whole word") {
    TextBuffer b = make("Speed speed speedy\nx_speed speed");
    FindOptions o;
    CHECK(find_all(b, "speed", o).ranges.size() == 5);
    o.match_case = true;
    CHECK(find_all(b, "speed", o).ranges.size() == 4);
    o.whole_word = true;
    const FindResult r = find_all(b, "speed", o);
    REQUIRE(r.ranges.size() == 2);
    CHECK(r.ranges[0].start == TextPos{0, 6});
    CHECK(r.ranges[1].start == TextPos{1, 8});
}

TEST_CASE("find: regex, invalid pattern and capture replace") {
    TextBuffer b = make("a1 b22 c333\nd4");
    FindOptions o;
    o.regex = true;
    CHECK(find_all(b, "[0-9]+", o).ranges.size() == 4);
    CHECK_FALSE(find_all(b, "(", o).error.empty());
    CHECK(find_all(b, "(", o).ranges.empty());
    CHECK(find_replace_all(b, "([a-z])([0-9]+)", o, "$2$1") == 4);
    CHECK(b.text() == "1a 22b 333c\n4d");
    o.whole_word = true;
    TextBuffer c = make("cat concat cat");
    CHECK(find_all(c, "cat", o).ranges.size() == 2);
}

TEST_CASE("find: replace all is one undo step and navigation wraps") {
    TextBuffer b = make("x = x + x\nx");
    FindOptions o;
    CHECK(find_replace_all(b, "x", o, "val") == 4);
    CHECK(b.text() == "val = val + val\nval");
    CHECK(b.undo());
    CHECK(b.text() == "x = x + x\nx");
    CHECK_FALSE(b.undo());
    const FindResult r = find_all(b, "x", o);
    CHECK(find_index(r.ranges, {0, 0}, false, false) == 0);
    CHECK(find_index(r.ranges, {0, 0}, false, true) == 1);
    CHECK(find_index(r.ranges, {1, 1}, false, true) == 0); // wraps
    CHECK(find_index(r.ranges, {0, 0}, true, false) == 3); // wraps backward
    CHECK(find_index(r.ranges, {0, 8}, true, false) == 1);
}

TEST_CASE("find: bounded results and regex limits") {
    std::string t;
    for (int i = 0; i < 25000; ++i) t += "a\n";
    TextBuffer b = make(t.c_str());
    const FindResult r = find_all(b, "a", {});
    CHECK(r.truncated);
    CHECK(r.ranges.size() == kMaxMatches);
    std::string longline(kMaxRegexLine + 10, 'a');
    TextBuffer c = make((longline + "\nab").c_str());
    FindOptions o;
    o.regex = true;
    const FindResult r2 = find_all(c, "ab", o);
    CHECK(r2.limited);
    CHECK(r2.ranges.size() == 1);
}

TEST_CASE("find session: step, replace one, replace all, edits invalidate") {
    TextBuffer b = make("foo bar foo\nfoo");
    FindSession s;
    s.query = "foo";
    s.refresh(b, true);
    CHECK(s.result.ranges.size() == 3);
    CHECK(s.current == 0);
    CHECK(b.primary().start() == TextPos{0, 0});
    CHECK(s.step(b, false));
    CHECK(s.current == 1);
    CHECK(s.step(b, false));
    CHECK(s.step(b, false)); // wraps to the first
    CHECK(s.current == 0);
    CHECK(s.step(b, true)); // backward wraps to the last
    CHECK(s.current == 2);
    // Replace one: the selected match goes, the next one is selected.
    s.replacement = "Q";
    s.select(b, 0);
    CHECK(s.replace_current(b));
    CHECK(b.text() == "Q bar foo\nfoo");
    CHECK(s.current == 0);
    CHECK(b.selected_text() == "foo");
    // Replace all is one undo step on top of the previous one.
    CHECK(s.replace_everything(b) == 2);
    CHECK(s.message == "Replaced 2");
    CHECK(b.text() == "Q bar Q\nQ");
    b.undo();
    CHECK(b.text() == "Q bar foo\nfoo");
    // A stale session refreshes itself on step.
    b.set_caret({0, 0});
    b.insert("foo ");
    CHECK(s.step(b, false));
    CHECK(s.result.ranges.size() == 3);
}
