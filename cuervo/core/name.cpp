#include "cuervo/core/name.hpp"

#include <deque>
#include <string>
#include <unordered_map>

namespace cuervo {

namespace {

struct InternTable {
    // deque keeps string addresses stable, so the map can key on views into it.
    std::deque<std::string> strings{std::string()};
    std::unordered_map<std::string_view, std::uint32_t> ids{{std::string_view(), 0u}};
};

InternTable& table() {
    static InternTable instance;
    return instance;
}

} // namespace

Name::Name(std::string_view text) {
    InternTable& t = table();
    if (auto it = t.ids.find(text); it != t.ids.end()) {
        id_ = it->second;
        return;
    }
    id_ = static_cast<std::uint32_t>(t.strings.size());
    const std::string& stored = t.strings.emplace_back(text);
    t.ids.emplace(stored, id_);
}

std::string_view Name::str() const { return table().strings[id_]; }

} // namespace cuervo
