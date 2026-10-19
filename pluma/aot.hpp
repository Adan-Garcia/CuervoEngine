#pragma once

// Ahead-of-time C code generation and native module loader (DESIGN.md 5.12).
//
// `emit_c` translates a checked, compiled Program + VM image into a self-
// contained C11/C++20 translation unit. Every function (`Proto`) becomes a C
// function specialized to its instruction operands (`op`, `a`, `b`, `c`, `k`),
// with direct `goto` control flow, inline scalar/vector arithmetic, and a
// coroutine resume table (`switch (*ctx.pc_ptr)`) for functions that suspend.
//
// The emitted translation unit also embeds the type tables (`RecordRT`,
// `EnumRT`), interned names, globals, constants, local-variable debug ranges
// (`LocalDebug`), and pre-resolved source locations (`SpanLoc`), so a shipped
// native build (`ScriptForm::Native`, docs/EXPORT.md) loads and runs without
// any `.plm` source files while keeping full panic stack traces, live-local
// inspection, and use-after-free diagnostics.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "pluma/sema.hpp"
#include "pluma/vm.hpp"

namespace pluma {

class Session;

// Emits a self-contained C11/C++20 source file for `program` and `vm`.
std::string emit_c(const Program& program, const VM& vm);

// Compiles `c_source` into a shared library at `so_path` using the host C
// compiler (`CC` or `cc`), with `-ffp-contract=off -fno-fast-math` so floating-
// point results match the bytecode VM bit for bit (DESIGN.md 4.1).
bool compile_c_module(std::string_view c_source, const std::filesystem::path& so_path, std::string& error,
                      int opt_level = 2);

// A loaded native AOT shared object. Keep alive at least as long as the `VM`
// it is bound or loaded into.
class AotModule {
public:
    AotModule() = default;
    ~AotModule();
    AotModule(AotModule&& other) noexcept;
    AotModule& operator=(AotModule&& other) noexcept;
    AotModule(const AotModule&) = delete;
    AotModule& operator=(const AotModule&) = delete;

    static std::unique_ptr<AotModule> open(const std::filesystem::path& so_path, std::string& error);
    static std::unique_ptr<AotModule> open_bytes(std::span<const std::byte> so_bytes, std::string& error);

    // Attaches compiled AOT functions (`Proto::aot_fn`) to an already-compiled
    // `VM` (e.g., after `compile_program`).
    bool bind(VM& vm, std::string& error) const;

    // Populates a fresh `VM` with the module's interned names, types, globals,
    // and protos (without `.plm` sources) and runs its initialization chunks.
    RunResult load(VM& vm, std::string& error) const;

private:
    void* handle_ = nullptr;
    bool (*bind_fn_)(VM*, const void*) = nullptr;
    bool (*load_fn_)(VM*, const void*, RunResult*) = nullptr;
    std::filesystem::path temp_path_; // deleted on destruction when temporary
    friend std::unique_ptr<AotModule> compile_aot_module(const Program& program, const VM& vm, std::string& error,
                                                         int opt_level);
};

// Emits C for `(program, vm)`, compiles it to a temporary shared library, and
// returns the opened `AotModule`.
std::unique_ptr<AotModule> compile_aot_module(const Program& program, const VM& vm, std::string& error,
                                              int opt_level = 2);

} // namespace pluma
