#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Localization (DESIGN.md 4.19 "11.3"): string tables keyed by name per locale,
// CLDR-style plural categories, positional format arguments, and
// locale-aware number / date / time formatting.
//
// None of this is simulation state: the active locale is never hashed or
// replayed, and the formatting uses floating point and the host's text rules.
// Game logic must not branch on a translated string or a formatted number.
namespace cuervo::locale {

enum class Plural { Zero, One, Two, Few, Many, Other };

// The CLDR plural category of integer `n` for a language ("en", "ar", "ru",
// "pt-BR" ...; unknown languages follow English).
Plural plural_category(std::string_view locale, std::int64_t n);
std::string_view plural_name(Plural p); // "zero", "one", ...

// True for right-to-left locales (Arabic, Hebrew, Persian, Urdu).
bool is_rtl(std::string_view locale);

// "pt-BR" -> "pt"; "en" -> "en".
std::string_view base_language(std::string_view locale);

// Number and date formatting per locale (digits, separators, field order).
std::string format_int(std::string_view locale, std::int64_t value);
std::string format_float(std::string_view locale, double value, int decimals);
std::string format_date(std::string_view locale, int year, int month, int day); // short numeric form
std::string format_time(std::string_view locale, int hour, int minute);         // 12 or 24 hour per locale

// Replaces {0}, {1} ... with args (out of range indices stay as written);
// "{{" and "}}" produce braces.
std::string format_text(std::string_view text, std::span<const std::string> args);

// Translation tables for any number of locales plus the active locale.
class Localizer {
public:
    // Registers `text` for (`locale`, `key`). A plural form is the key with a
    // category suffix: "items.one", "items.other". Replaces an earlier entry.
    void add(std::string_view locale, std::string_view key, std::string text);

    // Loads a CSV table: header `key,<locale>,<locale>...` then one row per
    // key. Fields may be quoted (RFC 4180: "" escapes a quote, newlines allowed
    // inside quotes); `#` rows are comments; empty cells are skipped (so the
    // fallback locale fills the gap). Returns the number of cells added, or -1
    // with `error` set.
    int load_csv(std::string_view csv, std::string& error);

    // Selects the active locale; false (nothing changes) if no table has it.
    // Exact id first, then its base language ("pt-BR" matches a "pt" table).
    bool set_locale(std::string_view locale);
    const std::string& locale() const { return locale_; }
    // The locale used when the active one lacks a key (default "en").
    void set_fallback(std::string_view locale) { fallback_ = std::string(locale); }
    const std::string& fallback() const { return fallback_; }
    bool has_locale(std::string_view locale) const { return tables_.contains(locale); }
    std::vector<std::string> locales() const;
    bool rtl() const { return is_rtl(locale_); }

    // Counts every change of active locale or table content, so a UI can tell
    // when to re-layout its text.
    std::uint64_t revision() const { return revision_; }

    // The text of `key` in the active locale, then its base language, then the
    // fallback locale; the key itself (and a recorded miss) if none has it.
    std::string tr(std::string_view key) const;
    // As tr() but for the plural form of `n`; "key.<category>", then
    // "key.other", then "key".
    std::string tr_plural(std::string_view key, std::int64_t n) const;
    bool has(std::string_view key) const;

    // Keys that were asked for and not found (sorted, for a development warning).
    std::vector<std::string> missing_keys() const;

private:
    const std::string* find(std::string_view locale, std::string_view key) const;
    const std::string* lookup(std::string_view key) const;

    using Table = std::map<std::string, std::string, std::less<>>;
    std::map<std::string, Table, std::less<>> tables_;
    std::string locale_ = "en";
    std::string fallback_ = "en";
    std::uint64_t revision_ = 0;
    mutable std::map<std::string, int, std::less<>> missing_;
};

} // namespace cuervo::locale
