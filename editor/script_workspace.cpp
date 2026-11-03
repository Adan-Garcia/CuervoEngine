#include "editor/script_workspace.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>

#include "cuervo/script/script_host.hpp"
#include "pluma/format.hpp"

namespace fs = std::filesystem;

namespace cuervo::editor {

namespace {

std::int64_t mtime_of(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
}

bool read_file(const fs::path& p, std::string& out, std::string& error) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { error = "can't read " + p.string(); return false; }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

void flatten(const std::vector<pluma::lsp::DocumentSymbol>& in, int depth, const ScriptDoc& d, std::vector<OutlineItem>& out) {
    for (const pluma::lsp::DocumentSymbol& s : in) {
        OutlineItem it;
        it.name = s.name;
        it.detail = s.detail;
        it.kind = static_cast<int>(s.kind);
        it.depth = depth;
        it.pos = d.buf.from_utf16(static_cast<int>(s.selection_range.start.line), static_cast<int>(s.selection_range.start.character));
        it.range = {d.buf.from_utf16(static_cast<int>(s.range.start.line), static_cast<int>(s.range.start.character)),
                    d.buf.from_utf16(static_cast<int>(s.range.end.line), static_cast<int>(s.range.end.character))};
        out.push_back(std::move(it));
        flatten(s.children, depth + 1, d, out);
    }
}

} // namespace

void ScriptWorkspace::make_server() {
    // The same prelude sources and @native names the play and check paths use (script_host default options).
    server_ = std::make_unique<pluma::lsp::Server>(script::default_lsp_options(dir_.empty() ? std::string() : dir_.string(), false));
}

void ScriptWorkspace::reset(const fs::path& project_dir) {
    docs_.clear();
    bps_.clear();
    mru_.clear();
    active_ = -1;
    // Same spelling open() uses for files (weakly_canonical). On Windows a temp dir arrives as an 8.3
    // short name (RUNNER~1) while files resolve to the long one, so an un-normalized root made
    // relative() yield "..", broke the server's root-vs-document URIs and lost diagnostics/references.
    std::error_code ec;
    dir_ = project_dir.empty() ? project_dir : fs::weakly_canonical(project_dir, ec);
    if (ec) dir_ = project_dir;
    make_server();
}

std::string ScriptWorkspace::uri(const ScriptDoc& d) const { return pluma::lsp::path_to_uri(d.path.string()); }

pluma::lsp::Position ScriptWorkspace::to_lsp(const ScriptDoc& d, TextPos p) const {
    p = d.buf.clamp(p);
    return {static_cast<std::uint32_t>(p.line), static_cast<std::uint32_t>(d.buf.to_utf16(p))};
}
TextPos ScriptWorkspace::from_lsp(const ScriptDoc& d, pluma::lsp::Position p) const { return d.buf.from_utf16(static_cast<int>(p.line), static_cast<int>(p.character)); }
TextRange ScriptWorkspace::from_lsp(const ScriptDoc& d, pluma::lsp::Range r) const { return {from_lsp(d, r.start), from_lsp(d, r.end)}; }

int ScriptWorkspace::find(const fs::path& file) const {
    std::error_code ec;
    const fs::path want = fs::weakly_canonical(file, ec);
    for (const auto& d : docs_) if (d->path == want || d->path == file) return d->id;
    return -1;
}

ScriptDoc* ScriptWorkspace::doc(int id) {
    for (auto& d : docs_) if (d->id == id) return d.get();
    return nullptr;
}
const ScriptDoc* ScriptWorkspace::doc(int id) const {
    for (const auto& d : docs_) if (d->id == id) return d.get();
    return nullptr;
}

void ScriptWorkspace::touch_mru(int id) {
    mru_.erase(std::remove(mru_.begin(), mru_.end(), id), mru_.end());
    mru_.insert(mru_.begin(), id);
}

void ScriptWorkspace::activate(int id) {
    if (doc(id) == nullptr) return;
    active_ = id;
    touch_mru(id);
    cycle_pos_ = -1;
}

int ScriptWorkspace::cycle(bool back) {
    if (mru_.size() < 2) return active_;
    // Repeated presses walk further down the MRU list; `activate` (any other action) resets the walk.
    const int n = static_cast<int>(mru_.size());
    int pos = cycle_pos_ < 0 ? 0 : cycle_pos_;
    pos = ((pos + (back ? -1 : 1)) % n + n) % n;
    const int target = mru_[static_cast<std::size_t>(pos)];
    active_ = target;
    cycle_pos_ = pos;
    return target;
}

int ScriptWorkspace::open(const fs::path& file, std::string& error) {
    std::error_code ec;
    fs::path abs = file.is_absolute() ? file : dir_ / file;
    abs = fs::weakly_canonical(abs, ec);
    if (const int have = find(abs); have >= 0) { activate(have); return have; }
    std::string text;
    if (!read_file(abs, text, error)) return -1;
    auto d = std::make_unique<ScriptDoc>();
    d->id = next_id_++;
    d->path = abs;
    const fs::path rel = fs::relative(abs, dir_, ec);
    d->rel = slashes((ec || rel.empty() || rel.string().starts_with("..")) ? abs.string() : rel.string());
    // The syntax cache sees every line change first; breakpoints ride along.
    std::set<int>* const set = &bps_[d->rel];
    d->bps = set;
    d->buf.set_text(text);
    d->syn.update(d->buf); // consumes the load, so only later edits move breakpoints
    d->syn.on_change = [this, set](const LineChange& c) {
        const std::set<int> before = *set;
        shift_breakpoints(*set, c);
        if (*set != before) ++bp_rev_;
    };
    d->disk_mtime = mtime_of(abs);
    const int id = d->id;
    server_->open_document(uri(*d), d->buf.text(), ++d->version);
    d->synced_rev = d->buf.revision();
    docs_.push_back(std::move(d));
    activate(id);
    return id;
}

int ScriptWorkspace::new_script(const std::string& rel_path, const std::string& class_name, const std::string& base, std::string& error) {
    if (rel_path.empty() || class_name.empty()) { error = "A script needs a file name and a class name."; return -1; }
    const fs::path abs = dir_ / rel_path;
    std::error_code ec;
    if (fs::exists(abs, ec)) { error = "File '" + rel_path + "' already exists."; return -1; }
    fs::create_directories(abs.parent_path(), ec);
    std::ofstream out(abs, std::ios::binary);
    if (!out) { error = "Could not create '" + rel_path + "'."; return -1; }
    out << "class " << class_name << " extends " << (base.empty() ? "Node2D" : base) << " {\n    fn ready() {\n    }\n\n    fn update(dt: float) {\n    }\n}\n";
    out.close();
    return open(abs, error);
}

void ScriptWorkspace::close(int id) {
    const auto it = std::find_if(docs_.begin(), docs_.end(), [&](const auto& d) { return d->id == id; });
    if (it == docs_.end()) return;
    server_->close_document(uri(**it));
    docs_.erase(it);
    mru_.erase(std::remove(mru_.begin(), mru_.end(), id), mru_.end());
    if (active_ == id) active_ = mru_.empty() ? -1 : mru_.front();
    cycle_pos_ = -1;
}

int ScriptWorkspace::dirty_count() const {
    return static_cast<int>(std::count_if(docs_.begin(), docs_.end(), [](const auto& d) { return d->buf.dirty(); }));
}

bool ScriptWorkspace::format(int id) {
    ScriptDoc* d = doc(id);
    if (d == nullptr) return false;
    sync(*d);
    // The server returns nothing for text that does not parse, so a broken file is never reshaped.
    const std::vector<pluma::lsp::TextEdit> edits = server_->format_document(uri(*d));
    if (edits.empty()) return false;
    std::vector<std::pair<TextRange, std::string>> mine;
    for (const auto& e : edits) mine.emplace_back(from_lsp(*d, e.range), e.new_text);
    const std::vector<Selection> keep = d->buf.selections();
    const std::uint64_t before = d->buf.revision();
    d->buf.replace_all(std::move(mine));
    d->buf.set_selections(keep);
    return d->buf.revision() != before;
}

bool ScriptWorkspace::save(int id, std::string& error) {
    ScriptDoc* d = doc(id);
    if (d == nullptr) { error = "No such tab."; return false; }
    if (format_on_save) format(id);
    std::ofstream out(d->path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "Could not write " + d->rel; return false; }
    const std::string bytes = d->buf.text_for_save();
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) { error = "Could not write " + d->rel; return false; }
    d->buf.mark_saved();
    d->disk_mtime = mtime_of(d->path);
    d->missing = false;
    return true;
}

