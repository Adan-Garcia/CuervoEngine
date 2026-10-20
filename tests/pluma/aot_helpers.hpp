#pragma once

// Shared by the AOT and semantics tests: compile a script to C, build it into a
// native module, run it in a source-less VM, and compare with the bytecode VM.

#include <doctest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pluma/aot.hpp"
#include "pluma/bindings.hpp"
#include "pluma/checker.hpp"
#include "pluma/compiler.hpp"
#include "pluma/heap.hpp"
#include "pluma/parser.hpp"
#include "pluma/session.hpp"
#include "tests/pluma/run_helpers.hpp"

namespace aot_test {

struct AotRan {
    bool ok = false;
    std::string out;
    std::optional<pluma::PanicInfo> panic;
    std::string panic_short;
};

// Compiles `text` to C, builds a native `.so`, and runs it in a brand-new
// source-less VM (`load_aot`) to test full standalone native execution.
inline AotRan run_aot(std::string_view text, bool wrap_overflow = false, const pluma::Bindings* bindings = nullptr) {
    pluma::VmOptions compile_opts;
    compile_opts.wrap_overflow = wrap_overflow;
    pluma::Program prog;
    pluma::VM compile_vm(compile_opts);
    std::vector<std::unique_ptr<pluma::SourceFile>> sources;
    std::vector<std::unique_ptr<pluma::Arena>> arenas;

    if (bindings != nullptr) {
        for (const auto& n : bindings->natives()) {
            prog.native_names.insert(n.name);
            compile_vm.register_native(n.name, n.fn);
        }
        for (const auto& f : bindings->finalizers()) compile_vm.set_native_finalizer(f.class_name, f.fn);
        if (!bindings->preludes().empty()) {
            std::vector<pluma::SourceFile::Part> parts;
            for (const auto& p : bindings->preludes()) parts.push_back({p.path, p.source});
            sources.push_back(pluma::SourceFile::combine("<prelude>", parts));
            arenas.push_back(std::make_unique<pluma::Arena>());
            pluma::DiagnosticSink pdiags(*sources.back());
            pluma::Module pmod = pluma::parse_module(*sources.back(), *arenas.back(), pdiags);
            REQUIRE_FALSE(pdiags.has_errors());
            pluma::CheckedUnit punit = pluma::check_module(prog, *sources.back(), pmod, pdiags);
            REQUIRE(punit.ok);
            pluma::compile_program(prog, compile_vm);
        }
    }

    sources.push_back(std::make_unique<pluma::SourceFile>("test.plm", std::string(text)));
    arenas.push_back(std::make_unique<pluma::Arena>());
    const pluma::SourceFile& src = *sources.back();
    pluma::DiagnosticSink diags(src);
    pluma::Script script = pluma::parse_script(src, *arenas.back(), diags);
    REQUIRE_FALSE_MESSAGE(diags.has_errors(), pluma::render_all(diags.all(), src));
    pluma::CheckedUnit unit = pluma::check_script(prog, src, script, diags);
    REQUIRE_MESSAGE(unit.ok, pluma::render_all(diags.all(), src));
    pluma::compile_program(prog, compile_vm);

    std::string err;
    auto mod = pluma::compile_aot_module(prog, compile_vm, err, 2);
    REQUIRE_MESSAGE(mod != nullptr, err);

    // Drop the compiler's SourceFiles before running to prove the native
    // module does not depend on in-memory source buffers.
    sources.clear();

    AotRan ran;
    pluma::VmOptions run_opts;
    run_opts.wrap_overflow = wrap_overflow;
    run_opts.out = [&ran](std::string_view s) { ran.out += s; };
    pluma::Session runtime_session(run_opts, bindings, /*compile_preludes=*/false);
    pluma::SessionResult sr = runtime_session.load_aot(std::move(mod));
    REQUIRE_MESSAGE(sr.compiled, sr.rendered);
    ran.ok = sr.ok;
    ran.panic = sr.panic;
    if (sr.panic) ran.panic_short = std::format("{}:{}: {}", sr.panic->line, sr.panic->column, sr.panic->message);
    return ran;
}

inline void check_aot_parity(std::string_view text, bool wrap_overflow = false) {
    INFO("source:\n" << text);
    test::Ran vm_r = test::run(text, wrap_overflow);
    REQUIRE_MESSAGE(vm_r.compiled, vm_r.rendered);
    AotRan aot_r = run_aot(text, wrap_overflow);
    CHECK(aot_r.ok == vm_r.ok);
    CHECK(aot_r.out == vm_r.out);
    CHECK(aot_r.panic_short == vm_r.panic);
}


} // namespace aot_test
