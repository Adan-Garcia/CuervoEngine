#include "tools/cuervo_tools/api_docs.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>

#include "pluma/arena.hpp"
#include "pluma/ast.hpp"
#include "pluma/diagnostic.hpp"
#include "pluma/parser.hpp"
#include "pluma/printer.hpp"

namespace cuervo::tools {

namespace {

using namespace pluma;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// The text of a `//` comment line without the slashes and one space;
// nullopt if `line` (already trimmed) isn't a line comment.
std::optional<std::string_view> comment_body(std::string_view line) {
    if (!line.starts_with("//")) return std::nullopt;
    std::string_view body = line.substr(2);
    if (body.starts_with('/')) body.remove_prefix(1);
    if (body.starts_with(' ')) body.remove_prefix(1);
    while (!body.empty() && (body.back() == ' ' || body.back() == '\r')) body.remove_suffix(1);
    return body;
}

// Section dividers inside a type (`// ---- layout`, `// -------- widgets`)
// group the source for its readers; they don't describe the next member.
bool is_divider(std::string_view body) { return body.starts_with("--") || body.starts_with("=="); }

struct Doc {
    std::vector<std::string> lines;
    std::uint32_t first_line = 0; // 1-based line of the first comment line (0: none)
};

// The `//` lines directly above the line where `begin` is (the LSP's hover
// rule, pluma/lsp.cpp extract_doc_comment): a blank line or code ends the
// comment. Attributes are part of a declaration's span, so they don't need
// skipping.
Doc leading_doc(const SourceFile& src, std::uint32_t begin) {
    Doc doc;
    const std::uint32_t decl_line = src.line_col(begin).line;
    std::vector<std::string> reversed;
    for (std::uint32_t line = decl_line; line > 1; --line) {
        auto body = comment_body(trim(src.line_text(line - 1)));
        if (!body) break;
        doc.first_line = line - 1;
        if (!is_divider(*body)) reversed.emplace_back(*body);
    }
    doc.lines.assign(reversed.rbegin(), reversed.rend());
    return doc;
}

// A `//` comment after a declaration on its last line, past an optional `,`
// (`var life = 6.0  // seconds`, `Ring(count: int),  // evenly around`).
std::optional<std::string> trailing_doc(const SourceFile& src, Span span) {
    std::string_view rest = src.text().substr(span.end);
    rest = rest.substr(0, rest.find('\n'));
    rest = trim(rest);
    if (rest.starts_with(',')) rest = trim(rest.substr(1));
    auto body = comment_body(rest);
    if (!body || body->empty()) return std::nullopt;
    return std::string(*body);
}

std::string type_str(const TypeExpr* t);

std::string type_list(const List<TypeExpr*>& types, std::string_view sep) {
    std::string out;
    for (std::uint32_t i = 0; i < types.size(); ++i) {
        if (i > 0) out += sep;
        out += type_str(types[i]);
    }
    return out;
}

std::string type_str(const TypeExpr* t) {
    if (t == nullptr) return "?";
    if (auto n = as<NamedType>(t)) {
        std::string out(n->name.name);
        if (!n->args.empty()) out += "<" + type_list(n->args, ", ") + ">";
        return out;
    }
    if (auto a = as<ArrayType>(t)) return "[" + type_str(a->element) + "; " + print(a->size) + "]";
    if (auto f = as<FnType>(t)) {
        std::string out = "fn(" + type_list(f->params, ", ") + ")";
        if (f->ret != nullptr) out += " -> " + type_str(f->ret);
        return out;
    }
    if (auto n = as<NullableType>(t)) return type_str(n->inner) + "?";
    return "?";
}

std::string param_str(const Param& p) {
    std::string out(p.name.name);
    if (p.type != nullptr) out += ": " + type_str(p.type);
    if (p.default_value != nullptr) out += " = " + print(p.default_value);
    return out;
}

std::string params_str(const List<Param>& params) {
    std::string out = "(";
    for (std::uint32_t i = 0; i < params.size(); ++i) {
        if (i > 0) out += ", ";
        out += param_str(params[i]);
    }
    return out + ")";
}

std::string generics_str(const List<GenericParam>& generics) {
    if (generics.empty()) return {};
    std::string out = "<";
    for (std::uint32_t i = 0; i < generics.size(); ++i) {
        if (i > 0) out += ", ";
        out += generics[i].name.name;
        if (!generics[i].bounds.empty()) out += ": " + type_list(generics[i].bounds, " + ");
    }
    return out + ">";
}

// Attributes and modifiers in front of a declaration. `@native` only says
// where the body lives (C++, DESIGN.md 5.12), which callers don't see.
std::string prefix_str(const Decl* d) {
    std::string out;
    for (const Attribute& a : d->attributes) {
        if (a.name.name == "native") continue;
        out += "@";
        out += a.name.name;
        if (a.has_args) {
            out += "(";
            for (std::uint32_t i = 0; i < a.args.size(); ++i) {
                if (i > 0) out += ", ";
                if (!a.args[i].name.empty()) out += std::string(a.args[i].name.name) + " = ";
                out += print(a.args[i].value);
            }
            out += ")";
        }
        out += " ";
    }
    if (d->is_pub) out += "pub ";
    if (d->is_static) out += "static ";
    if (d->is_final) out += "final ";
    return out;
}

// The declaration as it reads in source, without bodies.
std::string signature(const Decl* d) {
    std::string out = prefix_str(d);
    if (auto v = as<VarDecl>(d)) {
        if (v->weak) out += "weak ";
        out += "var " + std::string(v->name.name);
        if (v->type != nullptr) out += ": " + type_str(v->type);
        if (v->init != nullptr) out += " = " + print(v->init);
        if (v->has_accessors) out += v->setter != nullptr ? " { get set }" : " { get }";
    } else if (auto c = as<ConstDecl>(d)) {
        out += "const " + std::string(c->name.name);
        if (c->type != nullptr) out += ": " + type_str(c->type);
        if (c->value != nullptr) out += " = " + print(c->value);
    } else if (auto f = as<FnDecl>(d)) {
        out += "fn " + std::string(f->name.name) + generics_str(f->generics) + params_str(f->params);
        if (f->ret != nullptr) out += " -> " + type_str(f->ret);
    } else if (auto s = as<SignalDecl>(d)) {
        out += "signal " + std::string(s->name.name);
        if (s->has_params) out += params_str(s->params);
    } else if (auto k = as<ClassDecl>(d)) {
        out += (k->manual ? "manual class " : "class ") + std::string(k->name.name) + generics_str(k->generics);
        if (k->extends != nullptr) out += " extends " + type_str(k->extends);
        if (!k->traits.empty()) out += ": " + type_list(k->traits, ", ");
    } else if (auto st = as<StructDecl>(d)) {
        out += "struct " + std::string(st->name.name) + generics_str(st->generics);
        if (!st->traits.empty()) out += ": " + type_list(st->traits, ", ");
    } else if (auto e = as<EnumDecl>(d)) {
        out += "enum " + std::string(e->name.name);
        if (e->backing != nullptr) out += ": " + type_str(e->backing);
    } else if (auto t = as<TraitDecl>(d)) {
        out += "trait " + std::string(t->name.name) + generics_str(t->generics);
        if (!t->supertraits.empty()) out += ": " + type_list(t->supertraits, " + ");
    }
    return out;
}

std::string variant_signature(const EnumVariant& v) {
    std::string out(v.name.name);
    if (v.has_params) out += params_str(v.params);
    if (v.value != nullptr) out += " = " + print(v.value);
    return out;
}

// Prose outside backticks goes to Markdown as is, except what Markdown would
// read as markup: `<` (an HTML tag: `List<Node>`) and a `#` starting a line
// (a heading).
std::string escape_prose(std::string_view line) {
    std::string out;
    bool in_code = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '`') in_code = !in_code;
        const char next = i + 1 < line.size() ? line[i + 1] : ' ';
        const bool tag_start =
            (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') || next == '/' || next == '!';
        if (!in_code && c == '<' && tag_start) {
            out += "&lt;";
        } else if (!in_code && c == '#' && i == 0) {
            out += "\\#";
        } else {
            out += c;
        }
    }
    return out;
}

// Doc comment lines as Markdown, every line prefixed by `indent` (list item
// continuation). Runs of lines indented past the comment's own margin
// (`//   ready()        once, ...`) are column-aligned tables in the source:
// they become a code block, minus their common indentation, so the alignment
// survives.
void append_doc(std::string& out, const std::vector<std::string>& lines, std::string_view indent) {
    const std::string ind(indent);
    auto pre = [&](std::size_t i) { return !lines[i].empty() && lines[i].front() == ' '; };
    for (std::size_t i = 0; i < lines.size();) {
        if (!pre(i)) {
            out += lines[i].empty() ? std::string("\n") : ind + escape_prose(lines[i]) + "\n";
            ++i;
            continue;
        }
        std::size_t end = i;
        std::size_t margin = std::string::npos;
        while (end < lines.size() && pre(end)) {
            margin = std::min(margin, lines[end].find_first_not_of(' '));
            ++end;
        }
        if (i > 0 && !lines[i - 1].empty()) out += "\n";
        out += ind + "```\n";
        for (; i < end; ++i) out += ind + lines[i].substr(margin) + "\n";
        out += ind + "```\n";
        if (i < lines.size() && !lines[i].empty()) out += "\n";
    }
}

std::vector<std::string> describe(const SourceFile& src, const Decl* d) {
    std::vector<std::string> lines = leading_doc(src, d->span.begin).lines;
    if (d->kind == DeclKind::Var || d->kind == DeclKind::Const || d->kind == DeclKind::Signal) {
        if (auto t = trailing_doc(src, d->span)) {
            if (!lines.empty()) lines.emplace_back();
            lines.push_back(*t);
        }
    }
    return lines;
}

bool is_private(std::string_view name) { return name.starts_with('_'); }

// One `- `signature`` list item with its description.
void append_item(std::string& out, const std::string& sig, const std::vector<std::string>& doc) {
    out += "- `" + sig + "`\n";
    if (!doc.empty()) {
        out += "\n";
        append_doc(out, doc, "  ");
    }
    out += "\n";
}

const List<Decl*>* members_of(const Decl* d) {
    if (auto c = as<ClassDecl>(d)) return &c->members;
    if (auto s = as<StructDecl>(d)) return &s->members;
    if (auto t = as<TraitDecl>(d)) return &t->members;
    return nullptr;
}

void append_type(std::string& out, const SourceFile& src, const Decl* d, const std::vector<std::string>& doc) {
    out += "### " + std::string(d->name.name) + "\n\n";
    out += "```pluma\n" + signature(d) + "\n```\n\n";
    if (!doc.empty()) {
        append_doc(out, doc, "");
        out += "\n";
    }
    if (auto e = as<EnumDecl>(d)) {
        out += "**Variants**\n\n";
        for (const EnumVariant& v : e->variants) {
            std::vector<std::string> vdoc = leading_doc(src, v.span.begin).lines;
            if (auto t = trailing_doc(src, v.span)) vdoc.push_back(*t);
            append_item(out, variant_signature(v), vdoc);
        }
    }
    const List<Decl*>* members = members_of(d);
    if (members == nullptr) return;
    // Signals, then fields, then methods: what an object has before what it does.
    const struct {
        const char* title;
        DeclKind kind;
    } groups[] = {{"Signals", DeclKind::Signal}, {"Fields", DeclKind::Var}, {"Methods", DeclKind::Fn}};
    for (const auto& g : groups) {
        bool any = false;
        for (const Decl* m : *members) {
            if (m->kind != g.kind || is_private(m->name.name)) continue;
            if (!any) out += std::string("**") + g.title + "**\n\n";
            any = true;
            append_item(out, signature(m), describe(src, m));
        }
    }
}

// GitHub's heading anchor for a plain name: lowercase, '.' dropped.
std::string anchor(std::string_view heading) {
    std::string out;
    for (char c : heading) {
        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            out += c;
        } else if (c == ' ') {
            out += '-';
        }
    }
    return out;
}

