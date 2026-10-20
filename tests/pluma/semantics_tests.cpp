// Differential semantics corpus: every tests/pluma/semantics/NAME.plm is run as
// a script on the bytecode VM and again as native code (AOT-to-C). Its printed
// output, followed by a "panic LINE:COL: message" line when it panics, must
// equal NAME.out, and both back ends must agree exactly (DESIGN.md 5.12).
// Set PLUMA_UPDATE_SEMANTICS=1 to rewrite the .out files from the VM, then
// review the diff by hand: the files are the specification being tested.

#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "pluma/format.hpp"
#include "tests/pluma/aot_helpers.hpp"

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The output, then the panic with its stack trace and each frame's live locals.
std::string transcript(const std::string& out, const std::optional<pluma::PanicInfo>& panic) {
    std::string t = out;
    if (panic) t += panic->render_with_locals();
    return t;
}

struct VmRan {
    pluma::SessionResult result;
    std::string out;
};

VmRan run_vm(const std::string& text) {
    VmRan ran;
    pluma::VmOptions options;
    options.out = [&ran](std::string_view s) { ran.out += s; };
    pluma::Session session(options);
    ran.result = session.run_script("test.plm", text);
    return ran;
}

bool env_set(const char* name) {
#if defined(_MSC_VER)
    char* v = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&v, &len, name) == 0 && v != nullptr) {
        std::free(v);
        return true;
    }
    return false;
#else
    return std::getenv(name) != nullptr;
#endif
}

} // namespace

TEST_CASE("semantics: corpus matches expectations on the VM and in AOT") {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(CUERVO_SEMANTICS_DIR)) {
        if (e.path().extension() == ".plm") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    REQUIRE_FALSE(files.empty());
    const bool update = env_set("PLUMA_UPDATE_SEMANTICS");
    for (const fs::path& file : files) {
        CAPTURE(file.filename().string());
        const std::string text = read_file(file);
        VmRan vm = run_vm(text);
        REQUIRE_MESSAGE(vm.result.compiled, vm.result.rendered);
        const std::string got = transcript(vm.out, vm.result.panic);
        fs::path expect_path = file;
        expect_path.replace_extension(".out");
        if (update) {
            std::ofstream(expect_path, std::ios::binary) << got;
        } else {
            CHECK_MESSAGE(got == read_file(expect_path), "VM output differs from ", expect_path.filename().string());
        }
        aot_test::AotRan native = aot_test::run_aot(text);
        const std::string native_text = transcript(native.out, native.panic);
        if (native_text != got) {
            // Keep the native transcript for a diff against the .out file.
            fs::path dump = fs::temp_directory_path() / (file.stem().string() + ".aot.txt");
            std::ofstream(dump, std::ios::binary) << native_text;
            FAIL_CHECK("AOT output differs from the VM; native transcript written to ", dump.string());
        }
        CHECK(native.ok == vm.result.ok);
    }
}

TEST_CASE("semantics: formatting the corpus is idempotent and keeps behavior") {
    for (const auto& e : fs::directory_iterator(CUERVO_SEMANTICS_DIR)) {
        if (e.path().extension() != ".plm") continue;
        CAPTURE(e.path().filename().string());
        const std::string text = read_file(e.path());
        pluma::FormatOptions options;
        options.script_mode = true;
        options.check_style = false;
        pluma::FormatResult once = pluma::format_source("test.plm", text, options);
        REQUIRE_MESSAGE(once.ok, once.rendered);
        pluma::FormatResult twice = pluma::format_source("test.plm", once.formatted, options);
        REQUIRE_MESSAGE(twice.ok, twice.rendered);
        CHECK_MESSAGE(twice.formatted == once.formatted, "formatting is not idempotent");
        VmRan original = run_vm(text);
        VmRan formatted = run_vm(once.formatted);
        REQUIRE_MESSAGE(formatted.result.compiled, formatted.result.rendered);
        CHECK(formatted.out == original.out);
        CHECK(formatted.result.ok == original.result.ok);
        if (original.result.panic && formatted.result.panic) {
            CHECK(formatted.result.panic->message == original.result.panic->message);
        }
    }
}
