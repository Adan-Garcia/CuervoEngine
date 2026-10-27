/* The Pluma embedding API (pluma/embed/pluma_embed.h, milestone 10.5c) from a
 * plain C program: host natives, module loading, calls, globals, methods,
 * panics on both sides and coroutines. Exits non-zero on the first failure. */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS /* strncat is bounded below */
#endif
#include <stdio.h>
#include <string.h>

#include "pluma/embed/pluma_embed.h"

static int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                \
        }                                                              \
    } while (0)

static char logged[256];

static pluma_value host_log(pluma_vm* vm, const pluma_value* args, uint32_t count, void* user) {
    (void)vm;
    (void)user;
    if (count == 1 && args[0].type == PLUMA_STR) {
        strncat(logged, args[0].as.str, sizeof logged - strlen(logged) - 1);
        strncat(logged, ";", sizeof logged - strlen(logged) - 1);
    }
    return pluma_none();
}

static pluma_value host_scale(pluma_vm* vm, const pluma_value* args, uint32_t count, void* user) {
    (void)vm;
    (void)count;
    return pluma_int(args[0].as.integer * *(const int*)user);
}

static pluma_value host_mid(pluma_vm* vm, const pluma_value* args, uint32_t count, void* user) {
    (void)vm;
    (void)count;
    (void)user;
    return pluma_vec2((args[0].as.vec[0] + args[1].as.vec[0]) / 2, (args[0].as.vec[1] + args[1].as.vec[1]) / 2);
}

static pluma_value host_check(pluma_vm* vm, const pluma_value* args, uint32_t count, void* user) {
    (void)count;
    (void)user;
    if (args[0].as.number < 0) pluma_panic(vm, "negative input");
    return pluma_float(args[0].as.number * 2);
}

static const char* prelude = "@native fn host_log(s: str) {}\n"
                             "@native fn host_scale(x: int) -> int {}\n"
                             "@native fn host_mid(a: vec2, b: vec2) -> vec2 {}\n"
                             "@native fn host_check(x: f64) -> f64 {}\n";

static const char* game = "var best = 10\n"
                          "var title = \"crow\"\n"
                          "class Crow {\n"
                          "    var name = \"\"\n"
                          "    var caws = 0\n"
                          "    fn caw(times: int) -> str {\n"
                          "        caws += times\n"
                          "        host_log(\"{name} x{times}\")\n"
                          "        return \"{name}:{caws}\"\n"
                          "    }\n"
                          "}\n"
                          "fn make(name: str) -> Crow {\n"
                          "    var c = Crow()\n"
                          "    c.name = name\n"
                          "    return c\n"
                          "}\n"
                          "fn score(points: int, bonus: float) -> float {\n"
                          "    return host_scale(points) as float + bonus + best as float\n"
                          "}\n"
                          "fn middle() -> vec2 { return host_mid(vec2(0, 0), vec2(4, 2)) }\n"
                          "fn doubled(x: f64) -> f64 { return host_check(x) }\n"
                          "fn crash() -> int {\n"
                          "    var xs: List<int> = []\n"
                          "    return xs[3]\n"
                          "}\n"
                          "var ticks = 0\n"
                          "fn counter() {\n"
                          "    while true {\n"
                          "        ticks += 1\n"
                          "        await wait_ticks(1)\n"
                          "    }\n"
                          "}\n"
                          "fn launch() { start(counter()) }\n";

