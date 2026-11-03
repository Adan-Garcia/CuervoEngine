#include "cuervo/locale/locale.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace cuervo::locale {

namespace {

bool in(std::int64_t v, std::int64_t lo, std::int64_t hi) { return v >= lo && v <= hi; }

bool lang_is(std::string_view lang, std::initializer_list<std::string_view> set) {
    return std::ranges::find(set, lang) != set.end();
}

// Arabic-Indic digits and separators (the ar-EG convention).
constexpr std::string_view arabic_digits[10] = {"٠", "١", "٢", "٣", "٤",
                                                "٥", "٦", "٧", "٨", "٩"};

struct NumberStyle {
    std::string_view decimal = ".";
    std::string_view group = ",";
    bool arabic = false;
};

NumberStyle number_style(std::string_view locale) {
    const std::string_view l = base_language(locale);
    if (l == "ar") return {"٫", "٬", true};
    if (lang_is(l, {"de", "es", "it", "nl", "pt", "id", "tr", "da"})) return {",", "."};
    if (l == "fr") return {",", " "};
    if (lang_is(l, {"ru", "uk", "pl", "cs", "sv", "fi", "nb", "no"})) return {",", " "};
    return {};
}

std::string localize_digits(std::string_view ascii, const NumberStyle& style) {
    if (!style.arabic) return std::string(ascii);
    std::string out;
    for (const char c : ascii) {
        if (c >= '0' && c <= '9') out += arabic_digits[c - '0'];
        else out += c;
    }
    return out;
}

// Groups ASCII digits in threes with the locale separator (digits localized by the caller).
std::string group_digits(const std::string& digits, const NumberStyle& style) {
    std::string out;
    const std::size_t n = digits.size();
    for (std::size_t i = 0; i < n; ++i) {
        out += digits[i];
        const std::size_t left = n - i - 1;
        if (left > 0 && left % 3 == 0) out += style.group;
    }
    return out;
}

std::string two(int v) { return std::format("{:02}", v); }

} // namespace

std::string_view base_language(std::string_view locale) {
    const std::size_t cut = locale.find_first_of("-_");
    return cut == std::string_view::npos ? locale : locale.substr(0, cut);
}

Plural plural_category(std::string_view locale, std::int64_t n) {
    const std::string_view l = base_language(locale);
    if (n < 0) n = -n;
    if (lang_is(l, {"ja", "zh", "ko", "vi", "th", "id", "ms"})) return Plural::Other;
    if (l == "fr" || l == "pt") return n <= 1 ? Plural::One : Plural::Other;
    if (l == "ar") {
        if (n == 0) return Plural::Zero;
        if (n == 1) return Plural::One;
        if (n == 2) return Plural::Two;
        if (in(n % 100, 3, 10)) return Plural::Few;
        if (in(n % 100, 11, 99)) return Plural::Many;
        return Plural::Other;
    }
    if (l == "ru" || l == "uk") {
        if (n % 10 == 1 && n % 100 != 11) return Plural::One;
        if (in(n % 10, 2, 4) && !in(n % 100, 12, 14)) return Plural::Few;
        return Plural::Many;
    }
    if (l == "pl") {
        if (n == 1) return Plural::One;
        if (in(n % 10, 2, 4) && !in(n % 100, 12, 14)) return Plural::Few;
        return Plural::Many;
    }
    return n == 1 ? Plural::One : Plural::Other;
}

std::string_view plural_name(Plural p) {
    switch (p) {
    case Plural::Zero: return "zero";
    case Plural::One: return "one";
    case Plural::Two: return "two";
    case Plural::Few: return "few";
    case Plural::Many: return "many";
    case Plural::Other: break;
    }
    return "other";
}

bool is_rtl(std::string_view locale) { return lang_is(base_language(locale), {"ar", "he", "fa", "ur"}); }

std::string format_int(std::string_view locale, std::int64_t value) {
    const NumberStyle style = number_style(locale);
    const bool neg = value < 0;
    const std::uint64_t mag = neg ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    return (neg ? "-" : "") + localize_digits(group_digits(std::to_string(mag), style), style);
}

std::string format_float(std::string_view locale, double value, int decimals) {
    decimals = std::clamp(decimals, 0, 12);
    const NumberStyle style = number_style(locale);
    const std::string raw = std::format("{:.{}f}", std::fabs(value), decimals);
    const std::size_t dot = raw.find('.');
    // No minus sign for a value that rounds to zero.
    std::string out = value < 0 && raw.find_first_not_of("0.") != std::string::npos ? "-" : "";
    out += localize_digits(group_digits(raw.substr(0, dot), style), style);
    if (dot != std::string::npos) {
        out += style.decimal;
        out += localize_digits(raw.substr(dot + 1), style);
    }
    return out;
}

std::string format_date(std::string_view locale, int year, int month, int day) {
    const std::string_view l = base_language(locale);
    std::string out;
    if (l == "en" && locale != "en-GB") out = std::format("{}/{}/{}", month, day, year);
    else if (lang_is(l, {"ja", "zh", "ko"})) out = std::format("{}/{}/{}", year, month, day);
    else if (lang_is(l, {"de", "ru", "pl", "uk", "tr"})) out = std::format("{}.{:02}.{}", day, month, year);
    else if (lang_is(l, {"sv", "fi"})) out = std::format("{}-{:02}-{:02}", year, month, day);
    else out = std::format("{:02}/{:02}/{}", day, month, year);
    return localize_digits(out, number_style(locale));
}