bool ScriptWorkspace::save_all(std::string& error) {
    bool ok = true;
    for (const auto& d : docs_) if (d->buf.dirty() && !save(d->id, error)) ok = false;
    return ok;
}

bool ScriptWorkspace::refresh_from_disk() {
    bool changed = false;
    for (auto& d : docs_) {
        std::error_code ec;
        if (!fs::exists(d->path, ec)) { d->missing = true; continue; }
        const std::int64_t m = mtime_of(d->path);
        if (m == d->disk_mtime || d->buf.dirty()) continue;
        std::string text, err;
        if (!read_file(d->path, text, err)) continue;
        d->buf.set_text(text);
        d->disk_mtime = m;
        changed = true;
    }
    return changed;
}

void ScriptWorkspace::sync(ScriptDoc& d) {
    if (d.synced_rev == d.buf.revision()) return;
    server_->change_document(uri(d), d.buf.text(), ++d.version);
    d.synced_rev = d.buf.revision();
}

void ScriptWorkspace::shift_breakpoints(std::set<int>& lines, const LineChange& c) {
    // 0-based change [first, first+removed) became `added` lines; breakpoints are 1-based.
    const int keep = std::min(c.removed, c.added);
    const int delta = c.added - c.removed;
    std::set<int> out;
    for (const int l : lines) {
        const int z = l - 1;
        if (z < c.first + keep) out.insert(l);
        else if (z < c.first + c.removed) continue; // deleted
        else out.insert(l + delta);
    }
    lines = std::move(out);
}

