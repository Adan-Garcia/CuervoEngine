#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "cuervo/input/input_map.hpp"
#include "cuervo/locale/locale.hpp"
#include "cuervo/script/script_host.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {
const char* const table = "key,en,ar,ja\n"
                          "# a comment row\n"
                          "start,Start,ابدأ,スタート\n"
                          "greet,\"Hello, {0}!\",مرحبا {0},\n"
                          "items.one,{0} item,,\n"
                          "items.other,{0} items,,{0}個\n"
                          "items.zero,,لا عناصر,\n"
                          "items.two,,عنصران,\n"
                          "items.few,,{0} عناصر,\n"
                          "quote,\"say \"\"hi\"\"\nnow\",,\n";
}

TEST_CASE("plural categories follow CLDR for en, ar, ru, ja, fr") {
    using locale::Plural;
    CHECK(locale::plural_category("en", 1) == Plural::One);
    CHECK(locale::plural_category("en", 0) == Plural::Other);
    CHECK(locale::plural_category("fr", 0) == Plural::One);
    CHECK(locale::plural_category("ja", 1) == Plural::Other);
    CHECK(locale::plural_category("ar", 0) == Plural::Zero);
    CHECK(locale::plural_category("ar", 2) == Plural::Two);
    CHECK(locale::plural_category("ar", 7) == Plural::Few);
    CHECK(locale::plural_category("ar", 11) == Plural::Many);
    CHECK(locale::plural_category("ar", 100) == Plural::Other);
    CHECK(locale::plural_category("ru", 21) == Plural::One);
    CHECK(locale::plural_category("ru", 22) == Plural::Few);
    CHECK(locale::plural_category("ru", 12) == Plural::Many);
    CHECK(locale::plural_category("pt-BR", 1) == Plural::One);
}

TEST_CASE("string tables: CSV, fallback, plurals, format, missing keys") {
    locale::Localizer l;
    std::string error;
    REQUIRE(l.load_csv(table, error) > 0);
    CHECK(l.locales() == std::vector<std::string>{"ar", "en", "ja"});

    CHECK(l.tr("start") == "Start");
    CHECK(l.tr("quote") == "say \"hi\"\nnow");
    CHECK(locale::format_text(l.tr("greet"), std::vector<std::string>{"Ana"}) == "Hello, Ana!");

    CHECK(l.set_locale("ja"));
    CHECK(l.tr("start") == "スタート");
    CHECK(l.tr("greet") == "Hello, {0}!"); // empty cell: falls back to en
    CHECK_FALSE(l.set_locale("xx"));
    CHECK(l.locale() == "ja");

    CHECK(l.tr("nope") == "nope");
    CHECK(l.missing_keys() == std::vector<std::string>{"nope"});

    // Plurals: English picks .one/.other, Japanese only has .other, Arabic has six forms.
    l.set_locale("en");
    CHECK(l.tr_plural("items", 1) == "{0} item");
    CHECK(l.tr_plural("items", 5) == "{0} items");
    l.set_locale("ja");
    CHECK(l.tr_plural("items", 1) == "{0}個");
    l.set_locale("ar");
    CHECK(l.tr_plural("items", 0) == "لا عناصر");
    CHECK(l.tr_plural("items", 2) == "عنصران");
    CHECK(l.tr_plural("items", 5) == "{0} عناصر");
    CHECK(l.tr_plural("items", 50) == "{0} items"); // no Arabic .many/.other: fallback locale
    CHECK(l.rtl());

    CHECK(l.load_csv("name,en\n", error) == -1);
    CHECK(l.load_csv("key,en\na,\"open", error) == -1);
}

TEST_CASE("switching locale bumps the revision and base languages match") {
    locale::Localizer l;
    l.add("en", "k", "v");
    l.add("pt", "k", "valor");
    const auto r0 = l.revision();
    CHECK(l.set_locale("pt-BR"));
    CHECK(l.locale() == "pt");
    CHECK(l.revision() > r0);
    const auto r1 = l.revision();
    l.set_locale("pt");
    CHECK(l.revision() == r1);
    CHECK(l.tr("k") == "valor");
}

