// The orbit plugin's natives (scripts/orbit.plm), over the C++ wrapper of the
// plugin ABI. Only IEEE arithmetic, so the plugin is sim-safe (package.toml).

#include <cctype>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/plugin/cuervo_plugin.hpp"

namespace {

constexpr float tau = 6.28318530717958647692f;

} // namespace

CUERVO_PLUGIN(orbit) {
    plugin.function("advance", [](float angle, float speed, float dt) {
        // std::fmod is exact (IEEE remainder of the division), so this is
        // deterministic everywhere.
        float a = std::fmod(angle + speed * dt, tau);
        return a < 0.0f ? a + tau : a;
    });
    plugin.function("orbits_in", [](float seconds, float speed) {
        return static_cast<std::int32_t>(seconds * speed / tau);
    });
    plugin.function("describe", [] { return std::string("orbit 1.0.0 (C++)"); });
    plugin.function("Orbiter.energy", [](cuervo::plugin::Object self) {
        const double r = self.get_float("radius");
        const double w = self.get_float("speed");
        return static_cast<float>(0.5 * (r * w) * (r * w));
    });
    // The smallest importer: "notes/a.upper" is read as "notes/a.txt",
    // upper-cased. In development the VFS converts on read; export stores
    // the converted file instead (docs/PLUGINS.md "Importers").
    plugin.importer(".upper", ".txt", [](std::string_view, std::span<const std::uint8_t> data) {
        std::vector<std::uint8_t> out(data.begin(), data.end());
        for (auto& c : out) c = static_cast<std::uint8_t>(std::toupper(c));
        return out;
    });
}
