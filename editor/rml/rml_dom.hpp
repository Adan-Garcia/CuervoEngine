#pragma once
// Small DOM lookups the shell's translation units share.
#include <RmlUi/Core.h>

#include <string>
#include <string_view>

#include "editor/rml_widgets.hpp"

namespace cuervo::editor::dom {

// The nearest element at or above `e` that has attribute `attr`.
inline Rml::Element* find_attr(Rml::Element* e, const char* attr) {
    for (; e != nullptr; e = e->GetParentNode()) if (e->HasAttribute(attr)) return e;
    return nullptr;
}
inline bool inside(Rml::Element* e, const char* id) {
    for (; e != nullptr; e = e->GetParentNode()) if (e->GetId() == id) return true;
    return false;
}
inline bool tid_is(Rml::Element* t, const char* id) { return t != nullptr && t->GetId() == id; }
inline bool typing(Rml::Context* ctx) {
    Rml::Element* f = ctx != nullptr ? ctx->GetFocusElement() : nullptr;
    return f != nullptr && f->GetTagName() == "input";
}
inline std::string icon_img(std::string_view name, int size = 16, std::string_view cls = {}) { return w::icon(name, size, cls); }

} // namespace cuervo::editor::dom