std::string format_time(std::string_view locale, int hour, int minute) {
    const std::string_view l = base_language(locale);
    const int h12 = hour % 12 == 0 ? 12 : hour % 12;
    if (l == "ar") {
        return localize_digits(std::format("{}:{}", h12, two(minute)), number_style(locale)) +
               (hour >= 12 ? " م" : " ص");
    }
    if (l == "en" && locale != "en-GB") return std::format("{}:{} {}", h12, two(minute), hour >= 12 ? "PM" : "AM");
    return std::format("{}:{}", l == "ja" ? std::to_string(hour) : two(hour), two(minute));
}

std::string format_text(std::string_view text, std::span<const std::string> args) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if ((c == '{' || c == '}') && i + 1 < text.size() && text[i + 1] == c) {
            out += c;
            ++i;
        } else if (c == '{') {
            const std::size_t close = text.find('}', i);
            bool digits = close != std::string_view::npos && close > i + 1 && close - i <= 6;
            std::size_t index = 0;
            for (std::size_t k = i + 1; digits && k < close; ++k) {
                if (text[k] < '0' || text[k] > '9') digits = false;
                else index = index * 10 + static_cast<std::size_t>(text[k] - '0');
            }
            if (digits && index < args.size()) {
                out += args[index];
                i = close;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

void Localizer::add(std::string_view locale, std::string_view key, std::string text) {
    auto table = tables_.find(locale);
    if (table == tables_.end()) table = tables_.emplace(std::string(locale), Table{}).first;
    table->second[std::string(key)] = std::move(text);
    ++revision_;
}

namespace {

// Splits CSV into rows of fields; blank lines are skipped.
bool parse_csv(std::string_view s, std::vector<std::vector<std::string>>& rows, std::string& error) {
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    bool was_quoted = false;
    bool any = false; // the current row has content
    auto end_field = [&] {
        row.push_back(std::move(field));
        field.clear();
        was_quoted = false;
    };
    auto end_row = [&] {
        end_field();
        if (any) rows.push_back(std::move(row));
        row.clear();
        any = false;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quoted) {
            if (c == '"' && i + 1 < s.size() && s[i + 1] == '"') {
                field += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                field += c;
            }
        } else if (c == '"' && field.empty() && !was_quoted) {
            quoted = true;
            was_quoted = true;
            any = true;
        } else if (c == ',') {
            end_field();
            any = true;
        } else if (c == '\n') {
            end_row();
        } else if (c != '\r') {
            field += c;
            any = true;
        }
    }
    if (quoted) {
        error = "unterminated quoted field";
        return false;
    }
    if (any) end_row();
    return true;
}

} // namespace

int Localizer::load_csv(std::string_view csv, std::string& error) {
    if (csv.starts_with("\xEF\xBB\xBF")) csv.remove_prefix(3); // UTF-8 BOM from spreadsheet exports
    std::vector<std::vector<std::string>> rows;
    if (!parse_csv(csv, rows, error)) return -1;
    std::erase_if(rows, [](const auto& r) { return !r.empty() && r[0].starts_with('#'); });
    if (rows.empty() || rows[0].size() < 2 || rows[0][0] != "key") {
        error = "the first row must be: key,<locale>,<locale>...";
        return -1;
    }
    const std::vector<std::string> header = rows[0];
    int added = 0;
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (row.size() > header.size()) {
            error = std::format("row {} has {} cells for {} columns", r + 1, row.size(), header.size());
            return -1;
        }
        if (row[0].empty()) continue;
        for (std::size_t c = 1; c < row.size(); ++c) {
            if (row[c].empty()) continue;
            add(header[c], row[0], row[c]);
            ++added;
        }
    }
    return added;
}

bool Localizer::set_locale(std::string_view locale) {
    std::string chosen;
    if (tables_.contains(locale)) chosen = std::string(locale);
    else if (tables_.contains(base_language(locale))) chosen = std::string(base_language(locale));
    else return false;
    if (chosen != locale_) {
        locale_ = std::move(chosen);
        ++revision_;
    }
    return true;
}

std::vector<std::string> Localizer::locales() const {
    std::vector<std::string> out;
    for (const auto& [id, table] : tables_) out.push_back(id);
    return out;
}

const std::string* Localizer::find(std::string_view locale, std::string_view key) const {
    const auto table = tables_.find(locale);
    if (table == tables_.end()) return nullptr;
    const auto it = table->second.find(key);
    return it == table->second.end() ? nullptr : &it->second;
}

const std::string* Localizer::lookup(std::string_view key) const {
    if (const std::string* s = find(locale_, key)) return s;
    if (const std::string* s = find(base_language(locale_), key)) return s;
    return find(fallback_, key);
}

std::string Localizer::tr(std::string_view key) const {
    if (const std::string* s = lookup(key)) return *s;
    ++missing_[std::string(key)];
    return std::string(key);
}

bool Localizer::has(std::string_view key) const { return lookup(key) != nullptr; }

std::string Localizer::tr_plural(std::string_view key, std::int64_t n) const {
    const std::string name = std::format("{}.{}", key, plural_name(plural_category(locale_, n)));
    if (const std::string* s = lookup(name)) return *s;
    const std::string other = std::format("{}.other", key);
    if (const std::string* s = lookup(other)) return *s;
    return tr(key);
}

std::vector<std::string> Localizer::missing_keys() const {
    std::vector<std::string> out;
    for (const auto& [k, n] : missing_) out.push_back(k);
    return out;
}

} // namespace cuervo::locale
