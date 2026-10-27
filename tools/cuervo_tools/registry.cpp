#include "tools/cuervo_tools/registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <span>
#include <sstream>

#include "cuervo/assets/pack.hpp"
#include "cuervo/assets/vfs.hpp"
#include "cuervo/core/sha256.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/script/plugin_host.hpp"
#include "cuervo/script/project.hpp"
#include "pluma/json.hpp"
#include "tools/cuervo_tools/export.hpp"
#include "tools/cuervo_tools/packages.hpp"

namespace cuervo::tools {

namespace fs = std::filesystem;
namespace json = pluma::json;

namespace {

constexpr int index_format = 1;

bool read_file(const fs::path& file, std::vector<std::byte>& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out.resize(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return true;
}

bool write_file(const fs::path& file, std::span<const std::byte> data, std::string& error) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) {
        error = std::format("can't write '{}'", platform::utf8(file));
        return false;
    }
    return true;
}

bool write_text(const fs::path& file, std::string_view text, std::string& error) {
    return write_file(file, std::as_bytes(std::span(text.data(), text.size())), error);
}

std::string current_platform() {
#if defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

// The host library a plugin needs here ("" for a .NET plugin).
const std::string& host_library(const script::PluginSpec& p) {
#if defined(_WIN32)
    return p.lib_windows;
#else
    return p.lib_linux;
#endif
}

// ------------------------------------------------------------ index.json

void pretty(std::string& out, const json::Value& v, int depth) {
    const std::string pad(static_cast<std::size_t>(depth + 1) * 2, ' ');
    const std::string close(static_cast<std::size_t>(depth) * 2, ' ');
    if (v.is_object() && !v.o.empty()) {
        out += "{\n";
        std::size_t i = 0;
        for (const auto& [k, item] : v.o) {
            out += pad;
            json::dump_escaped(out, k);
            out += ": ";
            pretty(out, item, depth + 1);
            out += ++i < v.o.size() ? ",\n" : "\n";
        }
        out += close + "}";
    } else if (v.is_array() && !v.a.empty()) {
        out += "[";
        for (std::size_t i = 0; i < v.a.size(); ++i) {
            if (i > 0) out += ", ";
            json::dump_to(out, v.a[i]);
        }
        out += "]";
    } else {
        json::dump_to(out, v);
    }
}

json::Value to_json(const RegistryEntry& e) {
    json::Object o;
    o["description"] = json::Value::string(e.description);
    o["license"] = json::Value::string(e.license);
    o["engine"] = json::Value::string(e.engine);
    o["archive"] = json::Value::string(e.archive);
    o["sha256"] = json::Value::string(e.sha256);
    json::Object files;
    for (const auto& [path, sum] : e.files) files[path] = json::Value::string(sum);
    o["files"] = json::Value::object(std::move(files));
    json::Object deps;
    for (const auto& [name, version] : e.dependencies) deps[name] = json::Value::string(version);
    o["dependencies"] = json::Value::object(std::move(deps));
    if (e.plugin) {
        json::Object p;
        p["abi"] = json::Value::string(e.abi);
        p["sim_safe"] = json::Value::boolean(e.sim_safe);
        json::Array platforms;
        for (const std::string& s : e.platforms) platforms.push_back(json::Value::string(s));
        p["platforms"] = json::Value::array(std::move(platforms));
        if (!e.conformance_hash.empty()) p["conformance_hash"] = json::Value::string(e.conformance_hash);
        o["plugin"] = json::Value::object(std::move(p));
    }
    return json::Value::object(std::move(o));
}

RegistryEntry from_json(std::string name, std::string version, const json::Value& v) {
    RegistryEntry e;
    e.name = std::move(name);
    e.version = std::move(version);
    e.description = std::string(v.str_or("description"));
    e.license = std::string(v.str_or("license"));
    e.engine = std::string(v.str_or("engine", "*"));
    e.archive = std::string(v.str_or("archive"));
    e.sha256 = std::string(v.str_or("sha256"));
    if (const json::Value* files = v.get("files"); files != nullptr && files->is_object()) {
        for (const auto& [path, sum] : files->o) e.files[path] = sum.s;
    }
    if (const json::Value* deps = v.get("dependencies"); deps != nullptr && deps->is_object()) {
        for (const auto& [n, ver] : deps->o) e.dependencies.emplace_back(n, ver.s);
    }
    if (const json::Value* p = v.get("plugin"); p != nullptr && p->is_object()) {
        e.plugin = true;
        e.abi = std::string(p->str_or("abi"));
        e.sim_safe = p->bool_or("sim_safe");
        e.conformance_hash = std::string(p->str_or("conformance_hash"));
        if (const json::Value* ps = p->get("platforms"); ps != nullptr && ps->is_array()) {
            for (const json::Value& s : ps->a) e.platforms.push_back(s.s);
        }
    }
    return e;
}

// ------------------------------------------------------------ files

bool hidden(const std::string& rel) {
    std::string_view s = rel;
    while (!s.empty()) {
        if (s.front() == '.') return true;
        const std::size_t slash = s.find('/');
        if (slash == std::string_view::npos) break;
        s.remove_prefix(slash + 1);
    }
    return false;
}

void add_tree(const fs::path& root, const std::string& rel_dir, std::set<std::string>& out) {
    std::error_code ec;
    const fs::path dir = root / platform::path_from_utf8(rel_dir);
    if (!fs::is_directory(dir, ec)) return;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = platform::utf8(it->path().lexically_relative(root));
        if (!hidden(rel) && !rel.ends_with(".pdb")) out.insert(fs::path(rel).generic_string());
    }
}

// What an archive of the package holds: the manifest, scripts, the license
// and notices, the plugin's binaries and its conformance project. Sources
// and build trees stay out.
std::set<std::string> archive_files(const fs::path& dir, const script::PackageManifest& m) {
    std::set<std::string> files{"package.toml"};
    add_tree(dir, "scripts", files);
    add_tree(dir, "conformance", files);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string upper = platform::utf8(entry.path().filename());
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (upper.starts_with("LICENSE") || upper.starts_with("COPYING") || upper.starts_with("NOTICE") ||
            upper.starts_with("README") || upper.starts_with("THIRD_PARTY")) {
            files.insert(platform::utf8(entry.path().filename()));
        }
    }
    const script::PluginSpec& p = m.plugin;
    if (p.present) {
        for (const std::string* lib :
             {&p.lib_linux, &p.static_linux, &p.lib_windows, &p.static_windows, &p.static_windows_gnu}) {
            if (!lib->empty() && fs::is_regular_file(dir / platform::path_from_utf8(*lib), ec)) files.insert(*lib);
        }
        if (!p.dotnet.empty()) add_tree(dir, fs::path(p.dotnet).parent_path().generic_string(), files);
    }
    return files;
}

