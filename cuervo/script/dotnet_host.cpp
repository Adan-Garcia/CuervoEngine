#include "cuervo/script/dotnet_host.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

#include "cuervo/platform/platform.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace cuervo::script {

namespace fs = std::filesystem;

namespace {

// hostfxr's C API (the .NET hosting documentation, hostfxr.h and
// coreclr_delegates.h): stable across .NET versions, declared here so no
// SDK headers are needed to build.
#if defined(_WIN32)
using char_t = wchar_t;
#define CUERVO_HOSTFXR_CALL __cdecl
#else
using char_t = char;
#define CUERVO_HOSTFXR_CALL
#endif
using hostfxr_handle = void*;
struct hostfxr_initialize_parameters {
    std::size_t size;
    const char_t* host_path;
    const char_t* dotnet_root;
};
constexpr int hdt_load_assembly_and_get_function_pointer = 5;
using init_fn = std::int32_t(CUERVO_HOSTFXR_CALL*)(const char_t*, const hostfxr_initialize_parameters*,
                                                   hostfxr_handle*);
using get_delegate_fn = std::int32_t(CUERVO_HOSTFXR_CALL*)(hostfxr_handle, int, void**);
using close_fn = std::int32_t(CUERVO_HOSTFXR_CALL*)(hostfxr_handle);
using load_assembly_fn = int(CUERVO_HOSTFXR_CALL*)(const char_t* assembly_path, const char_t* type_name,
                                                   const char_t* method_name, const char_t* delegate_type_name,
                                                   void* reserved, void** delegate);
const char_t* const unmanaged_callers_only = reinterpret_cast<const char_t*>(-1);

std::basic_string<char_t> native(const fs::path& p) { return p.native(); }
std::basic_string<char_t> native(std::string_view s) {
#if defined(_WIN32)
    return platform::path_from_utf8(s).native();
#else
    return std::string(s);
#endif
}

struct Fxr {
    void* lib = nullptr;
    init_fn init = nullptr;
    get_delegate_fn get_delegate = nullptr;
    close_fn close = nullptr;
};

void* open_lib(const fs::path& p) {
#if defined(_WIN32)
    return ::LoadLibraryW(p.c_str());
#else
    return ::dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* sym(void* lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return ::dlsym(lib, name);
#endif
}

template <class F>
F as_fn(void* p) {
    F f = nullptr;
    std::memcpy(&f, &p, sizeof f);
    return f;
}

// The highest version under <root>/host/fxr.
fs::path hostfxr_path(const fs::path& root) {
    std::error_code ec;
    std::vector<std::pair<std::vector<int>, fs::path>> found;
    for (const auto& e : fs::directory_iterator(root / "host" / "fxr", ec)) {
        std::vector<int> v;
        std::string name = platform::utf8(e.path().filename());
        for (std::size_t i = 0; i < name.size();) {
            std::size_t j = name.find('.', i);
            v.push_back(std::atoi(name.substr(i, j - i).c_str()));
            if (j == std::string::npos) break;
            i = j + 1;
        }
#if defined(_WIN32)
        fs::path lib = e.path() / "hostfxr.dll";
#else
        fs::path lib = e.path() / "libhostfxr.so";
#endif
        if (fs::exists(lib, ec)) found.emplace_back(std::move(v), lib);
    }
    if (found.empty()) return {};
    std::sort(found.begin(), found.end());
    return found.back().second;
}

std::mutex g_mutex;
Fxr g_fxr;
load_assembly_fn g_load = nullptr;

} // namespace

fs::path dotnet_root() {
    std::error_code ec;
    if (const auto env = platform::env("DOTNET_ROOT"); env && !env->empty()) {
        fs::path p = platform::path_from_utf8(*env);
        if (fs::exists(p / "host" / "fxr", ec)) return p;
    }
    if (const auto path = platform::env("PATH")) {
#if defined(_WIN32)
        const char sep = ';';
        const char* exe = "dotnet.exe";
#else
        const char sep = ':';
        const char* exe = "dotnet";
#endif
        std::string_view rest(*path);
        while (!rest.empty()) {
            std::size_t i = rest.find(sep);
            fs::path candidate = platform::path_from_utf8(rest.substr(0, i)) / exe;
            if (fs::exists(candidate, ec)) {
                fs::path real = fs::canonical(candidate, ec);
                if (!ec && fs::exists(real.parent_path() / "host" / "fxr", ec)) return real.parent_path();
            }
            if (i == std::string_view::npos) break;
            rest.remove_prefix(i + 1);
        }
    }
#if defined(_WIN32)
    const char* candidates[] = {"C:/Program Files/dotnet"};
#else
    const char* candidates[] = {"/usr/share/dotnet", "/usr/lib/dotnet", "/usr/local/share/dotnet", "/opt/dotnet"};
#endif
    for (const char* c : candidates) {
        if (fs::exists(fs::path(c) / "host" / "fxr", ec)) return c;
    }
    if (const auto home = platform::env("HOME")) {
        fs::path p = platform::path_from_utf8(*home) / ".dotnet";
        if (fs::exists(p / "host" / "fxr", ec)) return p;
    }
    return {};
}

bool load_dotnet_entry(const fs::path& assembly, std::string_view type_name, cuervo_plugin_entry_fn& entry,
                       std::string& error) {
    std::lock_guard lock(g_mutex);
    std::error_code ec;
    if (!fs::exists(assembly, ec)) {
        error = std::format("no .NET assembly '{}' (build the plugin first)", platform::utf8(assembly));
        return false;
    }
    if (g_load == nullptr) {
        const fs::path root = dotnet_root();
        if (root.empty()) {
            error = "no .NET runtime found (install .NET, or set DOTNET_ROOT)";
            return false;
        }
        const fs::path fxr = hostfxr_path(root);
        if (fxr.empty()) {
            error = std::format("no hostfxr under '{}'", platform::utf8(root / "host" / "fxr"));
            return false;
        }
        g_fxr.lib = open_lib(fxr);
        if (g_fxr.lib == nullptr) {
            error = std::format("can't load '{}'", platform::utf8(fxr));
            return false;
        }
        g_fxr.init = as_fn<init_fn>(sym(g_fxr.lib, "hostfxr_initialize_for_runtime_config"));
        g_fxr.get_delegate = as_fn<get_delegate_fn>(sym(g_fxr.lib, "hostfxr_get_runtime_delegate"));
        g_fxr.close = as_fn<close_fn>(sym(g_fxr.lib, "hostfxr_close"));
        if (g_fxr.init == nullptr || g_fxr.get_delegate == nullptr || g_fxr.close == nullptr) {
            error = std::format("'{}' lacks the hosting API", platform::utf8(fxr));
            return false;
        }
        // The first plugin's runtimeconfig.json picks the runtime; later
        // plugins must be compatible with it (one runtime per process).
        fs::path config = assembly;
        config.replace_extension(".runtimeconfig.json");
        const auto root_native = native(root);
        hostfxr_initialize_parameters params{sizeof(hostfxr_initialize_parameters), nullptr, root_native.c_str()};
        hostfxr_handle ctx = nullptr;
        const std::int32_t rc = g_fxr.init(native(config).c_str(), &params, &ctx);
        if (rc < 0 || ctx == nullptr) {
            error = std::format("can't start .NET for '{}' (hostfxr error 0x{:x}; is '{}' there?)",
                                platform::utf8(assembly), static_cast<std::uint32_t>(rc), platform::utf8(config));
            if (ctx != nullptr) g_fxr.close(ctx);
            return false;
        }
        void* load = nullptr;
        const std::int32_t rd = g_fxr.get_delegate(ctx, hdt_load_assembly_and_get_function_pointer, &load);
        g_fxr.close(ctx);
        if (rd < 0 || load == nullptr) {
            error = std::format(".NET: no assembly loader (hostfxr error 0x{:x})", static_cast<std::uint32_t>(rd));
            return false;
        }
        g_load = as_fn<load_assembly_fn>(load);
    }
    void* fn = nullptr;
    const int rc = g_load(native(assembly).c_str(), native(type_name).c_str(), native(std::string_view("Entry")).c_str(),
                          unmanaged_callers_only, nullptr, &fn);
    if (rc < 0 || fn == nullptr) {
        error = std::format(".NET: no [UnmanagedCallersOnly] '{}.Entry' in '{}' (error 0x{:x})", type_name,
                            platform::utf8(assembly), static_cast<std::uint32_t>(rc));
        return false;
    }
    entry = as_fn<cuervo_plugin_entry_fn>(fn);
    return true;
}

} // namespace cuervo::script
