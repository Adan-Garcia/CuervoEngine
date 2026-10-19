#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The release archive (DESIGN.md 4.11 "A release export packs everything into
// a single archive per platform"; docs/EXPORT.md "Pack format").
//
// Layout (little-endian, every offset absolute, sections 16-byte aligned):
//
//   header   64 bytes
//     0  char[4] magic "CPAK"
//     4  u32     format version (pack_version)
//     8  u32     platform (PackPlatform)
//    12  u32     entry count
//    16  u64     table offset   (= 64)
//    24  u64     names offset
//    32  u64     data offset
//    40  u64     file size
//    48  u64     content hash: FNV-1a 64 of bytes [64, file size)
//    56  u32     flags (0)
//    60  u32     reserved (0)
//   table    entry count x 32 bytes, sorted by path (bytewise UTF-8)
//     u32 name offset (into the names blob), u32 name size,
//     u64 data offset (absolute), u64 size, u32 compression (0 = stored), u32 reserved
//   names    the paths, concatenated (no terminators)
//   data     each entry's bytes, in table order, 16-byte aligned, zero padded
//
// Deterministic: the same entries give byte-identical packs (sorted table, no
// timestamps, zeroed padding), so a release can be rebuilt and compared.
// Entries are stored uncompressed: no compression library is pinned in
// cmake/Dependencies.cmake yet, the payload is mostly already-compact engine
// formats, and the web server compresses the download (gzip/brotli). The
// per-entry `compression` field is reserved for adding one later without a
// format break (readers reject codes they don't know).
//
// Paths are project-relative with forward slashes ("scripts/game.plm"), no
// empty, "." or ".." components and no leading slash. Lookups are exact
// (case-sensitive) on every platform, so a game behaves the same everywhere.
namespace cuervo::assets {

inline constexpr char pack_magic[4] = {'C', 'P', 'A', 'K'};
inline constexpr std::uint32_t pack_version = 1;
inline constexpr std::size_t pack_header_size = 64;
inline constexpr std::size_t pack_entry_size = 32;
inline constexpr std::size_t pack_alignment = 16;

// The archive's target (one archive per platform, DESIGN.md 4.11): what the
// import step produced for. Any = no platform-specific content.
enum class PackPlatform : std::uint32_t { Any = 0, Linux = 1, Web = 2, Windows = 3 };
std::string_view platform_name(PackPlatform p);
std::optional<PackPlatform> parse_platform(std::string_view text);
// The platform this binary runs on.
PackPlatform host_platform();

// True when `path` is a valid pack path (see above); otherwise false and,
// if given, the reason in `why`.
bool valid_pack_path(std::string_view path, std::string* why = nullptr);
// Pack path ordering: bytewise, so the table can be binary searched.
inline bool pack_path_less(std::string_view a, std::string_view b) { return a < b; }

// Builds a pack in memory. Entries may be added in any order.
class PackBuilder {
public:
    explicit PackBuilder(PackPlatform platform = PackPlatform::Any) : platform_(platform) {}

    // False (with `error`) for an invalid path or a path added twice.
    bool add(std::string path, std::vector<std::byte> data, std::string& error);
    bool contains(std::string_view path) const;
    std::size_t size() const { return entries_.size(); }
    PackPlatform platform() const { return platform_; }

    // The archive's bytes; the same entries always give the same bytes.
    std::vector<std::byte> build() const;
    // build() written to `file` (through a temporary file, then renamed).
    bool write(const std::filesystem::path& file, std::string& error) const;

private:
    struct Entry {
        std::string path;
        std::vector<std::byte> data;
    };
    PackPlatform platform_;
    std::vector<Entry> entries_;
};

struct PackEntry {
    std::string_view path;
    std::span<const std::byte> data;
};

struct PackOpenOptions {
    // Check the content hash (reads every byte once). Off only for tools
    // that want to inspect a damaged pack.
    bool verify = true;
};

// A read-only archive: the file is memory-mapped where the OS allows it
// (POSIX), otherwise read into memory (Windows, the web's MEMFS). Views stay
// valid as long as the Pack lives, so callers hold the shared_ptr.
class Pack {
public:
    static std::shared_ptr<const Pack> open(const std::filesystem::path& file, std::string& error,
                                            PackOpenOptions options = {});
    static std::shared_ptr<const Pack> from_bytes(std::vector<std::byte> bytes, std::string& error,
                                                  PackOpenOptions options = {});
    ~Pack();
    Pack(const Pack&) = delete;
    Pack& operator=(const Pack&) = delete;

    // The entry's bytes, or nullopt when there is no such path.
    std::optional<std::span<const std::byte>> find(std::string_view path) const;
    bool contains(std::string_view path) const { return find(path).has_value(); }
    // Every entry, sorted by path.
    const std::vector<PackEntry>& entries() const { return entries_; }

    PackPlatform platform() const { return platform_; }
    std::uint64_t content_hash() const { return content_hash_; }
    std::uint32_t version() const { return version_; }
    std::size_t size_bytes() const { return bytes_.size(); }
    // Where it was opened from ("" for from_bytes), for messages.
    const std::string& source() const { return source_; }

private:
    Pack() = default;
    bool parse(std::string& error, const PackOpenOptions& options);

    std::span<const std::byte> bytes_;
    std::vector<std::byte> owned_;
    void* mapping_ = nullptr;
    std::size_t mapping_size_ = 0;
    std::vector<PackEntry> entries_;
    PackPlatform platform_ = PackPlatform::Any;
    std::uint64_t content_hash_ = 0;
    std::uint32_t version_ = 0;
    std::string source_;
};

// FNV-1a 64 (cuervo/core/hash.hpp) of a pack's content (everything after the
// header), as stored in its header.
std::uint64_t pack_content_hash(std::span<const std::byte> after_header);

} // namespace cuervo::assets