std::string hash16(std::uint64_t h) { return std::format("{:016x}", h); }

// The declared platforms whose library is built in `dir`.
std::vector<std::string> built_platforms(const fs::path& dir, const script::PluginSpec& p) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const std::string& platform : p.platforms) {
        const std::string& lib = p.dotnet.empty() ? (platform == "linux" ? p.lib_linux : p.lib_windows) : p.dotnet;
        if (!lib.empty() && fs::is_regular_file(dir / platform::path_from_utf8(lib), ec)) out.push_back(platform);
    }
    return out;
}

fs::path temp_dir(std::string_view what) {
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec) / std::format("cuervo-{}-{}", what, std::random_device{}());
    fs::create_directories(d, ec);
    return d;
}

// Packs the conformance project and runs it for verify_tick ticks.
std::optional<std::uint64_t> conformance_hash(const fs::path& project, const std::string& font_path,
                                              std::string& error) {
    PackOptions o;
    o.font_path = font_path;
    PackSummary summary;
    auto bytes = pack_project(project, o, summary, error);
    if (!bytes) return std::nullopt;
    const fs::path work = temp_dir("conformance");
    const fs::path pak = work / "conformance.pak";
    std::optional<std::uint64_t> hash;
    if (write_file(pak, *bytes, error)) hash = reference_state_hash(pak, error);
    std::error_code ec;
    fs::remove_all(work, ec);
    return hash;
}

// ------------------------------------------------------------ licenses