void ScriptWorkspace::set_breakpoints(Breakpoints b) {
    // Keep the sets the open documents point at.
    for (auto& [rel, set] : bps_) set.clear();
    for (auto& [rel, set] : b) bps_[rel] = std::move(set);
    ++bp_rev_;
}

bool ScriptWorkspace::toggle_breakpoint(const std::string& rel, int line) {
    std::set<int>& set = bps_[rel];
    ++bp_rev_;
    if (set.erase(line) > 0) return false;
    set.insert(line);
    return true;
}

void ScriptWorkspace::clear_breakpoints() {
    for (auto& [rel, set] : bps_) set.clear();
    ++bp_rev_;
}

void ScriptWorkspace::sync_all() {
    for (auto& d : docs_) sync(*d);
}

std::vector<RefHit> ScriptWorkspace::references(int id, TextPos at) {
    std::vector<RefHit> out;
    ScriptDoc* d = doc(id);
    if (d == nullptr) return out;
    sync_all();
    std::map<std::string, TextBuffer> disk; // lines of files without a tab
    for (const pluma::lsp::Location& loc : server_->references(uri(*d), to_lsp(*d, at), true)) {
        RefHit h;
        h.path = pluma::lsp::uri_to_path(loc.uri);
        std::error_code ec;
        const fs::path rel = fs::relative(h.path, dir_, ec);
        h.rel = slashes((ec || rel.empty() || rel.string().starts_with("..")) ? h.path.string() : rel.string());
        const int open_id = find(h.path);
        h.open = open_id >= 0;
        std::string line;
        if (const ScriptDoc* o = doc(open_id)) {
            h.range = from_lsp(*o, loc.range);
            line = o->buf.line(std::clamp(h.range.start.line, 0, o->buf.line_count() - 1));
        } else {
            auto it = disk.find(h.rel);
            if (it == disk.end()) {
                std::string text, err;
                TextBuffer tmp;
                if (read_file(h.path, text, err)) tmp.set_text(text);
                it = disk.emplace(h.rel, std::move(tmp)).first;
            }
            const TextBuffer& tb = it->second;
            h.range = {tb.from_utf16(static_cast<int>(loc.range.start.line), static_cast<int>(loc.range.start.character)),
                       tb.from_utf16(static_cast<int>(loc.range.end.line), static_cast<int>(loc.range.end.character))};
            line = tb.line(std::clamp(h.range.start.line, 0, tb.line_count() - 1));
        }
        const std::size_t a = line.find_first_not_of(" \t");
        h.preview = a == std::string::npos ? std::string() : line.substr(a);
        out.push_back(std::move(h));
    }
    std::sort(out.begin(), out.end(), [](const RefHit& x, const RefHit& y) { return x.rel != y.rel ? x.rel < y.rel : x.range.start < y.range.start; });
    return out;
}

std::optional<TextRange> ScriptWorkspace::prepare_rename(int id, TextPos at) {
    ScriptDoc* d = doc(id);
    if (d == nullptr) return std::nullopt;
    sync_all();
    const auto r = server_->prepare_rename(uri(*d), to_lsp(*d, at));
    if (!r) return std::nullopt;
    return from_lsp(*d, *r);
}

std::optional<RenamePlan> ScriptWorkspace::plan_rename(int id, TextPos at, const std::string& new_name) {
    ScriptDoc* d = doc(id);
    if (d == nullptr) return std::nullopt;
    sync_all();
    auto we = server_->rename(uri(*d), to_lsp(*d, at), new_name);
    if (!we) return std::nullopt;
    RenamePlan plan;
    for (const auto& [u, edits] : we->changes) {
        if (edits.empty()) continue;
        ++plan.files;
        plan.edits += static_cast<int>(edits.size());
        const fs::path path = pluma::lsp::uri_to_path(u);
        std::error_code ec;
        const fs::path rel = fs::relative(path, dir_, ec);
        plan.rels.push_back(slashes((ec || rel.empty() || rel.string().starts_with("..")) ? path.string() : rel.string()));
        if (const ScriptDoc* o = doc(find(path)); o != nullptr && o->buf.dirty()) ++plan.unsaved;
    }
    plan.edit = std::move(*we);
    return plan;
}

