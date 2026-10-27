#include "cuervo/script/plugin_host.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <fstream>

#include "cuervo/assets/vfs.hpp"
#include "cuervo/core/log.hpp"
#include "cuervo/platform/platform.hpp"
#include "cuervo/script/dotnet_host.hpp"
#include "pluma/aot.hpp"
#include "pluma/bindings.hpp"
#include "pluma/bytecode.hpp"
#include "pluma/vm.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif


// The registrar and call handles of the ABI are these structs.
struct cuervo_registrar {
    cuervo::script::LoadedPlugin* plugin;
    std::vector<cuervo::script::PluginImporter>* importers;
};

struct cuervo_call {
    pluma::NativeCall* native;
    pluma::Value result;
    bool panicked = false;
    std::string message;
};

struct cuervo_buffer {
    std::vector<std::byte> bytes;
};

namespace cuervo::script {

namespace fs = std::filesystem;
using pluma::Tag;
using pluma::Value;

namespace {

// ------------------------------------------------------------ the host table

const Value* arg(const cuervo_call* c, std::uint32_t i) {
    return i < c->native->count ? &c->native->args[i] : nullptr;
}

std::int64_t h_arg_int(const cuervo_call* c, std::uint32_t i) {
    const Value* v = arg(c, i);
    if (v == nullptr) return 0;
    if (v->tag == Tag::F32) return static_cast<std::int64_t>(v->f);
    if (v->tag == Tag::F64) return static_cast<std::int64_t>(v->d);
    if (v->tag == Tag::Bool) return v->b ? 1 : 0;
    return v->i;
}

double h_arg_float(const cuervo_call* c, std::uint32_t i) {
    const Value* v = arg(c, i);
    if (v == nullptr) return 0.0;
    if (v->tag == Tag::F32) return static_cast<double>(v->f);
    if (v->tag == Tag::F64) return v->d;
    if (pluma::is_num_tag(v->tag)) return static_cast<double>(v->i);
    return 0.0;
}

int h_arg_bool(const cuervo_call* c, std::uint32_t i) {
    const Value* v = arg(c, i);
    return v != nullptr && v->tag == Tag::Bool && v->b ? 1 : 0;
}

const char* h_arg_str(const cuervo_call* c, std::uint32_t i, std::size_t* length) {
    const Value* v = arg(c, i);
    std::string_view s;
    if (v != nullptr && v->tag == Tag::Name) {
        s = c->native->vm.name_text(static_cast<std::uint32_t>(v->i));
    } else if (v != nullptr && v->tag == Tag::Obj && v->o->kind == pluma::ObjKind::Str) {
        s = static_cast<const pluma::StrObj*>(v->o)->s;
    }
    if (length != nullptr) *length = s.size();
    return s.data() != nullptr ? s.data() : "";
}

std::uint32_t h_arg_vec(const cuervo_call* c, std::uint32_t i, float* out, std::uint32_t max) {
    const Value* v = arg(c, i);
    if (v == nullptr || !pluma::is_vec(*v)) return 0;
    const std::uint32_t n = pluma::vec_size(*v);
    const float* d = pluma::vec_data(*v);
    for (std::uint32_t k = 0; k < n && k < max; ++k) out[k] = d[k];
    return n;
}

cuervo_object* h_arg_object(const cuervo_call* c, std::uint32_t i) {
    const Value* v = arg(c, i);
    if (v == nullptr || v->tag != Tag::Obj || v->o->kind != pluma::ObjKind::Instance) return nullptr;
    return reinterpret_cast<cuervo_object*>(v->o);
}

int h_arg_is_null(const cuervo_call* c, std::uint32_t i) {
    const Value* v = arg(c, i);
    return v == nullptr || v->tag == Tag::Null || v->tag == Tag::Nil ? 1 : 0;
}

std::uint32_t h_arg_count(const cuervo_call* c) { return c->native->count; }

void set(cuervo_call* c, Value v) {
    pluma::release(c->result);
    c->result = v;
}
void h_ret_bool(cuervo_call* c, int v) { set(c, Value::boolean(v != 0)); }
void h_ret_i32(cuervo_call* c, std::int32_t v) { set(c, Value::integer(pluma::NumKind::I32, v)); }
void h_ret_i64(cuervo_call* c, std::int64_t v) { set(c, Value::integer(pluma::NumKind::I64, v)); }
void h_ret_f32(cuervo_call* c, float v) { set(c, Value::f32(v)); }
void h_ret_f64(cuervo_call* c, double v) { set(c, Value::f64(v)); }
void h_ret_str(cuervo_call* c, const char* s, std::size_t n) {
    set(c, pluma::native::owned(pluma::str_value(std::string(s != nullptr ? s : "", s != nullptr ? n : 0))));
}
void h_ret_vec(cuervo_call* c, const float* d, std::uint32_t n) {
    set(c, pluma::native::owned(pluma::vec_value(d, n)));
}
void h_ret_object(cuervo_call* c, cuervo_object* o) {
    if (o == nullptr) {
        set(c, Value::null());
        return;
    }
    set(c, pluma::native::owned(Value::object(reinterpret_cast<pluma::Object*>(o))));
}
void h_panic(cuervo_call* c, const char* message) {
    c->panicked = true;
    c->message = message != nullptr ? message : "panic in a plugin";
}

pluma::InstanceObj* inst(cuervo_object* o) { return reinterpret_cast<pluma::InstanceObj*>(o); }

void* h_object_native(cuervo_object* o) { return o != nullptr ? inst(o)->native : nullptr; }
void h_object_set_native(cuervo_object* o, void* data) {
    if (o != nullptr) inst(o)->native = data;
}
const char* h_object_type(cuervo_object* o) { return o != nullptr ? inst(o)->type->name.c_str() : ""; }

Value* field(cuervo_object* o, const char* name) {
    if (o == nullptr || name == nullptr) return nullptr;
    pluma::InstanceObj* x = inst(o);
    const auto& names = x->type->field_names;
    for (std::size_t i = 0; i < names.size() && i < x->fields.size(); ++i) {
        if (names[i] == name) return &x->fields[i];
    }
    return nullptr;
}

int h_get_float(cuervo_object* o, const char* name, double* out) {
    Value* v = field(o, name);
    if (v == nullptr || (v->tag != Tag::F32 && v->tag != Tag::F64)) return 1;
    *out = v->tag == Tag::F32 ? static_cast<double>(v->f) : v->d;
    return 0;
}
int h_set_float(cuervo_object* o, const char* name, double value) {
    Value* v = field(o, name);
    if (v == nullptr || (v->tag != Tag::F32 && v->tag != Tag::F64)) return 1;
    if (v->tag == Tag::F32) {
        v->f = static_cast<float>(value);
    } else {
        v->d = value;
    }
    return 0;
}
bool is_int_tag(Tag t) { return t >= Tag::I8 && t <= Tag::U64; }
int h_get_int(cuervo_object* o, const char* name, std::int64_t* out) {
    Value* v = field(o, name);
    if (v == nullptr || !is_int_tag(v->tag)) return 1;
    *out = v->i;
    return 0;
}
int h_set_int(cuervo_object* o, const char* name, std::int64_t value) {
    Value* v = field(o, name);
    if (v == nullptr || !is_int_tag(v->tag)) return 1;
    v->i = value;
    return 0;
}
int h_get_vec(cuervo_object* o, const char* name, float* out, std::uint32_t max, std::uint32_t* size) {
    Value* v = field(o, name);
    if (v == nullptr || !pluma::is_vec(*v)) return 1;
    const std::uint32_t n = pluma::vec_size(*v);
    const float* d = pluma::vec_data(*v);
    for (std::uint32_t k = 0; k < n && k < max; ++k) out[k] = d[k];
    if (size != nullptr) *size = n;
    return 0;
}
int h_set_vec(cuervo_object* o, const char* name, const float* d, std::uint32_t n) {
    Value* v = field(o, name);
    if (v == nullptr || !pluma::is_vec(*v) || pluma::vec_size(*v) != n) return 1;
    Value next = pluma::vec_value(d, n);
    pluma::retain(next);
    pluma::release(*v);
    *v = next;
    return 0;
}

void h_buffer_write(cuervo_buffer* out, const std::uint8_t* data, std::size_t size) {
    if (out == nullptr || data == nullptr) return;
    const auto* b = reinterpret_cast<const std::byte*>(data);
    out->bytes.insert(out->bytes.end(), b, b + size);
}

void h_log(int level, const char* message) {
    const char* m = message != nullptr ? message : "";
    if (level >= CUERVO_LOG_ERROR) {
        LOG_ERROR("plugin: {}", m);
    } else if (level == CUERVO_LOG_WARN) {
        LOG_WARN("plugin: {}", m);
    } else {
        LOG_INFO("plugin: {}", m);
    }
}

int h_register_native(cuervo_registrar* reg, const char* name, cuervo_native_fn fn, void* user) {
    if (reg == nullptr || name == nullptr || fn == nullptr) return 1;
    reg->plugin->natives.push_back({name, fn, user});
    return 0;
}
int h_register_finalizer(cuervo_registrar* reg, const char* class_name, cuervo_finalizer_fn fn) {
    if (reg == nullptr || class_name == nullptr || fn == nullptr) return 1;
    reg->plugin->finalizers.emplace_back(class_name, fn);
    return 0;
}
int h_register_importer(cuervo_registrar* reg, const char* ext, const char* out_ext, cuervo_import_fn fn,
                        void* user) {
    if (reg == nullptr || ext == nullptr || out_ext == nullptr || fn == nullptr || ext[0] != '.' || out_ext[0] != '.') {
        return 1;
    }
    reg->importers->push_back({reg->plugin->name, ext, out_ext, fn, user});
    return 0;
}

const cuervo_host g_host = [] {
    cuervo_host h{};
    h.abi_version = CUERVO_PLUGIN_ABI_VERSION;
    h.size = sizeof(cuervo_host);
    h.engine_version = CUERVO_ENGINE_VERSION;
    h.register_native = h_register_native;
    h.register_finalizer = h_register_finalizer;
    h.register_importer = h_register_importer;
    h.arg_count = h_arg_count;
    h.arg_int = h_arg_int;
    h.arg_float = h_arg_float;
    h.arg_bool = h_arg_bool;
    h.arg_str = h_arg_str;
    h.arg_vec = h_arg_vec;
    h.arg_object = h_arg_object;
    h.arg_is_null = h_arg_is_null;
    h.ret_bool = h_ret_bool;
    h.ret_i32 = h_ret_i32;
    h.ret_i64 = h_ret_i64;
    h.ret_f32 = h_ret_f32;
    h.ret_f64 = h_ret_f64;
    h.ret_str = h_ret_str;
    h.ret_vec = h_ret_vec;
    h.ret_object = h_ret_object;
    h.panic = h_panic;
    h.object_native = h_object_native;
    h.object_set_native = h_object_set_native;
    h.object_type = h_object_type;
    h.object_get_float = h_get_float;
    h.object_set_float = h_set_float;
    h.object_get_int = h_get_int;
    h.object_set_int = h_set_int;
    h.object_get_vec = h_get_vec;
    h.object_set_vec = h_set_vec;
    h.buffer_write = h_buffer_write;
    h.log = h_log;
    return h;
}();

std::string platform_key() {
#if defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

std::atomic<std::uint64_t> g_temp_counter{0};

std::uint64_t process_id() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

} // namespace

const cuervo_host& plugin_host_api() { return g_host; }

// A shared library, unloaded with the set (after every script session that
// could call into it is gone: the set outlives its hosts).
struct PluginSet::Library {
    void* handle = nullptr;
    fs::path temp; // extracted from a pack: removed on unload
    ~Library() {
#if defined(_WIN32)
        if (handle != nullptr) ::FreeLibrary(static_cast<HMODULE>(handle));
#else
        if (handle != nullptr) ::dlclose(handle);
#endif
        if (!temp.empty()) {
            std::error_code ec;
            fs::remove(temp, ec);
        }
    }
    void* symbol(const char* name) const {
#if defined(_WIN32)
        return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
        return ::dlsym(handle, name);
#endif
    }
};

PluginSet::PluginSet() = default;
PluginSet::~PluginSet() = default;

bool PluginSet::add_entry(std::string name, std::string version, bool sim_safe, cuervo_plugin_entry_fn entry,
                          std::string& error, std::string kind) {
    LoadedPlugin p;
    p.name = std::move(name);
    p.version = std::move(version);
    p.sim_safe = sim_safe;
    p.kind = std::move(kind);
    std::vector<PluginImporter> importers;
    cuervo_registrar reg{&p, &importers};
    const int code = entry(&g_host, &reg);
    if (code != 0) {
        error = std::format("plugin '{}' refused to load (entry returned {})", p.name, code);
        return false;
    }
    for (const LoadedPlugin::Native& n : p.natives) {
        const std::string prefix = p.name + ".";
        if (!n.name.starts_with(prefix)) {
            error = std::format("plugin '{}' registered native '{}' outside its namespace (names start with '{}')",
                                p.name, n.name, prefix);
            return false;
        }
    }
    importers_.insert(importers_.end(), importers.begin(), importers.end());
    plugins_.push_back(std::move(p));
    return true;
}

bool PluginSet::load_library(const ResolvedPackage& pkg, std::string& error) {
    const PluginSpec& spec = pkg.manifest.plugin;
    const std::string& name = pkg.manifest.name;
    if (!spec.abi_compatible(CUERVO_PLUGIN_ABI_VERSION)) {
        error = std::format("plugin '{}' {} was built for plugin ABI {}; this engine has ABI {}", name,
                            pkg.manifest.version, spec.abi.text, CUERVO_PLUGIN_ABI_VERSION);
        return false;
    }
#if defined(_WIN32)
    const std::string& rel = spec.lib_windows;
#else
    const std::string& rel = spec.lib_linux;
#endif
    if (rel.empty()) {
        error = std::format("plugin '{}' has no library for {} ([plugin] {} = \"...\" in its package.toml)", name,
                            platform_key(), platform_key());
        return false;
    }
#if defined(__EMSCRIPTEN__)
    error = std::format("plugin '{}': native plugin libraries are not available on the web yet (docs/PLUGINS.md)",
                        name);
    return false;
#else
    auto lib = std::make_unique<Library>();
    fs::path file;
    if (!package_file(pkg, rel, false, file, error)) return false;
    if (pkg.dir.empty()) lib->temp = file;
#endif
#if defined(__EMSCRIPTEN__)
#elif defined(_WIN32)
    lib->handle = ::LoadLibraryW(file.c_str());
    if (lib->handle == nullptr) {
        error = std::format("plugin '{}': can't load '{}' (error {})", name, platform::utf8(file), ::GetLastError());
        return false;
    }
#else
    lib->handle = ::dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (lib->handle == nullptr) {
        const char* why = ::dlerror();
        error = std::format("plugin '{}': can't load '{}': {}", name, platform::utf8(file), why != nullptr ? why : "");
        return false;
    }
#endif
#if !defined(__EMSCRIPTEN__)
    const std::string entry_name = "cuervo_plugin_entry_" + name;
    void* sym = lib->symbol(entry_name.c_str());
    if (sym == nullptr) {
        error = std::format("plugin '{}': '{}' has no '{}'", name, platform::utf8(file), entry_name);
        return false;
    }
    cuervo_plugin_entry_fn entry = nullptr;
    std::memcpy(&entry, &sym, sizeof entry);
    libraries_.push_back(std::move(lib));
    return add_entry(name, pkg.manifest.version, spec.sim_safe, entry, error, "native");
#endif
}

bool PluginSet::package_file(const ResolvedPackage& pkg, const std::string& rel, bool whole_dir, fs::path& out,
                             std::string& error) {
    const std::string& name = pkg.manifest.name;
    if (!pkg.dir.empty()) {
        out = pkg.dir / platform::path_from_utf8(rel);
        return true;
    }
    // Inside a pack: extracted to a temporary directory (a library must be a
    // file to load; a .NET assembly needs its .json files beside it).
    const fs::path dir = fs::temp_directory_path() / std::format("cuervo_plugin_{}_{}_{}", process_id(),
                                                                ++g_temp_counter, name);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string parent = fs::path(rel).parent_path().generic_string();
    std::vector<std::string> files{pkg.prefix + rel};
    if (whole_dir) files = pkg.vfs->list(pkg.prefix + parent);
    for (const std::string& f : files) {
        std::string read_error;
        auto bytes = pkg.vfs->open(f, read_error);
        if (!bytes) {
            error = std::format("plugin '{}': {}", name, read_error);
            return false;
        }
        const fs::path dest = dir / platform::path_from_utf8(fs::path(f).filename().generic_string());
        std::ofstream o(dest, std::ios::binary | std::ios::trunc);
        const auto data = bytes->bytes();
        if (!o || !o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
            error = std::format("plugin '{}': can't write '{}'", name, platform::utf8(dest));
            return false;
        }
    }
    out = dir / platform::path_from_utf8(fs::path(rel).filename().generic_string());
    return true;
}

bool PluginSet::load_dotnet(const ResolvedPackage& pkg, std::string& error) {
    const PluginSpec& spec = pkg.manifest.plugin;
    const std::string& name = pkg.manifest.name;
    if (!spec.abi_compatible(CUERVO_PLUGIN_ABI_VERSION)) {
        error = std::format("plugin '{}' {} was built for plugin ABI {}; this engine has ABI {}", name,
                            pkg.manifest.version, spec.abi.text, CUERVO_PLUGIN_ABI_VERSION);
        return false;
    }
#if defined(__EMSCRIPTEN__)
    error = std::format("plugin '{}' is a .NET plugin: C# plugins run only on desktop (docs/PLUGINS.md)", name);
    return false;
#else
    fs::path assembly;
    if (!package_file(pkg, spec.dotnet, true, assembly, error)) return false;
    const std::string stem = platform::utf8(assembly.stem());
    const std::string type = spec.dotnet_type.empty() ? std::format("{}.Plugin, {}", stem, stem) : spec.dotnet_type;
    cuervo_plugin_entry_fn entry = nullptr;
    if (!load_dotnet_entry(assembly, type, entry, error)) {
        error = std::format("plugin '{}': {}", name, error);
        return false;
    }
    return add_entry(name, pkg.manifest.version, spec.sim_safe, entry, error, "dotnet");
#endif
}

std::shared_ptr<PluginSet> PluginSet::load(const std::vector<ResolvedPackage>& packages, std::string& error) {
    auto set = std::make_shared<PluginSet>();
    if (!set->load_more(packages, error)) return nullptr;
    return set;
}

bool PluginSet::add_aot_plugins(const pluma::AotModule& module, std::string& error) {
    struct Entry {
        const char* name;
        const char* version;
        int sim_safe;
        cuervo_plugin_entry_fn entry;
    };
    const auto* table = static_cast<const Entry*>(module.symbol("cuervo_aot_plugins"));
    for (const Entry* e = table; e != nullptr && e->name != nullptr; ++e) {
        if (!add_entry(e->name, e->version, e->sim_safe != 0, e->entry, error, "static")) return false;
    }
    return true;
}

bool PluginSet::load_more(const std::vector<ResolvedPackage>& packages, std::string& error) {
    PluginSet* set = this;
    for (const ResolvedPackage& pkg : packages) {
        if (!pkg.manifest.plugin.present) continue;
        if (std::any_of(plugins_.begin(), plugins_.end(),
                        [&](const LoadedPlugin& p) { return p.name == pkg.manifest.name; })) {
            continue;
        }
#if defined(_WIN32)
        const bool has_native = !pkg.manifest.plugin.lib_windows.empty();
#else
        const bool has_native = !pkg.manifest.plugin.lib_linux.empty();
#endif
        if (!has_native && !pkg.manifest.plugin.dotnet.empty()) {
            if (!set->load_dotnet(pkg, error)) return false;
            continue;
        }
        if (!set->load_library(pkg, error)) return false;
    }
    for (const std::string& name : set->unsafe_plugins()) {
        LOG_WARN("plugin '{}' is not sim-safe: its natives may make replays and state hashes differ", name);
    }
    return true;
}

void PluginSet::bind(pluma::Bindings& bindings) const {
    for (const LoadedPlugin& p : plugins_) {
        for (const LoadedPlugin::Native& n : p.natives) {
            cuervo_native_fn fn = n.fn;
            void* user = n.user;
            bindings.add_native(n.name, [fn, user](pluma::NativeCall& nc) -> Value {
                cuervo_call call{&nc, Value{}, false, {}};
                fn(&call, user);
                if (call.panicked) {
                    pluma::release(call.result);
                    nc.vm.panic(std::move(call.message));
                }
                return call.result;
            });
        }
        for (const auto& [cls, fn] : p.finalizers) bindings.add_finalizer(cls, fn);
    }
}

std::vector<std::string> PluginSet::unsafe_plugins() const {
    std::vector<std::string> out;
    for (const LoadedPlugin& p : plugins_) {
        if (!p.sim_safe) out.push_back(p.name);
    }
    return out;
}

const PluginImporter* PluginSet::importer_for(std::string_view path) const {
    for (const PluginImporter& imp : importers_) {
        if (path.size() > imp.extension.size() && path.ends_with(imp.extension)) return &imp;
    }
    return nullptr;
}

bool PluginSet::run_importer(const PluginImporter& importer, std::string_view path, std::span<const std::byte> data,
                             std::vector<std::byte>& out, std::string& error) {
    cuervo_buffer buffer;
    const std::string p(path);
    const int code = importer.fn(importer.user, p.c_str(), reinterpret_cast<const std::uint8_t*>(data.data()),
                                 data.size(), &buffer);
    if (code != 0) {
        error = std::format("{}: the '{}' importer failed: {}", p, importer.plugin,
                            std::string(reinterpret_cast<const char*>(buffer.bytes.data()), buffer.bytes.size()));
        return false;
    }
    out = std::move(buffer.bytes);
    return true;
}

void add_importers(const std::shared_ptr<const PluginSet>& set, assets::Vfs& vfs) {
    if (vfs.packed()) return;
    for (const PluginImporter& imp : set->importers()) {
        // The set keeps the plugin's library loaded while the VFS can call it.
        vfs.add_derived(imp.extension, imp.output_extension,
                        [set, imp](std::string_view path, std::span<const std::byte> in, std::vector<std::byte>& out,
                                    std::string& error) { return PluginSet::run_importer(imp, path, in, out, error); });
    }
}

std::shared_ptr<PluginSet> load_project_plugins(const assets::Vfs& vfs, std::string& error,
                                                const pluma::AotModule* aot) {
    auto set = std::make_shared<PluginSet>();
    if (aot != nullptr && !set->add_aot_plugins(*aot, error)) return nullptr;
    if (!vfs.exists("project.toml")) return set;
    auto project = load_project(vfs, error);
    if (!project) return nullptr;
    std::vector<ResolvedPackage> packages;
    if (!resolve_packages(*project, vfs, packages, error)) return nullptr;
    if (!set->load_more(packages, error)) return nullptr;
    return set;
}

std::optional<std::vector<StaticPlugin>> static_plugins(const std::vector<ResolvedPackage>& packages,
                                                        std::string& error) {
    std::vector<StaticPlugin> out;
    for (const ResolvedPackage& pkg : packages) {
        const PluginSpec& spec = pkg.manifest.plugin;
        if (!spec.present) continue;
#if defined(__MINGW32__)
        const std::string& rel = spec.static_windows_gnu;
        const bool native = !spec.lib_windows.empty();
#elif defined(_WIN32)
        const std::string& rel = spec.static_windows;
        const bool native = !spec.lib_windows.empty();
#else
        const std::string& rel = spec.static_linux;
        const bool native = !spec.lib_linux.empty();
#endif
        if (!native && !spec.dotnet.empty()) {
            error = std::format("plugin '{}' is a .NET (C#) plugin, which AOT exports can't include yet; export with "
                                "--scripts source (DESIGN.md 4.18)",
                                pkg.manifest.name);
            return std::nullopt;
        }
        if (rel.empty() || pkg.dir.empty()) continue; // loaded as a shared library from the pack
        const fs::path lib = pkg.dir / platform::path_from_utf8(rel);
        std::error_code ec;
        if (!fs::exists(lib, ec)) {
            error = std::format("plugin '{}': no static library '{}' (build the plugin first)", pkg.manifest.name,
                                platform::utf8(lib));
            return std::nullopt;
        }
        out.push_back({pkg.manifest.name, pkg.manifest.version, spec.sim_safe, lib});
    }
    return out;
}

} // namespace cuervo::script