bool known_license(std::string_view id) {
    static const std::set<std::string, std::less<>> ids{
        "0BSD",          "AFL-3.0",          "AGPL-3.0-only",     "AGPL-3.0-or-later", "Apache-2.0",
        "Artistic-2.0",  "BSD-1-Clause",     "BSD-2-Clause",      "BSD-3-Clause",      "BSL-1.0",
        "CC-BY-4.0",     "CC-BY-SA-4.0",     "CC0-1.0",           "EPL-2.0",           "EUPL-1.2",
        "FTL",           "GPL-2.0-only",     "GPL-2.0-or-later",  "GPL-3.0-only",      "GPL-3.0-or-later",
        "ISC",           "LGPL-2.1-only",    "LGPL-2.1-or-later", "LGPL-3.0-only",     "LGPL-3.0-or-later",
        "MIT",           "MIT-0",            "MPL-2.0",           "NCSA",              "OFL-1.1",
        "PSF-2.0",       "Unlicense",        "Zlib",
    };
    if (id.starts_with("LicenseRef-") && id.size() > 11) {
        return std::all_of(id.begin(), id.end(),
                           [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.'; });
    }
    std::string_view base = id;
    if (base.ends_with("+")) base.remove_suffix(1);
    return ids.contains(base);
}

bool copyleft(std::string_view license) {
    return license.find("GPL") != std::string_view::npos || license.find("MPL") != std::string_view::npos ||
           license.find("EUPL") != std::string_view::npos || license.find("CC-BY-SA") != std::string_view::npos;
}

// ------------------------------------------------------------ project.toml

// Adds `name = "version"` to [dependencies] (creating the table if needed).
bool pin_dependency(const fs::path& project, const std::string& name, const std::string& version,
                    std::string& error) {
    const fs::path file = project / "project.toml";
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = std::format("can't read '{}'", platform::utf8(file));
        return false;
    }
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    in.close();
    auto trimmed = [](const std::string& s) {
        const std::size_t a = s.find_first_not_of(" \t\r");
        const std::size_t b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    const std::string entry = std::format("{} = \"{}\"", name, version);
    std::optional<std::size_t> header;
    std::size_t end = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trimmed(lines[i]);
        if (t.starts_with("[dependencies." + name + "]")) {
            error = std::format("project.toml already has [dependencies.{}]; remove it to install from a registry",
                                name);
            return false;
        }
        if (t.starts_with("[")) {
            if (header && end == lines.size()) end = i;
            if (t.starts_with("[dependencies]")) header = i;
            continue;
        }
        if (header && end == lines.size()) {
            const std::size_t eq = t.find('=');
            if (eq != std::string::npos && trimmed(t.substr(0, eq)) == name) {
                lines[i] = entry;
                return write_text(file, [&] {
                    std::string out;
                    for (const std::string& l : lines) out += l + "\n";
                    return out;
                }(), error);
            }
        }
    }
    if (header) {
        std::size_t at = end;
        while (at > *header + 1 && trimmed(lines[at - 1]).empty()) --at;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), entry);
    } else {
        if (!lines.empty() && !trimmed(lines.back()).empty()) lines.emplace_back();
        lines.emplace_back("[dependencies]");
        lines.push_back(entry);
    }
    std::string out;
    for (const std::string& l : lines) out += l + "\n";
    return write_text(file, out, error);
}

// package.toml as published: `path` lines of dependency tables are a local
// layout, so they are dropped; installed packages find their dependencies
// vendored beside them.
std::string published_manifest(std::string_view text) {
    std::istringstream in{std::string(text)};
    std::string out;
    bool in_dep_table = false;
    for (std::string line; std::getline(in, line);) {
        std::string t = line;
        t.erase(0, t.find_first_not_of(" \t"));
        if (t.starts_with("[")) in_dep_table = t.starts_with("[dependencies.");
        if (in_dep_table && (t.starts_with("path ") || t.starts_with("path="))) continue;
        out += line + "\n";
    }
    return out;
}

std::vector<std::string> split_spec(std::string_view spec) {
    const std::size_t at = spec.find('@');
    if (at == std::string_view::npos) return {std::string(spec), "*"};
    return {std::string(spec.substr(0, at)), std::string(spec.substr(at + 1))};
}

} // namespace

// ------------------------------------------------------------ Registry

