/*
 * Cuervo Engine plugin ABI (DESIGN.md 4.18, milestone 10.5c; docs/PLUGINS.md).
 *
 * The one stable boundary between the engine and native plugins. Plain C so
 * that any language with a C FFI can implement a plugin: the C++ wrapper
 * (cuervo/plugin/cuervo_plugin.hpp), the Rust crate (bindings/rust) and the
 * C# host (bindings/dotnet) are all built over this header.
 *
 * A plugin is a package (docs/PACKAGES.md) whose package.toml has a
 * [plugin] section. Its Pluma API is the package's scripts: `@native`
 * declarations there are implemented by the library, registered under their
 * qualified names ("noise.value", "noise.Field.sample"). Scripts reach them
 * through `import noise` like any package.
 *
 * The library exports one entry point, named after the package:
 *
 *     CUERVO_PLUGIN_EXPORT int cuervo_plugin_entry_noise(const cuervo_host* host,
 *                                                       cuervo_registrar* reg);
 *
 * The engine calls it once, before any script is compiled. The plugin
 * registers its natives (and finalizers and importers) through `host` and
 * returns 0, or nonzero to refuse to load. Everything it registers stays
 * valid until the library is unloaded.
 *
 * Versioning: CUERVO_PLUGIN_ABI_VERSION changes only when existing entries
 * change meaning. New host functions are appended to `cuervo_host`, whose
 * `size` tells a plugin which ones exist. A plugin checks `host->abi_version`
 * and refuses to load on a mismatch; the engine refuses a plugin whose
 * package.toml asks for another ABI.
 *
 * Rules for native code (DESIGN.md 4.1): a plugin declared `sim_safe` must
 * be deterministic: no wall clock, no unseeded randomness, no unordered
 * iteration in results, and floating point without fast-math or FMA
 * contraction, so replays and state hashes hold. Natives must not throw
 * across this boundary or keep pointers from a call after it returns
 * (strings, vectors, call handles); objects stay valid while the script
 * holds them.
 */
#ifndef CUERVO_PLUGIN_H
#define CUERVO_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUERVO_PLUGIN_ABI_VERSION 1

#if defined(_WIN32)
#define CUERVO_PLUGIN_EXPORT __declspec(dllexport)
#else
#define CUERVO_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* One call of a native function: its arguments and its result. */
typedef struct cuervo_call cuervo_call;
/* A script object (a class or struct instance) passed to or held by a native. */
typedef struct cuervo_object cuervo_object;
/* Registration handle, valid only during the entry call. */
typedef struct cuervo_registrar cuervo_registrar;
/* Output of an importer. */
typedef struct cuervo_buffer cuervo_buffer;

/* A native function. `user` is the pointer given at registration. Set the
 * result with one of the ret_* functions (none for a function returning
 * nothing), or report an error with `panic`. */
typedef void (*cuervo_native_fn)(cuervo_call* call, void* user);

/* Frees an object's native data (object_set_native) when the object dies. */
typedef void (*cuervo_finalizer_fn)(void* native_data);

/* Converts a source asset (`data`) to the engine format named at
 * registration, written to `out` with host->buffer_write. `path` is the
 * asset's project path, for messages. Returns 0 on success; on failure,
 * writes a message to `out` and returns nonzero. Importers run at pack time
 * and when a game in development loads the asset; they must be pure. */
typedef int (*cuervo_import_fn)(void* user, const char* path, const uint8_t* data, size_t size, cuervo_buffer* out);

/* Log levels for host->log. */
enum { CUERVO_LOG_INFO = 0, CUERVO_LOG_WARN = 1, CUERVO_LOG_ERROR = 2 };

typedef struct cuervo_host {
    uint32_t abi_version; /* CUERVO_PLUGIN_ABI_VERSION of the engine */
    uint32_t size;        /* sizeof(cuervo_host) of the engine: later fields exist only if they fit */
    const char* engine_version;

    /* ---- registration (with the entry's `reg`); 0 on success */
    int (*register_native)(cuervo_registrar* reg, const char* qualified_name, cuervo_native_fn fn, void* user);
    int (*register_finalizer)(cuervo_registrar* reg, const char* class_name, cuervo_finalizer_fn fn);
    /* `extension` is the source's (".csv"), `output_extension` the engine format's (".scene"). */
    int (*register_importer)(cuervo_registrar* reg, const char* extension, const char* output_extension,
                             cuervo_import_fn fn, void* user);

    /* ---- arguments: self first for a method. Numbers convert from any numeric argument. */
    uint32_t (*arg_count)(const cuervo_call* call);
    int64_t (*arg_int)(const cuervo_call* call, uint32_t index);
    double (*arg_float)(const cuervo_call* call, uint32_t index);
    int (*arg_bool)(const cuervo_call* call, uint32_t index);
    /* UTF-8 bytes of a `str` argument, or the text of a `name` (#tag); valid during the call. */
    const char* (*arg_str)(const cuervo_call* call, uint32_t index, size_t* length);
    /* Copies up to `max` components of a vector argument to `out`; returns its size. */
    uint32_t (*arg_vec)(const cuervo_call* call, uint32_t index, float* out, uint32_t max);
    /* A class or struct argument; NULL for null. */
    cuervo_object* (*arg_object)(const cuervo_call* call, uint32_t index);
    int (*arg_is_null)(const cuervo_call* call, uint32_t index);

    /* ---- results: the setter must match the declared return type */
    void (*ret_bool)(cuervo_call* call, int value);
    void (*ret_i32)(cuervo_call* call, int32_t value); /* `int` */
    void (*ret_i64)(cuervo_call* call, int64_t value); /* `i64` */
    void (*ret_f32)(cuervo_call* call, float value);   /* `float` */
    void (*ret_f64)(cuervo_call* call, double value);  /* `f64` */
    void (*ret_str)(cuervo_call* call, const char* utf8, size_t length);
    void (*ret_vec)(cuervo_call* call, const float* components, uint32_t size); /* vec2..vec4 */
    /* Returns an object the call received (an argument): `self` for chaining. */
    void (*ret_object)(cuervo_call* call, cuervo_object* object);
    /* Ends the call with a script panic (a stack trace, like a failed assert). */
    void (*panic)(cuervo_call* call, const char* message);

    /* ---- objects */
    void* (*object_native)(cuervo_object* object);
    /* Sets host data, freed by the class's finalizer when the object dies. */
    void (*object_set_native)(cuervo_object* object, void* data);
    /* The object's class (or struct) name, qualified ("noise.Field"). */
    const char* (*object_type)(cuervo_object* object);
    /* Fields by name; 0 on success, nonzero if there is no such field of that kind. */
    int (*object_get_float)(cuervo_object* object, const char* field, double* out);
    int (*object_set_float)(cuervo_object* object, const char* field, double value);
    int (*object_get_int)(cuervo_object* object, const char* field, int64_t* out);
    int (*object_set_int)(cuervo_object* object, const char* field, int64_t value);
    int (*object_get_vec)(cuervo_object* object, const char* field, float* out, uint32_t max, uint32_t* size);
    int (*object_set_vec)(cuervo_object* object, const char* field, const float* components, uint32_t size);

    /* ---- importers */
    void (*buffer_write)(cuervo_buffer* out, const uint8_t* data, size_t size);

    /* ---- misc */
    void (*log)(int level, const char* message);
} cuervo_host;

typedef int (*cuervo_plugin_entry_fn)(const cuervo_host* host, cuervo_registrar* reg);

#ifdef __cplusplus
}
#endif

#endif /* CUERVO_PLUGIN_H */
