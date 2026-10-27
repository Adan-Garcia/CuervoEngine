/* The Pluma embedding API (DESIGN.md 4.18, milestone 10.5c): a C interface
 * for hosting the Pluma VM in other programs, without the engine.
 *
 * A host creates a VM, declares its own functions (`@native` declarations in
 * prelude sources, bound to C callbacks), loads `.plm` modules, then calls
 * script functions and reads or writes globals:
 *
 *     pluma_vm* vm = pluma_new();
 *     pluma_add_prelude(vm, "host.plm", "@native fn host_log(s: str) {}");
 *     pluma_add_native(vm, "host_log", my_log, NULL);
 *     if (!pluma_load(vm, "game.plm", source)) fputs(pluma_error(vm), stderr);
 *     pluma_value args[1] = {pluma_int(3)}, out;
 *     if (pluma_call(vm, "score", args, 1, &out)) printf("%lld\n", (long long)out.as.integer);
 *     pluma_free(vm);
 *
 * Values are converted by the declared Pluma types: PLUMA_INT fills any
 * integer type and PLUMA_FLOAT any float type, so the host never names a
 * width. The API is versioned by PLUMA_EMBED_API_VERSION; a new version only
 * adds functions.
 *
 * Lifetimes: strings handed to the host (results, globals, pluma_error) stay
 * valid until the next call on the same VM; native arguments, until the
 * native returns. Object handles in results and globals are owned by the
 * host (pluma_release them); those in native arguments are borrowed. A VM is
 * not thread-safe: use it from one thread at a time. */

#ifndef PLUMA_EMBED_H
#define PLUMA_EMBED_H

#include <stddef.h>
#include <stdint.h>

#define PLUMA_EMBED_API_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pluma_vm pluma_vm;
/* A script object: a class or struct instance, a list, a map, ... */
typedef struct pluma_object pluma_object;

typedef enum pluma_type {
    PLUMA_NONE = 0, /* no value (a function returning nothing) */
    PLUMA_NULL,
    PLUMA_BOOL,
    PLUMA_INT,
    PLUMA_FLOAT,
    PLUMA_STR,    /* also `#name` values */
    PLUMA_VEC,    /* vec1 to vec4: `length` components in as.vec */
    PLUMA_OBJECT, /* any other reference */
    PLUMA_OTHER   /* a value the API can't show (an enum, a vec5...) */
} pluma_type;

typedef struct pluma_value {
    pluma_type type;
    uint32_t length; /* PLUMA_STR: bytes (0 means strlen); PLUMA_VEC: components */
    union {
        int boolean;
        int64_t integer;
        double number;
        const char* str;
        float vec[4];
        pluma_object* object;
    } as;
} pluma_value;

/* A host function. `args` are the declaration's parameters (self first for
 * methods). Return the result (pluma_none() for nothing), or call
 * pluma_panic and return anything. */
typedef pluma_value (*pluma_native_fn)(pluma_vm* vm, const pluma_value* args, uint32_t count, void* user);

/* PLUMA_EMBED_API_VERSION of the library. */
uint32_t pluma_embed_version(void);

pluma_vm* pluma_new(void);
void pluma_free(pluma_vm* vm);

/* Host declarations, before the first load: a module whose `@native`
 * functions and methods are bound by name ("host_log", "Canvas.circle").
 * Return 0 after the first load. */
int pluma_add_prelude(pluma_vm* vm, const char* path, const char* source);
int pluma_add_native(pluma_vm* vm, const char* name, pluma_native_fn fn, void* user);

/* From inside a native: fails the script call with `message`. */
void pluma_panic(pluma_vm* vm, const char* message);

/* Checks and compiles module files (declarations), then initializes their
 * globals; `pluma_load_files` checks several as one unit. `pluma_run` also
 * calls the file's `fn main()`. Each returns 1 on success, or 0 with the
 * diagnostics or panic in pluma_error. */
int pluma_load(pluma_vm* vm, const char* path, const char* source);
int pluma_load_files(pluma_vm* vm, const char* const* paths, const char* const* sources, size_t count);
int pluma_run(pluma_vm* vm, const char* path, const char* source);

/* The last failure's text ("" after a success). */
const char* pluma_error(const pluma_vm* vm);

/* Calls a top-level function ("score", "pkg.score") or the method `method`
 * of an instance (virtual dispatch). `result` may be NULL. Return 1, or 0
 * with the error in pluma_error. */
int pluma_call(pluma_vm* vm, const char* function, const pluma_value* args, uint32_t count, pluma_value* result);
int pluma_call_method(pluma_vm* vm, pluma_object* self, const char* method, const pluma_value* args, uint32_t count,
                      pluma_value* result);

/* Globals by qualified name ("best", "Boss.count" for a static). */
int pluma_get_global(pluma_vm* vm, const char* name, pluma_value* out);
int pluma_set_global(pluma_vm* vm, const char* name, pluma_value value);

void pluma_retain(pluma_vm* vm, pluma_object* object);
void pluma_release(pluma_vm* vm, pluma_object* object);
/* The class or struct name of an instance, or "" for other objects. */
const char* pluma_object_class(pluma_vm* vm, pluma_object* object);

/* Display text of a value, as `print` shows it. */
const char* pluma_to_string(pluma_vm* vm, pluma_value value);

/* Advances the VM's tick and resumes due coroutines (DESIGN.md 5.9). */
int pluma_tick(pluma_vm* vm);
/* One incremental cycle-collector step of `budget` work units, when no
 * script runs (DESIGN.md 5.10). */
int pluma_collect(pluma_vm* vm, uint64_t budget);

/* ---- value constructors */

static inline pluma_value pluma_none(void) {
    pluma_value v = {PLUMA_NONE, 0, {0}};
    return v;
}
static inline pluma_value pluma_null(void) {
    pluma_value v = {PLUMA_NULL, 0, {0}};
    return v;
}
static inline pluma_value pluma_bool(int b) {
    pluma_value v = {PLUMA_BOOL, 0, {0}};
    v.as.boolean = b != 0;
    return v;
}
static inline pluma_value pluma_int(int64_t i) {
    pluma_value v = {PLUMA_INT, 0, {0}};
    v.as.integer = i;
    return v;
}
static inline pluma_value pluma_float(double d) {
    pluma_value v = {PLUMA_FLOAT, 0, {0}};
    v.as.number = d;
    return v;
}
static inline pluma_value pluma_str(const char* s) {
    pluma_value v = {PLUMA_STR, 0, {0}};
    v.as.str = s;
    return v;
}
static inline pluma_value pluma_vec2(float x, float y) {
    pluma_value v = {PLUMA_VEC, 2, {0}};
    v.as.vec[0] = x;
    v.as.vec[1] = y;
    return v;
}
static inline pluma_value pluma_object_value(pluma_object* o) {
    pluma_value v = {o != NULL ? PLUMA_OBJECT : PLUMA_NULL, 0, {0}};
    v.as.object = o;
    return v;
}

#ifdef __cplusplus
}
#endif

#endif /* PLUMA_EMBED_H */