std::optional<Registry> Registry::open(const fs::path& root, std::string& error) {
    Registry r;
    r.root_ = root;
    std::error_code ec;
    const fs::path index = root / "index.json";
    if (!fs::exists(index, ec)) return r;
    std::vector<std::byte> bytes;
    if (!read_file(index, bytes)) {
        error = std::format("can't read '{}'", platform::utf8(index));
        return std::nullopt;
    }
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    auto v = json::Parser(text).parse();
    if (!v || !v->is_object()) {
        error = std::format("'{}' is not a registry index (invalid JSON)", platform::utf8(index));
        return std::nullopt;
    }
    const json::Value* format = v->get("format");
    if (format == nullptr || !format->is_int() || format->i != index_format) {
        error = std::format("'{}': unknown registry format (this cuervo-tools reads format {})",
                            platform::utf8(index), index_format);
        return std::nullopt;
    }
    if (const json::Value* pkgs = v->get("packages"); pkgs != nullptr && pkgs->is_object()) {
        for (const auto& [name, versions] : pkgs->o) {
            for (const auto& [version, entry] : versions.o) r.entries_.push_back(from_json(name, version, entry));
        }
    }
    return r;
}

bool Registry::save(std::string& error) const {
    json::Object packages;
    for (const RegistryEntry& e : entries_) {
        json::Value& versions = packages[e.name];
        versions.kind = json::Kind::Object;
        versions.o[e.version] = to_json(e);
    }
    json::Object top;
    top["format"] = json::Value::integer(index_format);
    top["packages"] = json::Value::object(std::move(packages));
    std::string out;
    pretty(out, json::Value::object(std::move(top)), 0);
    out += "\n";
    return write_text(root_ / "index.json", out, error);
}

const RegistryEntry* Registry::find(std::string_view name, std::string_view version) const {
    for (const RegistryEntry& e : entries_) {
        if (e.name == name && e.version == version) return &e;
    }
    return nullptr;
}

std::vector<const RegistryEntry*> Registry::versions(std::string_view name) const {
    std::vector<const RegistryEntry*> out;
    for (const RegistryEntry& e : entries_) {
        if (e.name == name) out.push_back(&e);
    }
    std::sort(out.begin(), out.end(), [](const RegistryEntry* a, const RegistryEntry* b) {
        return *script::parse_version(a->version) > *script::parse_version(b->version);
    });
    return out;
}

void Registry::put(RegistryEntry entry) {
    for (RegistryEntry& e : entries_) {
        if (e.name == entry.name && e.version == entry.version) {
            e = std::move(entry);
            return;
        }
    }
    entries_.push_back(std::move(entry));
}

// ------------------------------------------------------------ licenses

bool valid_license_expression(std::string_view license, std::string& why) {
    std::string text(license);
    for (char& c : text) {
        if (c == '(' || c == ')') c = ' ';
    }
    std::istringstream in(text);
    std::vector<std::string> tokens;
    for (std::string t; in >> t;) tokens.push_back(t);
    if (tokens.empty()) {
        why = "no license declared (package.toml `license`, an SPDX expression such as \"MIT\")";
        return false;
    }
    bool want_id = true;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        if (want_id) {
            if (!known_license(t)) {
                why = std::format("'{}' is not an SPDX license identifier (e.g. MIT, Apache-2.0, BSD-3-Clause, "
                                  "or LicenseRef-<name> for your own terms)",
                                  t);
                return false;
            }
            want_id = false;
        } else if (t == "OR" || t == "AND") {
            want_id = true;
        } else if (t == "WITH" && i + 1 < tokens.size()) {
            ++i; // the exception's identifier
        } else {
            why = std::format("unexpected '{}' in license \"{}\" (combine identifiers with OR / AND)", t, license);
            return false;
        }
    }
    if (want_id) {
        why = std::format("license \"{}\" ends with an operator", license);
        return false;
    }
    return true;
}

fs::path find_license_file(const fs::path& dir) {
    for (const char* name : {"LICENSE", "LICENSE.md", "LICENSE.txt", "COPYING", "COPYING.md", "COPYING.txt"}) {
        std::error_code ec;
        if (fs::is_regular_file(dir / name, ec)) return dir / name;
    }
    return {};
}

// ------------------------------------------------------------ verify

