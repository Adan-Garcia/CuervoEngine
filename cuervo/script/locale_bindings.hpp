#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "cuervo/locale/locale.hpp"
#include "pluma/bindings.hpp"

// Natives of cuervo/script/prelude/locale.plm (DESIGN.md 4.19, 11.3). Like
// Save, the state lives beside the VM and is never hashed or replayed:
// translated text is for display only.
namespace cuervo::script {

struct LocaleContext {
    locale::Localizer localizer;
    // Reads a project file as text (Locale.load); empty: loading is refused.
    std::function<bool(std::string_view path, std::string& text, std::string& error)> read_text;
};

void bind_locale(pluma::Bindings& bindings, LocaleContext& context);

} // namespace cuervo::script
