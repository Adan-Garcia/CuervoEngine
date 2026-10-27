// Pluma REPL (`plm`). Each input is parsed in script mode (declarations and
// statements mixed), type-checked, compiled and run; declarations and globals
// persist between inputs, and top-level expressions print their value.
// Modes:
//   :run     (default) execute the input
//   :check   parse only: print syntax diagnostics, or "ok"
//   :ast     print the AST as an S-expression
//   :print   print the canonical source (LANGUAGE.md 6)
//   :fmt     format source preserving comments and blank lines (LANGUAGE.md 6)
//   :tokens  print the tokens
// A mode command followed by code (`:ast 1 + 2`) runs that code once in that
// mode. Input continues over several lines while brackets are open.
//
// Files:
//   plm file.plm            run a module: init globals, call main()
//   plm --script file.plm   run a script (top-level statements)
//   plm --check|--ast|--print|--fmt|--tokens file.plm   front-end only
//   plm --fmt --write file.plm   format file in place
//   plm --fmt --check file.plm   exit 1 if file is not formatted
//   plm --lsp                    run Language Server Protocol over stdio
//   plm test <file.plm|dir>... [--filter s]   run the @test functions (LANGUAGE.md 9.27)
// Exit status: 0 ok, 1 compile/format errors (or a failed test), 2 cannot read the file, 3 panic.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "pluma/arena.hpp"
#include "pluma/ast_dump.hpp"
#include "pluma/bindings.hpp"
#include "pluma/diagnostic.hpp"
#include "pluma/format.hpp"
#include "pluma/lexer.hpp"
#include "pluma/lsp.hpp"
#include "pluma/parser.hpp"
#include "pluma/printer.hpp"
#include "pluma/session.hpp"
#include "pluma/source.hpp"
#include "pluma/stdlib.hpp"
#include "pluma/testing.hpp"