std::optional<VerifyReport> verify_package(const fs::path& dir, const VerifyOptions& options, std::string& error) {
    auto vfs = assets::Vfs::dev(dir);
    auto manifest = script::load_package_manifest(*vfs, "", error);
    if (!manifest) return std::nullopt;
    const script::PackageManifest& m = *manifest;
    VerifyReport r;
    r.name = m.name;
    r.version = m.version;
    auto check = [&](bool ok, std::string what) { (ok ? r.passed : r.failed).push_back(std::move(what)); };

    // The license: an SPDX expression and its text.
    std::string why;
    if (valid_license_expression(m.license, why)) {
        check(true, std::format("license {}", m.license));
        if (copyleft(m.license)) {
            r.notes.push_back(std::format("{} is a copyleft license: games that ship this package must meet its terms",
                                          m.license));
        }
    } else {
        check(false, why);
    }
    const fs::path license_file = find_license_file(dir);
    check(!license_file.empty(), license_file.empty() ? "no license text (LICENSE, LICENSE.md or COPYING)"
                                                      : std::format("license text {}", platform::utf8(license_file.filename())));

    // Engine and ABI versions.
    const auto engine = *script::parse_version(script::engine_version(), true);
    check(m.engine.contains(engine), m.engine.contains(engine)
                                         ? std::format("engine {} is in {}", script::engine_version(), m.engine.text)
                                         : std::format("needs engine {}; this is engine {}", m.engine.text,
                                                       script::engine_version()));
    const script::PluginSpec& p = m.plugin;
    if (p.present) {
        const int abi = script::plugin_abi_version();
        check(p.abi_compatible(abi), p.abi_compatible(abi)
                                         ? std::format("plugin ABI {} is in {}", abi, p.abi.text)
                                         : std::format("built for plugin ABI {}; this engine has ABI {}", p.abi.text, abi));
        for (const std::string& platform : p.platforms) {
            const bool has = !p.dotnet.empty() || (platform == "linux" ? !p.lib_linux.empty() : !p.lib_windows.empty());
            check(has, has ? std::format("{}: a library is declared", platform)
                           : std::format("'platforms' lists {}, but [plugin] names no library for it", platform));
        }
        const std::vector<std::string> built = built_platforms(dir, p);
        for (const std::string& platform : p.platforms) {
            if (platform != current_platform() && std::find(built.begin(), built.end(), platform) == built.end()) {
                r.notes.push_back(std::format("no {} build in this package: it installs only where a library is "
                                              "built",
                                              platform));
            }
        }
        const bool here = std::find(p.platforms.begin(), p.platforms.end(), current_platform()) != p.platforms.end();
        if (!here) {
            r.notes.push_back(std::format("not built for {}: the library and conformance checks are skipped",
                                          current_platform()));
        } else {
            std::error_code ec;
            const std::string& lib = host_library(p);
            if (!lib.empty()) {
                const bool exists = fs::is_regular_file(dir / platform::path_from_utf8(lib), ec);
                check(exists, exists ? std::format("{} is there", lib)
                                     : std::format("'{}' is missing (build the plugin first)", lib));
            }
            std::vector<script::ResolvedPackage> pkgs{{m, vfs, "", dir}};
            std::string load_error;
            auto set = script::PluginSet::load(pkgs, load_error);
            check(set != nullptr, set != nullptr ? std::format("the plugin loads and registers {} native(s)",
                                                               set->plugins().empty() ? 0 : set->plugins()[0].natives.size())
                                                 : load_error);
        }
        // Determinism: a sim-safe plugin's conformance project reaches the
        // state hash recorded when it was published.
        if (p.sim_safe && here) {
            std::error_code ec;
            const fs::path conf = dir / "conformance";
            if (!fs::exists(conf / "project.toml", ec)) {
                check(false, "a sim-safe plugin needs a conformance project (conformance/project.toml)");
            } else {
                std::string conf_error;
                auto hash = conformance_hash(conf, options.font_path, conf_error);
                if (!hash) {
                    check(false, "the conformance project failed: " + conf_error);
                } else {
                    r.conformance_hash = hash16(*hash);
                    if (options.expected != nullptr && !options.expected->conformance_hash.empty()) {
                        const bool same = options.expected->conformance_hash == r.conformance_hash;
                        check(same, same ? std::format("conformance state hash {} matches the published one",
                                                       r.conformance_hash)
                                         : std::format("conformance state hash {} differs from the published {}: the "
                                                       "plugin is not deterministic here",
                                                       r.conformance_hash, options.expected->conformance_hash));
                    } else {
                        check(true, std::format("conformance project reaches state hash {} at tick {}",
                                                r.conformance_hash, verify_tick));
                    }
                }
            }
        } else if (!p.sim_safe) {
            r.notes.push_back("not sim-safe: its natives must stay out of the simulation, and no conformance hash "
                              "is checked");
        }
    }

    // The scripts check against the engine API (and the plugin's natives).
    std::string check_error;
    auto checked = check_package(dir, check_error);
    check(checked.has_value(), checked ? std::format("{} script(s) check", checked->files) : check_error);

    // The files match the published checksums.
    if (options.expected != nullptr) {
        std::size_t bad = 0;
        for (const auto& [rel, sum] : options.expected->files) {
            std::vector<std::byte> data;
            if (!read_file(dir / platform::path_from_utf8(rel), data) || sha256_hex(data) != sum) {
                r.failed.push_back(std::format("{} doesn't match its published checksum", rel));
                ++bad;
            }
        }
        if (bad == 0) r.passed.push_back(std::format("{} file(s) match their checksums", options.expected->files.size()));
    }
    return r;
}

