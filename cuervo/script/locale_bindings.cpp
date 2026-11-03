#include "cuervo/script/locale_bindings.hpp"

#include <vector>

#include "cuervo/core/log.hpp"

namespace cuervo::script {

namespace {

namespace nv = pluma::native;
using pluma::NativeCall;
using pluma::Value;

std::vector<std::string> string_list(const NativeCall& c, std::uint32_t i) {
    std::vector<std::string> out;
    if (!c.args[i].is_obj()) return out;
    for (const Value& v : static_cast<const pluma::ListObj*>(c.args[i].o)->items) {
        out.push_back(static_cast<const pluma::StrObj*>(v.o)->s);
    }
    return out;
}

} // namespace

void bind_locale(pluma::Bindings& b, LocaleContext& ctx) {
    LocaleContext* s = &ctx;

    b.add_native("Locale.load", [s](NativeCall& c) {
        const std::string path(nv::str(c, 0));
        std::string text, error;
        if (!s->read_text || !s->read_text(path, text, error)) {
            LOG_WARN("Locale.load('{}'): {}", path, s->read_text ? error : "no project files here");
            return Value::boolean(false);
        }
        const int added = s->localizer.load_csv(text, error);
        if (added < 0) LOG_WARN("Locale.load('{}'): {}", path, error);
        return Value::boolean(added >= 0);
    });
    b.add_native("Locale.add", [s](NativeCall& c) {
        s->localizer.add(nv::str(c, 0), nv::str(c, 1), std::string(nv::str(c, 2)));
        return Value{};
    });
    b.add_native("Locale.set_locale", [s](NativeCall& c) {
        const bool ok = s->localizer.set_locale(nv::str(c, 0));
        if (!ok) LOG_WARN("Locale.set_locale('{}'): no table for that locale", nv::str(c, 0));
        return Value::boolean(ok);
    });
    b.add_native("Locale.set_fallback", [s](NativeCall& c) {
        s->localizer.set_fallback(nv::str(c, 0));
        return Value{};
    });
    b.add_native("Locale.current", [s](NativeCall&) { return nv::str_result(s->localizer.locale()); });
    b.add_native("Locale.has_locale", [s](NativeCall& c) { return Value::boolean(s->localizer.has_locale(nv::str(c, 0))); });
    b.add_native("Locale.locales", [s](NativeCall&) {
        auto* list = pluma::new_list();
        Value out = Value::object(list);
        pluma::retain(out);
        for (const std::string& id : s->localizer.locales()) {
            Value v = pluma::str_value(id);
            pluma::retain(v);
            list->items.push_back(v);
        }
        return out;
    });
    b.add_native("Locale.is_rtl", [s](NativeCall&) { return Value::boolean(s->localizer.rtl()); });
    b.add_native("Locale.revision", [s](NativeCall&) { return Value::integer(pluma::NumKind::I64, static_cast<std::int64_t>(s->localizer.revision())); });
    b.add_native("Locale.has", [s](NativeCall& c) { return Value::boolean(s->localizer.has(nv::name(c, 0))); });
    b.add_native("Locale.tr", [s](NativeCall& c) { return nv::str_result(s->localizer.tr(nv::name(c, 0))); });
    b.add_native("Locale.tr_args", [s](NativeCall& c) {
        const std::vector<std::string> args = string_list(c, 1);
        return nv::str_result(locale::format_text(s->localizer.tr(nv::name(c, 0)), args));
    });
    b.add_native("Locale.tr_plural", [s](NativeCall& c) {
        const std::vector<std::string> args = string_list(c, 2);
        return nv::str_result(locale::format_text(s->localizer.tr_plural(nv::name(c, 0), nv::integer(c, 1)), args));
    });
    b.add_native("Locale.format_int", [s](NativeCall& c) {
        return nv::str_result(locale::format_int(s->localizer.locale(), nv::integer(c, 0)));
    });
    b.add_native("Locale.format_float", [s](NativeCall& c) {
        return nv::str_result(locale::format_float(s->localizer.locale(), static_cast<double>(nv::f32(c, 0)),
                                                   static_cast<int>(nv::integer(c, 1))));
    });
    b.add_native("Locale.format_date", [s](NativeCall& c) {
        return nv::str_result(locale::format_date(s->localizer.locale(), static_cast<int>(nv::integer(c, 0)),
                                                  static_cast<int>(nv::integer(c, 1)), static_cast<int>(nv::integer(c, 2))));
    });
    b.add_native("Locale.format_time", [s](NativeCall& c) {
        return nv::str_result(locale::format_time(s->localizer.locale(), static_cast<int>(nv::integer(c, 0)),
                                                  static_cast<int>(nv::integer(c, 1))));
    });
}

} // namespace cuervo::script