namespace {

// The REPL and `plm file` have the math library (pluma/std/math.plm) like the engine.
const pluma::Bindings& math_bindings() {
    static const pluma::Bindings b = [] {
        pluma::Bindings x;
        pluma::add_math_prelude(x);
        return x;
    }();
    return b;
}

enum class Mode { Run, Check, Ast, Print, Fmt, Tokens };

void print_tokens(std::string_view source) {
    for (const pluma::Token& token : pluma::Lexer::tokenize(source)) {
        if (token.kind == pluma::TokenKind::Eof) {
            break;
        }
        std::cout << "  " << token.line << ':' << token.column << "  " << pluma::to_string(token.kind);
        if (token.kind == pluma::TokenKind::Error) {
            std::cout << "  error: " << token.text;
        } else if (token.kind != pluma::TokenKind::Newline) {
            std::cout << "  '" << token.text << '\'';
        }
        std::cout << '\n';
    }
}

// Front-end modes. Returns true if there were no errors.
bool run_front_end(const std::string& name, const std::string& text, Mode mode, bool as_module,
                   bool check_style = false) {
    if (mode == Mode::Tokens) {
        print_tokens(text);
        return true;
    }
    pluma::SourceFile source(name, text);
    if (mode == Mode::Fmt) {
        pluma::FormatOptions opts;
        opts.script_mode = !as_module;
        opts.check_style = check_style;
        pluma::FormatResult res = pluma::format_source(source, opts);
        std::cout << res.rendered;
        if (!res.ok) return false;
        std::cout << res.formatted;
        return true;
    }
    pluma::Arena arena;
    pluma::DiagnosticSink diags(source);
    std::string ast;
    std::string printed;
    if (as_module) {
        pluma::Module m = pluma::parse_module(source, arena, diags);
        ast = pluma::dump(m, true);
        printed = pluma::print(m);
    } else {
        pluma::Script s = pluma::parse_script(source, arena, diags);
        ast = pluma::dump(s, true);
        printed = pluma::print(s);
    }
    std::cout << pluma::render_all(diags.all(), source);
    if (diags.has_errors()) {
        return false;
    }
    switch (mode) {
        case Mode::Check: std::cout << "ok\n"; break;
        case Mode::Ast: std::cout << ast << '\n'; break;
        case Mode::Print: std::cout << printed; break;
        default: break;
    }
    return true;
}

int report(const pluma::SessionResult& r) {
    std::cout << r.rendered;
    if (r.panic) std::cout << r.panic->render();
    if (!r.compiled) return 1;
    return r.ok ? 0 : 3;
}

// Bracket depth of `text`, and whether it ends inside a multi-line string or
// block comment: either means the input continues on the next line.
bool needs_more(const std::string& text) {
    int depth = 0;
    for (const pluma::Token& t : pluma::Lexer::tokenize(text)) {
        switch (t.kind) {
            case pluma::TokenKind::LeftParen:
            case pluma::TokenKind::LeftBracket:
            case pluma::TokenKind::LeftBrace: ++depth; break;
            case pluma::TokenKind::RightParen:
            case pluma::TokenKind::RightBracket:
            case pluma::TokenKind::RightBrace: --depth; break;
            case pluma::TokenKind::Error:
                if (t.text == "unterminated block comment" ||
                    (t.text == "unterminated string" && text.compare(t.offset, 3, "\"\"\"") == 0)) {
                    return true;
                }
                break;
            default: break;
        }
    }
    return depth > 0;
}

bool parse_mode(std::string_view word, Mode& mode) {
    if (word == ":run") mode = Mode::Run;
    else if (word == ":check") mode = Mode::Check;
    else if (word == ":ast") mode = Mode::Ast;
    else if (word == ":print") mode = Mode::Print;
    else if (word == ":fmt") mode = Mode::Fmt;
    else if (word == ":tokens") mode = Mode::Tokens;
    else return false;
    return true;
}

// `plm test`: the files given (a directory: its .plm files, recursively, in
// path order) checked as one program, then each @test run on its own.
int test_command(int argc, char** argv) {
    std::vector<pluma::SourceFile::Part> parts;
    std::string filter;
    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--filter" && i + 1 < argc) {
            filter = argv[++i];
            continue;
        }
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        if (std::filesystem::is_directory(std::string(arg), ec)) {
            for (const auto& e : std::filesystem::recursive_directory_iterator(std::string(arg), ec)) {
                if (e.is_regular_file() && e.path().extension() == ".plm") files.push_back(e.path());
            }
            std::sort(files.begin(), files.end());
        } else {
            files.emplace_back(std::string(arg));
        }
        for (const auto& f : files) {
            std::ifstream in(f, std::ios::binary);
            if (!in) {
                std::cerr << "cannot open " << f.string() << '\n';
                return 2;
            }
            std::stringstream buf;
            buf << in.rdbuf();
            parts.push_back({f.generic_string(), buf.str()});
        }
    }
    if (parts.empty()) {
        std::cerr << "usage: plm test <file.plm|dir>... [--filter <substring>]\n";
        return 2;
    }
    pluma::TestReport report = pluma::run_tests(
        parts, [] { return std::make_unique<pluma::Session>(pluma::VmOptions{}, &math_bindings()); },
        pluma::TestSelection{{}, filter});
    std::cout << pluma::format_test_report(report);
    return report.ok() ? 0 : 1;
}