TEST_CASE("format_text handles escapes and bad indices") {
    const std::vector<std::string> a{"x", "y"};
    CHECK(locale::format_text("{1}-{0} {{ok}} {2} {a}", a) == "y-x {ok} {2} {a}");
    CHECK(locale::format_text("open {", a) == "open {");
}

TEST_CASE("locale-aware number, date and time formatting") {
    CHECK(locale::format_int("en", 1234567) == "1,234,567");
    CHECK(locale::format_int("en", -999) == "-999");
    CHECK(locale::format_int("de", 1234567) == "1.234.567");
    CHECK(locale::format_int("fr", 12345) == "12 345");
    CHECK(locale::format_int("ar", 1234) == "١٬٢٣٤");
    CHECK(locale::format_float("en", 1234.5, 2) == "1,234.50");
    CHECK(locale::format_float("de", 1234.5, 1) == "1.234,5");
    CHECK(locale::format_float("ar", 0.5, 1) == "٠٫٥");
    CHECK(locale::format_float("en", -0.001, 2) == "0.00");
    CHECK(locale::format_date("en", 2026, 11, 1) == "11/1/2026");
    CHECK(locale::format_date("ja", 2026, 11, 1) == "2026/11/1");
    CHECK(locale::format_date("de", 2026, 11, 1) == "1.11.2026");
    CHECK(locale::format_date("en-GB", 2026, 11, 1) == "01/11/2026");
    CHECK(locale::format_time("en", 15, 5) == "3:05 PM");
    CHECK(locale::format_time("ja", 15, 5) == "15:05");
    CHECK(locale::format_time("de", 9, 5) == "09:05");
}

namespace {
struct ScriptFixture {
    InputMap map;
    std::unique_ptr<script::ScriptHost> host;
    fs::path dir = fs::temp_directory_path() / "cuervo_locale_tests";

    explicit ScriptFixture(const std::string& source) {
        fs::create_directories(dir / "locale");
        std::ofstream(dir / "locale" / "strings.csv", std::ios::binary) << table;
        script::HostOptions o;
        o.project_dir = dir;
        host = std::make_unique<script::ScriptHost>(o, map);
        REQUIRE_MESSAGE(host->load({{"main.plm", source}}, "Main"), host->error());
        host->tick(InputFrame{});
        REQUIRE_MESSAGE(host->running(), host->error());
    }
    std::string str(std::string_view n) { return static_cast<const pluma::StrObj*>(host->session()->global(n).o)->s; }
};
} // namespace

TEST_CASE("Locale script API: EN/AR/JA switch parity through tr()") {
    ScriptFixture f(R"(
class T { static var loaded = false
          static var en = ""
          static var ar = ""
          static var ja = ""
          static var n_en = ""
          static var n_ar = ""
          static var rtl = false
          static var rev = 0
          static var num = ""
          static var date = ""
          static var bad = true }
class Main extends Node {
    fn ready() {
        T.loaded = Locale.load("locale/strings.csv")
        T.en = tr(#start)
        var r0 = Locale.revision()
        T.bad = Locale.set_locale("xx")
        Locale.set_locale("ar")
        T.ar = tr(#start)
        T.rtl = Locale.is_rtl()
        T.n_ar = Locale.tr_plural(#items, 2, [])
        T.num = Locale.format_int(1234)
        T.date = Locale.format_date(2026, 11, 1)
        Locale.set_locale("ja")
        T.ja = Locale.tr_args(#greet, ["Ana"])
        T.n_en = Locale.tr_plural(#items, 3, ["3"])
        T.rev = Locale.revision() - r0
    }
}
)");
    auto g = [&](const char* n) { return f.host->session()->global(n); };
    CHECK(g("T.loaded").b);
    CHECK_FALSE(g("T.bad").b);
    CHECK(g("T.rtl").b);
    CHECK(g("T.rev").i == 2);
    CHECK(f.str("T.en") == "Start");
    CHECK(f.str("T.ar") == "ابدأ");
    CHECK(f.str("T.ja") == "Hello, Ana!");
    CHECK(f.str("T.n_ar") == "عنصران");
    CHECK(f.str("T.n_en") == "3個");
    CHECK(f.str("T.num") == "١٬٢٣٤");
    CHECK(f.str("T.date") == "٠١/١١/٢٠٢٦");
}