int main(void) {
    CHECK(pluma_embed_version() == PLUMA_EMBED_API_VERSION);

    pluma_vm* vm = pluma_new();
    static const int factor = 3;
    CHECK(pluma_add_prelude(vm, "host.plm", prelude));
    CHECK(pluma_add_native(vm, "host_log", host_log, NULL));
    CHECK(pluma_add_native(vm, "host_scale", host_scale, (void*)&factor));
    CHECK(pluma_add_native(vm, "host_mid", host_mid, NULL));
    CHECK(pluma_add_native(vm, "host_check", host_check, NULL));

    if (!pluma_load(vm, "game.plm", game)) {
        fprintf(stderr, "load failed:\n%s\n", pluma_error(vm));
        return 1;
    }
    CHECK(!pluma_add_native(vm, "late", host_log, NULL)); /* too late after a load */

    /* Calls convert by the declared types: an INT for `float`, a FLOAT result. */
    pluma_value args[2] = {pluma_int(4), pluma_int(1)};
    pluma_value out;
    CHECK(pluma_call(vm, "score", args, 2, &out));
    CHECK(out.type == PLUMA_FLOAT && out.as.number == 23.0);

    CHECK(pluma_call(vm, "middle", NULL, 0, &out));
    CHECK(out.type == PLUMA_VEC && out.length == 2 && out.as.vec[0] == 2.0f && out.as.vec[1] == 1.0f);

    /* Globals. */
    CHECK(pluma_get_global(vm, "best", &out) && out.type == PLUMA_INT && out.as.integer == 10);
    CHECK(pluma_set_global(vm, "best", pluma_int(20)));
    CHECK(pluma_call(vm, "score", args, 2, &out) && out.as.number == 33.0);
    CHECK(pluma_set_global(vm, "title", pluma_str("raven")));
    CHECK(pluma_get_global(vm, "title", &out) && out.type == PLUMA_STR && strcmp(out.as.str, "raven") == 0);
    CHECK(!pluma_get_global(vm, "nope", &out));

    /* Objects and methods. */
    pluma_value name = pluma_str("kraa");
    pluma_value crow;
    CHECK(pluma_call(vm, "make", &name, 1, &crow) && crow.type == PLUMA_OBJECT);
    CHECK(strcmp(pluma_object_class(vm, crow.as.object), "Crow") == 0);
    pluma_value times = pluma_int(2);
    CHECK(pluma_call_method(vm, crow.as.object, "caw", &times, 1, &out));
    CHECK(out.type == PLUMA_STR && strcmp(out.as.str, "kraa:2") == 0);
    times = pluma_int(3);
    CHECK(pluma_call_method(vm, crow.as.object, "caw", &times, 1, &out) && strcmp(out.as.str, "kraa:5") == 0);
    CHECK(strcmp(logged, "kraa x2;kraa x3;") == 0);
    CHECK(strstr(pluma_to_string(vm, crow), "Crow") != NULL);
    pluma_release(vm, crow.as.object);

    /* Errors: wrong arity, a native's panic, a script panic, a missing function. */
    CHECK(!pluma_call(vm, "score", args, 1, &out));
    CHECK(strstr(pluma_error(vm), "takes 2 arguments") != NULL);
    pluma_value x = pluma_float(2.5);
    CHECK(pluma_call(vm, "doubled", &x, 1, &out) && out.as.number == 5.0);
    x = pluma_float(-1.0);
    CHECK(!pluma_call(vm, "doubled", &x, 1, &out));
    CHECK(strstr(pluma_error(vm), "negative input") != NULL);
    CHECK(!pluma_call(vm, "crash", NULL, 0, &out));
    CHECK(strstr(pluma_error(vm), "index") != NULL);
    CHECK(!pluma_call(vm, "missing", NULL, 0, &out));
    CHECK(pluma_call(vm, "doubled", &args[0], 1, &out) && strcmp(pluma_error(vm), "") == 0);

    /* Coroutines advance with pluma_tick. */
    CHECK(pluma_call(vm, "launch", NULL, 0, NULL));
    for (int i = 0; i < 5; ++i) CHECK(pluma_tick(vm));
    CHECK(pluma_get_global(vm, "ticks", &out) && out.as.integer == 6);
    CHECK(pluma_collect(vm, 1000));

    /* Diagnostics come back as text. */
    pluma_vm* bad = pluma_new();
    CHECK(!pluma_load(bad, "bad.plm", "fn f() -> int { return \"x\" }\n"));
    CHECK(strstr(pluma_error(bad), "bad.plm") != NULL);
    pluma_free(bad);

    pluma_vm* runner = pluma_new();
    CHECK(pluma_run(runner, "main.plm", "var ran = 0\nfn main() { ran = 7 }\n"));
    CHECK(pluma_get_global(runner, "ran", &out) && out.as.integer == 7);
    pluma_free(runner);

    pluma_free(vm);
    if (failures == 0) printf("embed_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
