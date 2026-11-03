#include "cuervo/save/save_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <system_error>

#include "cuervo/core/binary_io.hpp"
#include "cuervo/platform/platform.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace cuervo::save {

namespace {

enum Tag : std::uint8_t { tag_bool = 0, tag_int = 1, tag_float = 2, tag_str = 3 };

std::uint64_t fnv1a(std::span<const std::byte> data) {
    std::uint64_t h = 14695981039346656037ull;
    for (const std::byte b : data) {
        h ^= static_cast<std::uint64_t>(b);
        h *= 1099511628211ull;
    }
    return h;
}

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

} // namespace

const Value* SlotData::find(std::string_view key) const {
    const auto it = fields.find(key);
    return it == fields.end() ? nullptr : &it->second;
}

std::int64_t SlotData::get_int(std::string_view key, std::int64_t fallback) const {
    const Value* v = find(key);
    return v != nullptr && std::holds_alternative<std::int64_t>(*v) ? std::get<std::int64_t>(*v) : fallback;
}

double SlotData::get_float(std::string_view key, double fallback) const {
    const Value* v = find(key);
    if (v == nullptr) return fallback;
    if (std::holds_alternative<double>(*v)) return std::get<double>(*v);
    if (std::holds_alternative<std::int64_t>(*v)) return static_cast<double>(std::get<std::int64_t>(*v));
    return fallback;
}

bool SlotData::get_bool(std::string_view key, bool fallback) const {
    const Value* v = find(key);
    return v != nullptr && std::holds_alternative<bool>(*v) ? std::get<bool>(*v) : fallback;
}

std::string SlotData::get_str(std::string_view key, std::string_view fallback) const {
    const Value* v = find(key);
    return v != nullptr && std::holds_alternative<std::string>(*v) ? std::get<std::string>(*v)
                                                                   : std::string(fallback);
}

// ---- backends -----------------------------------------------------------------

bool MemoryBackend::read(std::string_view slot, std::vector<std::byte>& out) {
    const auto it = files.find(slot);
    if (it == files.end()) return false;
    out = it->second;
    return true;
}
bool MemoryBackend::write(std::string_view slot, std::span<const std::byte> data) {
    files[std::string(slot)].assign(data.begin(), data.end());
    return true;
}
bool MemoryBackend::remove(std::string_view slot) {
    const auto it = files.find(slot);
    if (it == files.end()) return false;
    files.erase(it);
    return true;
}
std::vector<std::string> MemoryBackend::list() {
    std::vector<std::string> out;
    for (const auto& [name, bytes] : files) out.push_back(name);
    return out;
}

std::filesystem::path FileBackend::path_of(std::string_view slot) const {
    return dir_ / platform::path_from_utf8(std::string(slot) + ".sav");
}

bool FileBackend::read(std::string_view slot, std::vector<std::byte>& out) {
    std::ifstream in(path_of(slot), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) return false;
    in.seekg(0);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) in.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(in);
}

bool FileBackend::write(std::string_view slot, std::span<const std::byte> data) {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    const std::filesystem::path target = path_of(slot);
    std::filesystem::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        if (!data.empty()) {
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        out.flush();
        if (!out) return false;
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool FileBackend::remove(std::string_view slot) {
    std::error_code ec;
    return std::filesystem::remove(path_of(slot), ec) && !ec;
}

std::vector<std::string> FileBackend::list() {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sav") continue;
        out.push_back(platform::utf8(entry.path().stem()));
    }
    std::ranges::sort(out);
    return out;
}

// ---- format -------------------------------------------------------------------

bool valid_slot_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

std::vector<std::byte> encode(const SlotData& data, std::uint32_t version) {
    ByteWriter w;
    w.raw(magic, 4);
    w.u32(version);
    w.u32(static_cast<std::uint32_t>(data.fields.size()));
    for (const auto& [key, value] : data.fields) {
        w.str(key);
        if (const bool* b = std::get_if<bool>(&value)) {
            w.u8(tag_bool);
            w.u8(*b ? 1 : 0);
        } else if (const auto* i = std::get_if<std::int64_t>(&value)) {
            w.u8(tag_int);
            w.i64(*i);
        } else if (const double* d = std::get_if<double>(&value)) {
            w.u8(tag_float);
            w.f64(*d);
        } else {
            w.u8(tag_str);
            w.str(std::get<std::string>(value));
        }
    }
    std::vector<std::byte> bytes = w.take();
    ByteWriter tail;
    tail.u64(fnv1a(bytes));
    const std::vector<std::byte> t = tail.take();
    bytes.insert(bytes.end(), t.begin(), t.end());
    return bytes;
}

std::optional<SlotData> decode(std::span<const std::byte> bytes, std::uint32_t& version, std::string& error) {
    if (bytes.size() < 4 + 4 + 4 + 8 || std::memcmp(bytes.data(), magic, 4) != 0) {
        error = "not a save file";
        return std::nullopt;
    }
    const auto body = bytes.first(bytes.size() - 8);
    std::uint64_t stored = 0;
    std::memcpy(&stored, bytes.data() + body.size(), 8);
    if (stored != fnv1a(body)) {
        error = "save file is corrupt (checksum mismatch)";
        return std::nullopt;
    }
    ByteReader r(body, "Save");
    char m[4];
    r.raw(m, 4);
    version = r.u32();
    const std::uint32_t n = r.count(6);
    SlotData out;
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        std::string key = r.str();
        switch (r.u8()) {
        case tag_bool: out.fields[key] = r.u8() != 0; break;
        case tag_int: out.fields[key] = r.i64(); break;
        case tag_float: out.fields[key] = r.f64(); break;
        case tag_str: out.fields[key] = r.str(); break;
        default: r.fail("unknown value tag"); break;
        }
    }
    if (!r.ok() || !r.at_end()) {
        error = r.ok() ? "trailing bytes" : r.error();
        return std::nullopt;
    }
    return out;
}

