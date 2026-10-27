#include "pluma/testing.hpp"

#include <algorithm>
#include <format>

#include "pluma/sema.hpp"
#include "pluma/vm.hpp"

namespace pluma {

std::size_t TestReport::passed() const {
    return static_cast<std::size_t>(
        std::count_if(results.begin(), results.end(), [](const TestOutcome& t) { return t.passed; }));
}

namespace {

std::unique_ptr<Session> load(const std::vector<SourceFile::Part>& files, const SessionFactory& make_session,
                              SessionResult& result) {
    std::unique_ptr<Session> s = make_session ? make_session() : std::make_unique<Session>();
    if (!s->prelude_result().ok) {
        result = s->prelude_result();
        return s;
    }
    s->include_tests(true);
    result = s->load_files(files);
    return s;
}

} // namespace

TestReport run_tests(const std::vector<SourceFile::Part>& files, const SessionFactory& make_session,
                     TestSelection selection) {
    TestReport report;
    SessionResult first;
    std::unique_ptr<Session> listing = load(files, make_session, first);
    if (!first.compiled) {
        report.rendered = first.rendered;
        return report;
    }
    report.compiled = true;
    struct Found {
        std::string name;
        std::string file;
        std::uint32_t line;
    };
    std::vector<Found> tests;
    for (const Function* f : listing->program().tests) {
        if (f->source->module_at(f->decl->name.span.begin) != selection.module) continue;
        if (!selection.filter.empty() && f->name.find(selection.filter) == std::string::npos) continue;
        Location loc = f->source->location(f->decl->name.span.begin);
        tests.push_back(Found{f->name, std::string(loc.path), loc.line});
    }
    if (!first.ok) {
        // Global initialization panicked: every test would start from it.
        const std::string why = first.panic ? first.panic->render() : first.rendered;
        for (const Found& t : tests) report.results.push_back({t.name, t.file, t.line, false, why});
        return report;
    }
    listing.reset();
    for (const Found& t : tests) {
        TestOutcome out{t.name, t.file, t.line, false, {}};
        SessionResult r;
        std::unique_ptr<Session> s = load(files, make_session, r);
        auto index = s->vm().proto_by_key(t.name);
        if (!r.ok) {
            out.message = r.panic ? r.panic->render() : r.rendered;
        } else if (!index) {
            out.message = "test function not found in the compiled program";
        } else {
            RunResult run = s->vm().call(*index, {});
            out.passed = run.ok;
            if (!run.ok) out.message = run.panic ? run.panic->render() : "failed";
        }
        report.results.push_back(std::move(out));
    }
    return report;
}

std::string format_test_report(const TestReport& report) {
    if (!report.compiled) return report.rendered;
    std::string out;
    for (const TestOutcome& t : report.results) {
        out += std::format("{} {} ({}:{})\n", t.passed ? "ok  " : "FAIL", t.name, t.file, t.line);
        if (!t.passed) {
            std::string_view m = t.message;
            while (!m.empty()) {
                std::size_t nl = m.find('\n');
                out += std::format("     {}\n", m.substr(0, nl));
                if (nl == std::string_view::npos) break;
                m.remove_prefix(nl + 1);
            }
        }
    }
    out += std::format("{} passed, {} failed\n", report.passed(), report.failed());
    return out;
}

} // namespace pluma
