#include "pluma/session.hpp"

#include <stdexcept>

#include "pluma/aot.hpp"
#include "pluma/bindings.hpp"
#include "pluma/checker.hpp"
#include "pluma/compiler.hpp"
#include "pluma/parser.hpp"

namespace pluma {

Session::Session(VmOptions options, const Bindings* bindings, bool compile_preludes)
    : program_(std::make_unique<Program>()), vm_(std::make_unique<VM>(std::move(options))) {
    prelude_.ok = true;
    prelude_.compiled = true;
    if (bindings == nullptr) return;
    for (const Bindings::Native& n : bindings->natives()) {
        program_->native_names.insert(n.name);
        vm_->register_native(n.name, n.fn);
    }
    for (const Bindings::Finalizer& f : bindings->finalizers()) vm_->set_native_finalizer(f.class_name, f.fn);
    if (compile_preludes && !bindings->preludes().empty()) {
        std::vector<SourceFile::Part> parts;
        for (const Bindings::Prelude& p : bindings->preludes()) parts.push_back({p.path, p.source});
        prelude_ = load_files(parts);
    }
}

Session::~Session() {
    // The VM holds values that point into the program's protos and AOT module;
    // drop it first.
    vm_.reset();
    program_.reset();
    aot_.reset();
}

SessionResult Session::finish(SessionResult r, const SourceFile& source, DiagnosticSink& diags) {
    diags.sort_by_position();
    r.diagnostics = diags.all();
    r.rendered = render_all(diags.all(), source);
    return r;
}

bool Session::compile(SessionResult& r, DiagnosticSink& diags) {
    try {
        compile_program(*program_, *vm_);
    } catch (const std::runtime_error& e) {
        // Bytecode limits (65536 registers or constants per function).
        // TODO: the VM image is left partially built; start a new session.
        diags.error(Span{0, 0}, e.what());
        return false;
    }
    r.compiled = true;
    if (DebugHooks* hooks = vm_->debug_hooks()) hooks->on_code_loaded(*vm_); // before any of it runs
    return true;
}

SessionResult Session::run_script(std::string name, std::string text, bool echo) {
    sources_.push_back(std::make_unique<SourceFile>(std::move(name), std::move(text)));
    arenas_.push_back(std::make_unique<Arena>());
    const SourceFile& source = *sources_.back();
    DiagnosticSink diags(source);
    Script script = parse_script(source, *arenas_.back(), diags);
    SessionResult r;
    if (diags.has_errors()) return finish(r, source, diags);
    CheckOptions opts;
    opts.echo = echo;
    CheckedUnit unit = check_script(*program_, source, script, diags, opts);
    if (!unit.ok) return finish(r, source, diags);
    if (!compile(r, diags)) return finish(r, source, diags);
    RunResult run = vm_->run(unit.chunk->index);
    r.ok = run.ok;
    r.panic = run.panic;
    return finish(r, source, diags);
}

SessionResult Session::check_and_run_module(const SourceFile& source, bool call_main) {
    arenas_.push_back(std::make_unique<Arena>());
    DiagnosticSink diags(source);
    Module module = parse_module(source, *arenas_.back(), diags);
    SessionResult r;
    if (diags.has_errors()) return finish(r, source, diags);
    CheckedUnit unit = check_module(*program_, source, module, diags);
    if (!unit.ok) return finish(r, source, diags);
    if (!compile(r, diags)) return finish(r, source, diags);
    RunResult run = vm_->run(unit.chunk->index);
    if (run.ok && call_main && unit.main != nullptr) run = vm_->run(unit.main->index);
    r.ok = run.ok;
    r.panic = run.panic;
    return finish(r, source, diags);
}

SessionResult Session::run_module(std::string name, std::string text) {
    sources_.push_back(std::make_unique<SourceFile>(std::move(name), std::move(text)));
    return check_and_run_module(*sources_.back(), true);
}

SessionResult Session::load_files(const std::vector<SourceFile::Part>& files) {
    std::string name = files.size() == 1 ? files[0].path : "<project>";
    sources_.push_back(SourceFile::combine(std::move(name), files));
    return check_and_run_module(*sources_.back(), false);
}

SessionResult Session::load_aot(const std::filesystem::path& so_path) {
    std::string error;
    auto mod = AotModule::open(so_path, error);
    if (!mod) {
        SessionResult r;
        r.rendered = std::move(error);
        return r;
    }
    return load_aot(std::move(mod));
}

SessionResult Session::load_aot(std::unique_ptr<AotModule> module) {
    SessionResult r;
    if (!module) {
        r.rendered = "AOT module is null";
        return r;
    }
    std::string error;
    RunResult run = module->load(*vm_, error);
    if (!error.empty()) {
        r.rendered = std::move(error);
        return r;
    }
    aot_ = std::move(module);
    r.compiled = true;
    r.ok = run.ok;
    r.panic = run.panic;
    return r;
}

bool Session::bind_aot(std::unique_ptr<AotModule> module, std::string& error) {
    if (!module) {
        error = "AOT module is null";
        return false;
    }
    if (!module->bind(*vm_, error)) return false;
    aot_ = std::move(module);
    return true;
}

bool Session::set_global(std::string_view qualified, const Value& v) {
    auto i = vm_->global_index(qualified);
    if (!i) return false;
    assign(vm_->globals[*i], v);
    return true;
}

Value Session::global(std::string_view qualified) const {
    auto i = vm_->global_index(qualified);
    return i ? vm_->globals[*i] : Value{};
}

} // namespace pluma