int run_file(int argc, char** argv) {
    Mode mode = Mode::Run;
    bool script = false;
    bool fmt_write = false;
    bool fmt_check = false;
    bool lsp_mode = false;
    bool check_style = false;
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--check") {
            if (mode == Mode::Fmt) fmt_check = true;
            else mode = Mode::Check;
        } else if (arg == "--ast") mode = Mode::Ast;
        else if (arg == "--print") mode = Mode::Print;
        else if (arg == "--fmt" || (i == 1 && arg == "fmt")) {
            if (mode == Mode::Check) fmt_check = true;
            mode = Mode::Fmt;
        } else if (arg == "--write" || arg == "-w") fmt_write = true;
        else if (arg == "--tokens") mode = Mode::Tokens;
        else if (arg == "--script") script = true;
        else if (arg == "--lsp" || (i == 1 && arg == "lsp")) lsp_mode = true;
        else if (arg == "--check-style" || arg == "--lint") check_style = true;
        else paths.emplace_back(arg);
    }
    if (lsp_mode) {
        pluma::lsp::ServerOptions opts;
        opts.check_style = check_style;
        if (!paths.empty()) opts.root_path = paths[0];
        pluma::lsp::Server server(std::move(opts));
        return server.run_stdio(std::cin, std::cout);
    }
    if (paths.empty()) {
        std::cerr << "no input file specified\n";
        return 2;
    }
    if (mode == Mode::Fmt && (fmt_write || fmt_check)) {
        int status = 0;
        for (const std::string& p : paths) {
            std::ifstream in(p, std::ios::binary);
            if (!in) {
                std::cerr << "cannot open " << p << '\n';
                return 2;
            }
            std::stringstream buf;
            buf << in.rdbuf();
            std::string orig = buf.str();
            in.close();

            pluma::SourceFile sf(p, orig);
            pluma::FormatOptions fopts;
            fopts.script_mode = script;
            fopts.check_style = check_style;
            pluma::FormatResult res = pluma::format_source(sf, fopts);
            std::cerr << res.rendered;
            if (!res.ok) return 1;
            if (fmt_check) {
                if (res.formatted != orig) {
                    std::cerr << p << ": needs formatting\n";
                    status = 1;
                }
            } else if (fmt_write && res.formatted != orig) {
                std::ofstream out(p, std::ios::binary | std::ios::trunc);
                if (!out) {
                    std::cerr << "cannot write " << p << '\n';
                    return 2;
                }
                out << res.formatted;
            }
        }
        return status;
    }

    const std::string& path = paths[0];
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path << '\n';
        return 2;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (mode != Mode::Run) return run_front_end(path, buffer.str(), mode, !script, check_style) ? 0 : 1;
    pluma::Session session(pluma::VmOptions{}, &math_bindings());
    if (script) return report(session.run_script(path, buffer.str()));
    return report(session.run_module(path, buffer.str()));
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "test") return test_command(argc, argv);
    if (argc > 1) {
        return run_file(argc, argv);
    }
    std::cout << "Pluma REPL. :help lists commands, :quit exits.\n";
    pluma::Session session(pluma::VmOptions{}, &math_bindings());
    Mode mode = Mode::Run;
    std::string line;
    std::string input;
    int counter = 0;
    auto execute = [&](const std::string& text, Mode m) {
        if (m == Mode::Run) {
            report(session.run_script("<repl:" + std::to_string(++counter) + ">", text, true));
        } else {
            run_front_end("<repl>", text, m, false);
        }
    };
    while (true) {
        std::cout << (input.empty() ? "> " : ". ") << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (input.empty()) {
            if (line == ":quit") {
                break;
            }
            if (line == ":help") {
                std::cout << "  :run     execute input (default); declarations persist\n"
                             "  :check   parse only: print diagnostics or 'ok'\n"
                             "  :ast     print the syntax tree\n"
                             "  :print   print canonical source\n"
                             "  :fmt     format source preserving comments\n"
                             "  :tokens  print tokens\n"
                             "  :<mode> <code> runs code once in that mode\n";
                continue;
            }
            if (!line.empty() && line[0] == ':') {
                std::size_t space = line.find(' ');
                std::string_view word = std::string_view(line).substr(0, space);
                Mode once = mode;
                if (!parse_mode(word, once)) {
                    std::cout << "unknown command " << word << " (try :help)\n";
                    continue;
                }
                if (space == std::string::npos) {
                    mode = once;
                    continue;
                }
                execute(line.substr(space + 1), once);
                continue;
            }
        }
        input += line;
        input += '\n';
        if (needs_more(input)) {
            continue;
        }
        execute(input, mode);
        input.clear();
    }
    if (!input.empty()) {
        execute(input, mode);
    }
    return 0;
}
