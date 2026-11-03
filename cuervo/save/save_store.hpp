#pragma once

// Save data (DESIGN.md 7.2 "Save data", 4.12 for the versioning model).
//
// A save *slot* is a named bag of typed values (bool, int, float, string)
// stored as one file in the per-user directory (platform::user_data_dir).
// Like scene files (cuervo/scene/scene_file.hpp) a slot carries a format
// version; a game registers the slot's current version plus a chain of
// migrations, and load() upgrades older files step by step. A file written by
// a newer build is refused rather than half-read.
//
// Layout (little-endian, cuervo/core/binary_io.hpp):
//   "ESAV"  u32 version  u32 count  { str key, u8 tag, payload }*  u64 fnv1a(all before)
//
// Saves are outside the simulation: nothing here is hashed or replayed.
// Whatever a script loads and then stores into its own state becomes part of
// the sim hash from then on, which is the intended boundary.
//
// Storage is behind `Backend` so the web build (11.9) can plug in IndexedDB
// (an async store flushed into a synchronous cache); FileBackend and
// MemoryBackend are the native and test implementations.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cuervo::save {

using Value = std::variant<bool, std::int64_t, double, std::string>;

struct SlotData {
    std::map<std::string, Value, std::less<>> fields; // ordered: stable encoding

    void set(std::string key, Value v) { fields[std::move(key)] = std::move(v); }
    const Value* find(std::string_view key) const;
    std::int64_t get_int(std::string_view key, std::int64_t fallback = 0) const;
    double get_float(std::string_view key, double fallback = 0.0) const;
    bool get_bool(std::string_view key, bool fallback = false) const;
    std::string get_str(std::string_view key, std::string_view fallback = {}) const;
    friend bool operator==(const SlotData&, const SlotData&) = default;
};

// Byte storage by slot name. Implementations: FileBackend, MemoryBackend, and
// (web, 11.9) an IndexedDB-backed one installed with SaveStore::set_backend.
class Backend {
public:
    virtual ~Backend() = default;
    virtual bool read(std::string_view slot, std::vector<std::byte>& out) = 0; // false: no such slot
    virtual bool write(std::string_view slot, std::span<const std::byte> data) = 0;
    virtual bool remove(std::string_view slot) = 0;
    virtual std::vector<std::string> list() = 0; // sorted
};

class MemoryBackend final : public Backend {
public:
    bool read(std::string_view slot, std::vector<std::byte>& out) override;
    bool write(std::string_view slot, std::span<const std::byte> data) override;
    bool remove(std::string_view slot) override;
    std::vector<std::string> list() override;
    std::map<std::string, std::vector<std::byte>, std::less<>> files; // visible to tests
};

// `<dir>/<slot>.sav`, written to a temporary file and renamed so a crash
// leaves the previous save intact.
class FileBackend final : public Backend {
public:
    explicit FileBackend(std::filesystem::path dir) : dir_(std::move(dir)) {}
    bool read(std::string_view slot, std::vector<std::byte>& out) override;
    bool write(std::string_view slot, std::span<const std::byte> data) override;
    bool remove(std::string_view slot) override;
    std::vector<std::string> list() override;
    const std::filesystem::path& dir() const { return dir_; }

private:
    std::filesystem::path path_of(std::string_view slot) const;
    std::filesystem::path dir_;
};

// Forwards to `inner` and calls `flush` after every successful write/remove.
// The web build wraps a FileBackend on an IDBFS mount with a flush that
// schedules an IndexedDB sync: the Backend interface stays synchronous (reads
// and writes hit the in-memory file system) while persistence is asynchronous
// write-through (docs/WEB.md "Persistent saves").
class WriteThroughBackend final : public Backend {
public:
    WriteThroughBackend(std::unique_ptr<Backend> inner, std::function<void()> flush)
        : inner_(std::move(inner)), flush_(std::move(flush)) {}
    bool read(std::string_view slot, std::vector<std::byte>& out) override { return inner_->read(slot, out); }
    bool write(std::string_view slot, std::span<const std::byte> data) override;
    bool remove(std::string_view slot) override;
    std::vector<std::string> list() override { return inner_->list(); }

private:
    std::unique_ptr<Backend> inner_;
    std::function<void()> flush_;
};

struct Migration {
    std::uint32_t from = 0; // a step from `from` to `from + 1`
    const char* what = "";
    void (*apply)(SlotData&) = nullptr;
};

struct LoadInfo {
    std::uint32_t version = 0;         // as stored in the file
    std::vector<std::string> migrated; // descriptions of the migrations applied
};

inline constexpr char magic[4] = {'E', 'S', 'A', 'V'};

// Slot names: 1-64 characters from [A-Za-z0-9_-].
bool valid_slot_name(std::string_view name);

std::vector<std::byte> encode(const SlotData& data, std::uint32_t version);
std::optional<SlotData> decode(std::span<const std::byte> bytes, std::uint32_t& version, std::string& error);

class SaveStore {
public:
    explicit SaveStore(std::unique_ptr<Backend> backend);
    // The per-user directory of `game`; falls back to memory when there is none.
    static SaveStore for_game(std::string_view game);

    void set_backend(std::unique_ptr<Backend> backend);
    Backend& backend() { return *backend_; }

    // Declares a slot's current version and its migrations (any order; each
    // `from` once). Undeclared slots are version 1 and never migrated.
    void declare(std::string slot, std::uint32_t current, std::vector<Migration> migrations = {});
    std::uint32_t current_version(std::string_view slot) const;

    bool save(std::string_view slot, const SlotData& data, std::string* error = nullptr);
    // Same, with an explicit version (scripts that migrate on their own).
    bool save_versioned(std::string_view slot, const SlotData& data, std::uint32_t version,
                        std::string* error = nullptr);
    // nullopt: missing, corrupt, newer than current, or a migration is missing.
    std::optional<SlotData> load(std::string_view slot, std::string& error, LoadInfo* info = nullptr);
    bool exists(std::string_view slot);
    bool remove(std::string_view slot);
    std::vector<std::string> slots() { return backend_->list(); }

private:
    struct Schema {
        std::uint32_t current = 1;
        std::vector<Migration> migrations;
    };
    std::unique_ptr<Backend> backend_;
    std::map<std::string, Schema, std::less<>> schemas_;
};

} // namespace cuervo::save