// ---- store --------------------------------------------------------------------

SaveStore::SaveStore(std::unique_ptr<Backend> backend) : backend_(std::move(backend)) {}

bool WriteThroughBackend::write(std::string_view slot, std::span<const std::byte> data) {
    const bool ok = inner_->write(slot, data);
    if (ok && flush_) flush_();
    return ok;
}

bool WriteThroughBackend::remove(std::string_view slot) {
    const bool ok = inner_->remove(slot);
    if (ok && flush_) flush_();
    return ok;
}

SaveStore SaveStore::for_game(std::string_view game) {
#ifdef __EMSCRIPTEN__
    // /persist is an IDBFS mount filled from IndexedDB before main() (web/idbfs_saves.js).
    std::string folder;
    for (const char c : game) folder += (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_') ? c : '_';
    if (folder.empty()) folder = "Game";
    return SaveStore(std::make_unique<WriteThroughBackend>(
        std::make_unique<FileBackend>(std::filesystem::path("/persist") / folder / "saves"),
        [] { EM_ASM({ if (Module.cuervoPersistFlush) Module.cuervoPersistFlush(); }); }));
#else
    const std::filesystem::path dir = platform::user_data_dir(game);
    if (dir.empty()) return SaveStore(std::make_unique<MemoryBackend>());
    return SaveStore(std::make_unique<FileBackend>(dir / "saves"));
#endif
}

void SaveStore::set_backend(std::unique_ptr<Backend> backend) { backend_ = std::move(backend); }

void SaveStore::declare(std::string slot, std::uint32_t current, std::vector<Migration> migrations) {
    Schema& s = schemas_[std::move(slot)];
    s.current = std::max<std::uint32_t>(current, 1);
    s.migrations = std::move(migrations);
}

std::uint32_t SaveStore::current_version(std::string_view slot) const {
    const auto it = schemas_.find(slot);
    return it == schemas_.end() ? 1 : it->second.current;
}

bool SaveStore::save(std::string_view slot, const SlotData& data, std::string* error) {
    return save_versioned(slot, data, current_version(slot), error);
}

bool SaveStore::save_versioned(std::string_view slot, const SlotData& data, std::uint32_t version,
                               std::string* error) {
    if (!valid_slot_name(slot)) return fail(error, std::format("bad slot name '{}'", slot));
    const std::vector<std::byte> bytes = encode(data, version);
    if (!backend_->write(slot, bytes)) return fail(error, std::format("could not write slot '{}'", slot));
    return true;
}

std::optional<SlotData> SaveStore::load(std::string_view slot, std::string& error, LoadInfo* info) {
    if (!valid_slot_name(slot)) {
        error = std::format("bad slot name '{}'", slot);
        return std::nullopt;
    }
    std::vector<std::byte> bytes;
    if (!backend_->read(slot, bytes)) {
        error = std::format("no slot '{}'", slot);
        return std::nullopt;
    }
    std::uint32_t version = 0;
    std::optional<SlotData> data = decode(bytes, version, error);
    if (!data) return std::nullopt;
    if (info != nullptr) {
        info->version = version;
        info->migrated.clear();
    }
    const auto it = schemas_.find(slot);
    if (it == schemas_.end()) return data; // undeclared: handed over as stored
    const Schema& schema = it->second;
    if (version > schema.current) {
        error = std::format("slot '{}' is version {}, newer than this build's {}", slot, version, schema.current);
        return std::nullopt;
    }
    for (std::uint32_t v = version; v < schema.current; ++v) {
        const auto m = std::ranges::find(schema.migrations, v, &Migration::from);
        if (m == schema.migrations.end() || m->apply == nullptr) {
            error = std::format("slot '{}' has no migration from version {}", slot, v);
            return std::nullopt;
        }
        m->apply(*data);
        if (info != nullptr) info->migrated.emplace_back(m->what);
    }
    return data;
}

bool SaveStore::exists(std::string_view slot) {
    std::vector<std::byte> bytes;
    return valid_slot_name(slot) && backend_->read(slot, bytes);
}

bool SaveStore::remove(std::string_view slot) { return valid_slot_name(slot) && backend_->remove(slot); }

} // namespace cuervo::save
