#pragma once

// A language session: parse -> check -> compile -> run, keeping every
// declaration, global and type across inputs (the REPL), or running a file,
// or loading a whole project (several files checked as one unit, since names
// are global, DESIGN.md 5.11).
//
// With Bindings, the session registers the host functions and compiles the
// preludes first (pluma/bindings.hpp).

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pluma/arena.hpp"
#include "pluma/diagnostic.hpp"
#include "pluma/sema.hpp"
#include "pluma/source.hpp"
#include "pluma/vm.hpp"

namespace pluma {

class AotModule;
class Bindings;

struct SessionResult {
    bool ok = false;
    bool compiled = false;             // parsed and checked without errors
    std::vector<Diagnostic> diagnostics;
    std::string rendered;              // diagnostics rendered clang-style
    std::optional<PanicInfo> panic;
};

class Session {
public:
    explicit Session(VmOptions options = {}, const Bindings* bindings = nullptr, bool compile_preludes = true);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Result of compiling the bindings' preludes (ok when there are none).
    const SessionResult& prelude_result() const { return prelude_; }

    // Script mode: declarations and statements mixed. `echo` prints the value
    // of top-level expression statements (REPL).
    SessionResult run_script(std::string name, std::string text, bool echo = false);

    // Module mode (a file of declarations): initializes globals, then calls
    // `fn main()` if the file declares one.
    SessionResult run_module(std::string name, std::string text);

    // Several module files checked together as one unit; initializes their
    // globals (in file order, then declaration order). Does not call `main`.
    SessionResult load_files(const std::vector<SourceFile::Part>& files);

    // Loads a precompiled native AOT module (`ScriptForm::Native`,
    // pluma/aot.hpp) into this session's VM without `.plm` sources and runs
    // its initialization chunks.
    SessionResult load_aot(const std::filesystem::path& so_path);
    SessionResult load_aot(std::unique_ptr<AotModule> module);

    // Attaches a compiled AOT module to this session's already-compiled VM.
    bool bind_aot(std::unique_ptr<AotModule> module, std::string& error);

    // Units loaded from now on keep their `@test` functions (Program::tests;
    // LANGUAGE.md 9.27). Off by default, as in release builds.
    void include_tests(bool on) { include_tests_ = on; }

    // Globals by qualified name ("Time.tick"); the setter assigns (retains).
    bool set_global(std::string_view qualified, const Value& v);
    Value global(std::string_view qualified) const;

    Program& program() { return *program_; }
    const Program& program() const { return *program_; }
    VM& vm() { return *vm_; }
    const VM& vm() const { return *vm_; }

private:
    SessionResult finish(SessionResult r, const SourceFile& source, DiagnosticSink& diags);
    bool compile(SessionResult& r, DiagnosticSink& diags);
    SessionResult check_and_run_module(const SourceFile& source, bool call_main);

    std::vector<std::unique_ptr<SourceFile>> sources_;
    std::vector<std::unique_ptr<Arena>> arenas_;
    std::unique_ptr<Program> program_;
    std::unique_ptr<VM> vm_;
    std::unique_ptr<AotModule> aot_;
    SessionResult prelude_;
    bool include_tests_ = false;
};

} // namespace pluma