std::vector<int> ScriptWorkspace::apply_edit(const pluma::lsp::WorkspaceEdit& edit, std::string& error) {
    std::vector<int> touched;
    const int keep = active_;
    for (const auto& [u, edits] : edit.changes) {
        if (edits.empty()) continue;
        const fs::path path = pluma::lsp::uri_to_path(u);
        int id = find(path);
        if (id < 0) id = open(path, error);
        ScriptDoc* d = doc(id);
        if (d == nullptr) { if (error.empty()) error = "Cannot open " + path.string(); continue; }
        std::vector<std::pair<TextRange, std::string>> list;
        for (const pluma::lsp::TextEdit& e : edits) list.emplace_back(from_lsp(*d, e.range), e.new_text);
        d->buf.replace_all(std::move(list));
        d->buf.seal();
        touched.push_back(id);
    }
    if (keep >= 0 && doc(keep) != nullptr) activate(keep);
    return touched;
}

void ScriptWorkspace::recompute(ScriptDoc& d) {
    sync_all(); // other tabs may define what this one uses (a rename edits several files at once)
    d.problems.clear();
    for (const pluma::lsp::DiagnosticItem& it : server_->diagnostics(uri(d))) {
        Problem p;
        p.range = from_lsp(d, it.range);
        p.severity = it.severity == pluma::Severity::Error ? ProblemSeverity::Error : it.severity == pluma::Severity::Warning ? ProblemSeverity::Warning : ProblemSeverity::Note;
        p.message = it.message;
        p.code = it.code;
        d.problems.push_back(std::move(p));
    }
    std::stable_sort(d.problems.begin(), d.problems.end(), [](const Problem& a, const Problem& b) { return a.range.start < b.range.start; });
    d.outline.clear();
    flatten(server_->document_symbols(uri(d)), 0, d, d.outline);
    d.checked_rev = d.buf.revision();
    // Semantic colors for the text the server just saw (types, functions, constants).
    d.syn.update(d.buf);
    const std::vector<std::string>& legend = pluma::lsp::semantic_token_types();
    std::vector<SyntaxCache::SemanticSpan> sem;
    for (const pluma::lsp::SemanticToken& t : server_->semantic_tokens(uri(d))) {
        if (t.type >= legend.size()) continue;
        const std::string& ty = legend[t.type];
        Syn k = Syn::Plain;
        if (ty == "type") k = Syn::Type;
        else if (ty == "function") k = Syn::Function;
        else if (ty == "constant" || ty == "enumMember") k = Syn::Constant;
        if (k == Syn::Plain) continue;
        const TextPos at = from_lsp(d, pluma::lsp::Position{t.line, t.character});
        sem.push_back({static_cast<std::uint32_t>(at.line), static_cast<std::uint32_t>(at.col), t.length, k});
    }
    d.syn.set_semantic(sem);
}

void ScriptWorkspace::check_now(int id) {
    if (ScriptDoc* d = doc(id)) recompute(*d);
}

bool ScriptWorkspace::tick(std::uint64_t now_ms) {
    bool changed = false;
    for (auto& d : docs_) {
        if (d->checked_rev == d->buf.revision()) continue;
        // Every new revision restarts the debounce; a document's first check is immediate.
        if (d->due_rev != d->buf.revision()) {
            d->due_rev = d->buf.revision();
            d->due_ms = d->checked_rev == ~std::uint64_t{0} ? now_ms : now_ms + kDebounceMs;
        }
        if (now_ms >= d->due_ms) { recompute(*d); changed = true; }
    }
    return changed;
}

int ScriptWorkspace::error_count(const ScriptDoc& d) const {
    return static_cast<int>(std::count_if(d.problems.begin(), d.problems.end(), [](const Problem& p) { return p.severity == ProblemSeverity::Error; }));
}
int ScriptWorkspace::warning_count(const ScriptDoc& d) const {
    return static_cast<int>(std::count_if(d.problems.begin(), d.problems.end(), [](const Problem& p) { return p.severity == ProblemSeverity::Warning; }));
}

std::vector<std::string> ScriptWorkspace::project_scripts() const {
    std::vector<std::string> out;
    if (dir_.empty()) return out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir_, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec) && (name.starts_with('.') || name == "build" || name == "node_modules")) { it.disable_recursion_pending(); continue; }
        if (it->is_regular_file(ec) && it->path().extension() == ".plm") out.push_back(slashes(fs::relative(it->path(), dir_, ec).string()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace cuervo::editor