std::string format_verify_report(const VerifyReport& r) {
    std::string out = std::format("{} {}\n", r.name, r.version);
    for (const std::string& s : r.passed) out += "  ok    " + s + "\n";
    for (const std::string& s : r.failed) out += "  FAIL  " + s + "\n";
    for (const std::string& s : r.notes) out += "  note  " + s + "\n";
    out += r.ok() ? std::format("{} check(s) passed\n", r.passed.size())
                  : std::format("{} check(s) failed, {} passed\n", r.failed.size(), r.passed.size());
    return out;
}

// ------------------------------------------------------------ publish

bool publish_package(const fs::path& dir, const fs::path& registry_root, const std::string& font_path,
                     PublishResult& out, std::string& error) {
    VerifyOptions vo;
    vo.font_path = font_path;
    auto report = verify_package(dir, vo, error);
    if (!report) return false;
    out.report = *report;
    if (!report->ok()) {
        error = "the package doesn't pass its checks:\n" + format_verify_report(*report);
        return false;
    }
    auto vfs = assets::Vfs::dev(dir);
    auto m = script::load_package_manifest(*vfs, "", error);
    if (!m) return false;

    RegistryEntry e;
    e.name = m->name;
    e.version = m->version;
    e.description = m->description;
    e.license = m->license;
    e.engine = m->engine.text.empty() ? "*" : m->engine.text;
    auto registry = Registry::open(registry_root, error);
    if (!registry) return false;
    for (const script::Dependency& d : m->dependencies) {
        if (registry->find(d.name, d.version) == nullptr) {
            error = std::format("dependency {} {} is not in the registry; publish it first", d.name, d.version);
            return false;
        }
        e.dependencies.emplace_back(d.name, d.version);
    }
    if (m->plugin.present) {
        e.plugin = true;
        e.abi = m->plugin.abi.text;
        e.sim_safe = m->plugin.sim_safe;
        // Only where a library is built: install picks a version by platform.
        e.platforms = built_platforms(dir, m->plugin);
        if (e.platforms.empty()) {
            error = std::format("{} {}: no library is built for any platform it lists; build the plugin first", e.name,
                                e.version);
            return false;
        }
        e.conformance_hash = report->conformance_hash;
    }
    assets::PackBuilder archive(assets::PackPlatform::Any);
    for (const std::string& rel : archive_files(dir, *m)) {
        std::vector<std::byte> data;
        if (!read_file(dir / platform::path_from_utf8(rel), data)) {
            error = std::format("can't read '{}'", platform::utf8(dir / platform::path_from_utf8(rel)));
            return false;
        }
        if (rel == "package.toml") {
            const std::string text =
                published_manifest(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
            data.assign(reinterpret_cast<const std::byte*>(text.data()),
                        reinterpret_cast<const std::byte*>(text.data()) + text.size());
        }
        e.files[rel] = sha256_hex(data);
        if (!archive.add(rel, std::move(data), error)) return false;
    }
    const std::vector<std::byte> bytes = archive.build();
    e.sha256 = sha256_hex(bytes);
    e.archive = std::format("packages/{0}/{0}-{1}.cpkg", e.name, e.version);
    if (const RegistryEntry* old = registry->find(e.name, e.version)) {
        if (old->sha256 == e.sha256) {
            out.entry = *old;
            out.already = true;
            return true;
        }
        error = std::format("{} {} is already published with different contents; publish a new version", e.name,
                            e.version);
        return false;
    }
    if (!write_file(registry_root / platform::path_from_utf8(e.archive), bytes, error)) return false;
    out.entry = e;
    registry->put(std::move(e));
    return registry->save(error);
}

// ------------------------------------------------------------ install

namespace {

struct Installer {
    const fs::path& project;
    const Registry& registry;
    const std::string& font_path;
    InstallResult& out;
    std::string& error;
    std::vector<std::string> stack;

    // The newest version of `name` in `range` that works here.
    const RegistryEntry* choose(const std::string& name, const script::VersionRange& range) {
        const auto engine = *script::parse_version(script::engine_version(), true);
        std::vector<std::string> skipped;
        for (const RegistryEntry* e : registry.versions(name)) {
            if (!range.contains(*script::parse_version(e->version))) continue;
            std::string why;
            auto engines = script::parse_version_range(e->engine, why);
            if (!engines || !engines->contains(engine)) {
                skipped.push_back(std::format("{} needs engine {}", e->version, e->engine));
                continue;
            }
            if (e->plugin && std::find(e->platforms.begin(), e->platforms.end(), current_platform()) ==
                                 e->platforms.end()) {
                skipped.push_back(std::format("{} has no {} build", e->version, current_platform()));
                continue;
            }
            return e;
        }
        if (registry.versions(name).empty()) {
            error = std::format("no package '{}' in the registry at '{}'", name, platform::utf8(registry.root()));
        } else {
            error = std::format("no version of '{}' matches \"{}\" for engine {} on {}", name, range.text,
                                script::engine_version(), current_platform());
            for (const std::string& s : skipped) error += "\n  " + s;
        }
        return nullptr;
    }

    bool install(const std::string& name, const script::VersionRange& range) {
        if (std::find(stack.begin(), stack.end(), name) != stack.end()) {
            error = std::format("dependency cycle through '{}'", name);
            return false;
        }
        const RegistryEntry* e = choose(name, range);
        if (e == nullptr) return false;

        // Already in the project?
        auto vfs = assets::Vfs::dev(project);
        auto proj = script::load_project(*vfs, error);
        if (!proj) return false;
        for (const script::Dependency& d : proj->dependencies) {
            if (d.name != name) continue;
            if (d.version == e->version && d.path.empty()) return true; // nothing to do
            error = std::format("the project already depends on {} {}{}; remove it from project.toml first", name,
                                d.version, d.path.empty() ? "" : " (by path)");
            return false;
        }
        const fs::path dest = project / "packages" / name;
        std::error_code ec;
        if (fs::exists(dest, ec)) {
            error = std::format("'{}' already exists; remove it to install {} {}", platform::utf8(dest), name,
                                e->version);
            return false;
        }

        stack.push_back(name);
        for (const auto& [dep, version] : e->dependencies) {
            std::string why;
            auto exact = script::parse_version_range(version, why);
            if (!exact || !install(dep, *exact)) return false;
        }
        stack.pop_back();

        // The archive, checked.
        std::vector<std::byte> bytes;
        const fs::path archive = registry.root() / platform::path_from_utf8(e->archive);
        if (!read_file(archive, bytes)) {
            error = std::format("can't read '{}'", platform::utf8(archive));
            return false;
        }
        if (sha256_hex(bytes) != e->sha256) {
            error = std::format("'{}' doesn't match its checksum in the index: the download is damaged or was "
                                "changed after publishing",
                                platform::utf8(archive));
            return false;
        }
        auto pack = assets::Pack::from_bytes(std::move(bytes), error);
        if (!pack) return false;
        for (const assets::PackEntry& entry : pack->entries()) {
            const std::string rel(entry.path);
            auto data = pack->find(rel);
            auto it = e->files.find(rel);
            if (it == e->files.end() || sha256_hex(*data) != it->second) {
                error = std::format("{} {}: '{}' is not in the index's file list or doesn't match its checksum", name,
                                    e->version, rel);
                fs::remove_all(dest, ec);
                return false;
            }
            if (!write_file(dest / platform::path_from_utf8(rel), *data, error)) {
                fs::remove_all(dest, ec);
                return false;
            }
#if !defined(_WIN32)
            if (rel.ends_with(".so")) {
                fs::permissions(dest / platform::path_from_utf8(rel), fs::perms::owner_exec | fs::perms::group_exec |
                                                                        fs::perms::others_exec,
                                fs::perm_options::add, ec);
            }
#endif
        }

        // Checked again here, against what was published.
        VerifyOptions vo;
        vo.font_path = font_path;
        vo.expected = e;
        auto report = verify_package(dest, vo, error);
        if (!report || !report->ok()) {
            if (report) error = std::format("{} {} doesn't pass its checks on this machine:\n{}", name, e->version,
                                            format_verify_report(*report));
            fs::remove_all(dest, ec);
            return false;
        }
        if (!pin_dependency(project, name, e->version, error)) {
            fs::remove_all(dest, ec);
            return false;
        }
        out.reports.push_back(*report);
        out.installed.push_back(std::format("{} {}", name, e->version));
        return true;
    }
};

} // namespace

bool install_package(const fs::path& project, std::string_view spec, const fs::path& registry_root,
                     const std::string& font_path, InstallResult& out, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(project / "project.toml", ec)) {
        error = std::format("'{}' has no project.toml", platform::utf8(project));
        return false;
    }
    const std::vector<std::string> parts = split_spec(spec);
    if (!script::valid_package_name(parts[0])) {
        error = std::format("'{}' is not a package name", parts[0]);
        return false;
    }
    auto range = script::parse_version_range(parts[1], error);
    if (!range) return false;
    auto registry = Registry::open(registry_root, error);
    if (!registry) return false;
    Installer installer{project, *registry, font_path, out, error, {}};
    if (!installer.install(parts[0], *range)) return false;
    // The project still resolves as a whole.
    auto vfs = assets::Vfs::dev(project);
    auto proj = script::load_project(*vfs, error);
    if (!proj) return false;
    std::vector<script::ResolvedPackage> packages;
    return script::resolve_packages(*proj, *vfs, packages, error);
}