std::string_view file_name(std::string_view path) {
    if (path.starts_with("cuervo/")) return path.substr(7);
    if (path.starts_with("pluma/")) return path.substr(6);
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

bool is_type(const Decl* d) {
    return d->kind == DeclKind::Class || d->kind == DeclKind::Struct || d->kind == DeclKind::Enum ||
           d->kind == DeclKind::Trait;
}

struct Parsed {
    std::unique_ptr<SourceFile> source;
    Arena arena;
    Module module;
};

} // namespace

std::optional<std::string> generate_api_docs(const std::vector<SourceFile::Part>& preludes, std::string& error) {
    std::vector<std::unique_ptr<Parsed>> files;
    for (const SourceFile::Part& part : preludes) {
        auto p = std::make_unique<Parsed>();
        p->source = std::make_unique<SourceFile>(part.path, part.text);
        DiagnosticSink diags(*p->source);
        p->module = parse_module(*p->source, p->arena, diags);
        if (diags.has_errors()) {
            error = render_all(diags.all(), *p->source);
            return std::nullopt;
        }
        files.push_back(std::move(p));
    }

    std::string out;
    out += "# Engine API reference\n\n";
    out += "<!-- Generated by `cuervo-tools docs -o docs/API.md` from cuervo/script/prelude/*.plm.\n"
           "     Don't edit by hand: change the prelude's doc comments and regenerate\n"
           "     (the api_docs_up_to_date test fails while this file is stale). -->\n\n";
    out += "Everything the engine gives scripts, generated from the engine prelude: Pluma source\n"
           "(`cuervo/script/prelude/*.plm`) compiled before a game's own scripts, so every name below\n"
           "is global. Methods without a body there are implemented by the engine in C++. The\n"
           "language's built-ins (`print`, `min`/`max`, `List`, `Map`, `vec2`..`vec4` and their\n"
           "methods, `sin`, `lerp`, `wait`, ...) are in [LANGUAGE.md](LANGUAGE.md) §9.9-9.11;\n"
           "the guide is [GUIDE.md](GUIDE.md). Names starting with `_` are private and not listed.\n\n";

    out += "## Contents\n\n";
    for (const auto& f : files) {
        const std::string name(file_name(f->source->path()));
        out += "- [" + name + "](#" + anchor(name) + "):";
        bool first = true;
        for (const Decl* d : f->module.decls) {
            if (is_private(d->name.name)) continue;
            out += first ? " " : ", ";
            first = false;
            if (is_type(d)) {
                out += "[`" + std::string(d->name.name) + "`](#" + anchor(d->name.name) + ")";
            } else {
                out += "`" + std::string(d->name.name) + "`";
            }
        }
        out += "\n";
    }
    out += "\n";

    for (const auto& f : files) {
        const SourceFile& src = *f->source;
        const std::string name(file_name(src.path()));
        out += "## " + name + "\n\n";
        // The file's header comment introduces the section, unless it runs
        // into the first declaration and so is that declaration's doc.
        const Decl* first = f->module.decls.empty() ? nullptr : f->module.decls[0];
        const bool header_is_doc = first != nullptr && leading_doc(src, first->span.begin).first_line == 1;
        if (!header_is_doc) {
            Doc header;
            for (std::uint32_t line = 1; line <= src.line_count(); ++line) {
                auto body = comment_body(trim(src.line_text(line)));
                if (!body) break;
                if (!is_divider(*body)) header.lines.emplace_back(*body);
            }
            if (!header.lines.empty()) {
                append_doc(out, header.lines, "");
                out += "\n";
            }
        }
        // The language's own library lives in pluma/std, not the engine prelude.
        out += (src.path().starts_with("pluma/") ? "Source: `pluma/std/" : "Source: `cuervo/script/prelude/") + name + "`\n\n";

        bool any_value = false;
        for (const Decl* d : f->module.decls) {
            if (is_type(d) || is_private(d->name.name)) continue;
            if (!any_value) out += "### Constants and functions\n\n";
            any_value = true;
            append_item(out, signature(d), describe(src, d));
        }
        for (const Decl* d : f->module.decls) {
            if (!is_type(d) || is_private(d->name.name)) continue;
            append_type(out, src, d, describe(src, d));
        }
    }
    // Exactly one newline at the end, whatever the last item emitted.
    while (out.ends_with("\n\n")) out.pop_back();
    return out;
}