// ------------------------------------------------------------ notices

namespace {

struct PackageNotice {
    std::string name, version, license, origin;
    fs::path license_file;
};

std::optional<std::vector<PackageNotice>> package_notices(const fs::path& project, std::string& error) {
    auto vfs = assets::Vfs::dev(project);
    auto proj = script::load_project(*vfs, error);
    if (!proj) return std::nullopt;
    std::vector<script::ResolvedPackage> packages;
    if (!script::resolve_packages(*proj, *vfs, packages, error)) return std::nullopt;
    std::vector<PackageNotice> out;
    for (const script::ResolvedPackage& p : packages) {
        PackageNotice n;
        n.name = p.manifest.name;
        n.version = p.manifest.version;
        n.license = p.manifest.license.empty() ? "(not declared)" : p.manifest.license;
        n.origin = p.prefix.empty() ? "path dependency" : "vendored";
        if (p.manifest.plugin.present) n.origin += ", native plugin";
        n.license_file = find_license_file(p.dir);
        out.push_back(std::move(n));
    }
    std::sort(out.begin(), out.end(), [](const PackageNotice& a, const PackageNotice& b) { return a.name < b.name; });
    return out;
}

} // namespace

std::optional<std::string> license_report(const fs::path& project, std::string& error) {
    auto notices = package_notices(project, error);
    if (!notices) return std::nullopt;
    std::string out = "# Package licenses\n\n";
    if (notices->empty()) {
        out += "This game uses no packages. The engine's own notices are in THIRD_PARTY_NOTICES.md.\n";
        return out;
    }
    out += "The packages this game uses, each under its own license. Their license texts are in\n"
           "`packages/`; the engine's own notices are in THIRD_PARTY_NOTICES.md.\n\n"
           "| Package | Version | License | Kind | License text |\n|---|---|---|---|---|\n";
    for (const PackageNotice& n : *notices) {
        out += std::format("| {} | {} | {} | {} | {} |\n", n.name, n.version, n.license, n.origin,
                           n.license_file.empty() ? std::string("(none)") : std::format("packages/{}.txt", n.name));
    }
    return out;
}

bool write_package_notices(const fs::path& project, const fs::path& out_dir, std::vector<fs::path>& written,
                           std::string& error) {
    auto notices = package_notices(project, error);
    if (!notices) return false;
    if (notices->empty()) return true;
    auto report = license_report(project, error);
    if (!report) return false;
    const fs::path dest = out_dir / "licenses";
    if (!write_text(dest / "PACKAGES.md", *report, error)) return false;
    written.push_back(dest / "PACKAGES.md");
    for (const PackageNotice& n : *notices) {
        if (n.license_file.empty()) continue;
        std::vector<std::byte> data;
        if (!read_file(n.license_file, data)) {
            error = std::format("can't read '{}'", platform::utf8(n.license_file));
            return false;
        }
        const fs::path file = dest / "packages" / std::format("{}.txt", n.name);
        if (!write_file(file, data, error)) return false;
        written.push_back(file);
    }
    return true;
}

} // namespace cuervo::tools