// Doc coverage (DESIGN.md 4.16 "Clarified (Milestone 8.4)"): every public item
// the reference lists must have a description, i.e. what `describe` finds.
std::optional<ApiCoverage> check_api_coverage(const std::vector<SourceFile::Part>& preludes, std::string& error) {
    ApiCoverage cov;
    for (const SourceFile::Part& part : preludes) {
        SourceFile src(part.path, part.text);
        Arena arena;
        DiagnosticSink diags(src);
        Module module = parse_module(src, arena, diags);
        if (diags.has_errors()) {
            error = render_all(diags.all(), src);
            return std::nullopt;
        }
        auto note = [&](std::string_view what, bool documented, std::uint32_t offset) {
            ++cov.total;
            if (documented) {
                ++cov.documented;
                return;
            }
            cov.missing.push_back(std::string(part.path) + ":" + std::to_string(src.line_col(offset).line) + ": " +
                                  std::string(what));
        };
        for (const Decl* d : module.decls) {
            if (is_private(d->name.name)) continue;
            const std::string top(d->name.name);
            note(top, !describe(src, d).empty(), d->span.begin);
            if (auto e = as<EnumDecl>(d)) {
                for (const EnumVariant& v : e->variants) {
                    std::vector<std::string> vdoc = leading_doc(src, v.span.begin).lines;
                    if (auto t = trailing_doc(src, v.span)) vdoc.push_back(*t);
                    note(top + "." + std::string(v.name.name), !vdoc.empty(), v.span.begin);
                }
            }
            const List<Decl*>* members = members_of(d);
            if (members == nullptr) continue;
            for (const Decl* m : *members) {
                if (is_private(m->name.name)) continue;
                if (m->kind != DeclKind::Var && m->kind != DeclKind::Fn && m->kind != DeclKind::Signal) continue;
                note(top + "." + std::string(m->name.name), !describe(src, m).empty(), m->span.begin);
            }
        }
    }
    return cov;
}

} // namespace cuervo::tools
