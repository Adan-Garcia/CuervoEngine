#include "cuervo/assets/pack.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <system_error>

#include "cuervo/core/hash.hpp"
#include "cuervo/platform/platform.hpp"

#if defined(__unix__) && !defined(__EMSCRIPTEN__)
#define CUERVO_PACK_MMAP 1
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cuervo::assets {

namespace fs = std::filesystem;

namespace {

std::size_t align_up(std::size_t n) { return (n + pack_alignment - 1) / pack_alignment * pack_alignment; }

void put_u32(std::vector<std::byte>& out, std::size_t at, std::uint32_t v) { std::memcpy(out.data() + at, &v, 4); }
void put_u64(std::vector<std::byte>& out, std::size_t at, std::uint64_t v) { std::memcpy(out.data() + at, &v, 8); }
std::uint32_t get_u32(std::span<const std::byte> in, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, in.data() + at, 4);
    return v;
}
std::uint64_t get_u64(std::span<const std::byte> in, std::size_t at) {
    std::uint64_t v = 0;
    std::memcpy(&v, in.data() + at, 8);
    return v;
}

} // namespace

std::string_view platform_name(PackPlatform p) {
    switch (p) {
        case PackPlatform::Any:     return "any";
        case PackPlatform::Linux:   return "linux";
        case PackPlatform::Web:     return "web";
        case PackPlatform::Windows: return "windows";
    }
    return "unknown";
}

std::optional<PackPlatform> parse_platform(std::string_view text) {
    for (PackPlatform p : {PackPlatform::Any, PackPlatform::Linux, PackPlatform::Web, PackPlatform::Windows}) {
        if (platform_name(p) == text) return p;
    }
    return std::nullopt;
}

PackPlatform host_platform() {
    switch (platform::os) {
        case platform::Os::Linux:   return PackPlatform::Linux;
        case platform::Os::Web:     return PackPlatform::Web;
        case platform::Os::Windows: return PackPlatform::Windows;
        case platform::Os::Other:   return PackPlatform::Any;
    }
    return PackPlatform::Any;
}

bool valid_pack_path(std::string_view path, std::string* why) {
    auto fail = [&](std::string_view reason) {
        if (why != nullptr) *why = std::format("'{}': {}", path, reason);
        return false;
    };
    if (path.empty()) return fail("empty path");
    if (path.size() > 4096) return fail("path too long");
    if (path.front() == '/') return fail("pack paths are relative (no leading '/')");
    if (path.find('\\') != std::string_view::npos) return fail("pack paths use '/', not '\\'");
    if (path.find('\0') != std::string_view::npos) return fail("NUL in path");
    if (path.size() >= 2 && path[1] == ':') return fail("pack paths are relative (no drive letter)");
    for (std::size_t pos = 0; pos <= path.size();) {
        std::size_t end = path.find('/', pos);
        if (end == std::string_view::npos) end = path.size();
        const std::string_view part = path.substr(pos, end - pos);
        if (part.empty()) return fail("empty path component");
        if (part == "." || part == "..") return fail("'.' and '..' are not allowed");
        pos = end + 1;
    }
    return true;
}

// ------------------------------------------------------------ writing

bool PackBuilder::add(std::string path, std::vector<std::byte> data, std::string& error) {
    if (!valid_pack_path(path, &error)) return false;
    if (contains(path)) {
        error = std::format("'{}' is already in the pack", path);
        return false;
    }
    entries_.push_back({std::move(path), std::move(data)});
    return true;
}

bool PackBuilder::contains(std::string_view path) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.path == path; });
}

std::vector<std::byte> PackBuilder::build() const {
    std::vector<const Entry*> sorted;
    sorted.reserve(entries_.size());
    for (const Entry& e : entries_) sorted.push_back(&e);
    std::sort(sorted.begin(), sorted.end(), [](const Entry* a, const Entry* b) { return pack_path_less(a->path, b->path); });

    const std::size_t table_offset = pack_header_size;
    const std::size_t names_offset = align_up(table_offset + sorted.size() * pack_entry_size);
    std::size_t names_size = 0;
    for (const Entry* e : sorted) names_size += e->path.size();
    const std::size_t data_offset = align_up(names_offset + names_size);
    std::size_t total = data_offset;
    for (const Entry* e : sorted) total = align_up(total) + e->data.size();
    total = align_up(total);

    std::vector<std::byte> out(total, std::byte{0}); // padding stays zero: deterministic bytes
    std::memcpy(out.data(), pack_magic, 4);
    put_u32(out, 4, pack_version);
    put_u32(out, 8, static_cast<std::uint32_t>(platform_));
    put_u32(out, 12, static_cast<std::uint32_t>(sorted.size()));
    put_u64(out, 16, table_offset);
    put_u64(out, 24, names_offset);
    put_u64(out, 32, data_offset);
    put_u64(out, 40, total);

    std::size_t name_at = 0;
    std::size_t data_at = data_offset;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const Entry& e = *sorted[i];
        const std::size_t row = table_offset + i * pack_entry_size;
        data_at = align_up(data_at);
        put_u32(out, row + 0, static_cast<std::uint32_t>(name_at));
        put_u32(out, row + 4, static_cast<std::uint32_t>(e.path.size()));
        put_u64(out, row + 8, data_at);
        put_u64(out, row + 16, e.data.size());
        put_u32(out, row + 24, 0); // stored
        std::memcpy(out.data() + names_offset + name_at, e.path.data(), e.path.size());
        if (!e.data.empty()) std::memcpy(out.data() + data_at, e.data.data(), e.data.size());
        name_at += e.path.size();
        data_at += e.data.size();
    }
    put_u64(out, 48, pack_content_hash(std::span<const std::byte>(out).subspan(pack_header_size)));
    return out;
}

bool PackBuilder::write(const fs::path& file, std::string& error) const {
    const std::vector<std::byte> bytes = build();
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            error = std::format("can't write '{}'", platform::utf8(tmp));
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        error = std::format("can't move '{}' to '{}': {}", platform::utf8(tmp), platform::utf8(file), ec.message());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::uint64_t pack_content_hash(std::span<const std::byte> after_header) {
    StateHasher h;
    h.bytes(after_header.data(), after_header.size());
    return h.digest();
}

// ------------------------------------------------------------ reading

Pack::~Pack() {
#if defined(CUERVO_PACK_MMAP)
    if (mapping_ != nullptr) munmap(mapping_, mapping_size_);
#else
    (void)mapping_; // read into owned_ instead (no mmap on this platform)
    (void)mapping_size_;
#endif
}

std::shared_ptr<const Pack> Pack::open(const fs::path& file, std::string& error, PackOpenOptions options) {
    std::shared_ptr<Pack> pack(new Pack());
    pack->source_ = platform::utf8(file);
#if defined(CUERVO_PACK_MMAP)
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = std::format("can't open pack '{}'", pack->source_);
        return nullptr;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        error = std::format("'{}': empty or unreadable pack", pack->source_);
        return nullptr;
    }
    const auto size = static_cast<std::size_t>(st.st_size);
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd); // the mapping keeps the file alive
    if (map == MAP_FAILED) {
        error = std::format("'{}': can't map the pack", pack->source_);
        return nullptr;
    }
    pack->mapping_ = map;
    pack->mapping_size_ = size;
    pack->bytes_ = {static_cast<const std::byte*>(map), size};
#else
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        error = std::format("can't open pack '{}'", pack->source_);
        return nullptr;
    }
    const std::streamsize size = in.tellg();
    in.seekg(0);
    pack->owned_.resize(static_cast<std::size_t>(std::max<std::streamsize>(size, 0)));
    if (!in.read(reinterpret_cast<char*>(pack->owned_.data()), size)) {
        error = std::format("can't read pack '{}'", pack->source_);
        return nullptr;
    }
    pack->bytes_ = pack->owned_;
#endif
    if (!pack->parse(error, options)) {
        error = std::format("'{}': {}", pack->source_, error);
        return nullptr;
    }
    return pack;
}

std::shared_ptr<const Pack> Pack::from_bytes(std::vector<std::byte> bytes, std::string& error, PackOpenOptions options) {
    std::shared_ptr<Pack> pack(new Pack());
    pack->owned_ = std::move(bytes);
    pack->bytes_ = pack->owned_;
    if (!pack->parse(error, options)) return nullptr;
    return pack;
}

bool Pack::parse(std::string& error, const PackOpenOptions& options) {
    const std::span<const std::byte> b = bytes_;
    if (b.size() < pack_header_size || std::memcmp(b.data(), pack_magic, 4) != 0) {
        error = "not a Cuervo pack (bad magic)";
        return false;
    }
    version_ = get_u32(b, 4);
    if (version_ == 0 || version_ > pack_version) {
        error = std::format("pack format version {} is not supported (this runtime reads up to {})", version_,
                            pack_version);
        return false;
    }
    const std::uint32_t plat = get_u32(b, 8);
    if (plat > static_cast<std::uint32_t>(PackPlatform::Windows)) {
        error = std::format("unknown pack platform {}", plat);
        return false;
    }
    platform_ = static_cast<PackPlatform>(plat);
    const std::uint64_t count = get_u32(b, 12);
    const std::uint64_t table = get_u64(b, 16);
    const std::uint64_t names = get_u64(b, 24);
    const std::uint64_t data = get_u64(b, 32);
    const std::uint64_t size = get_u64(b, 40);
    content_hash_ = get_u64(b, 48);
    if (size != b.size()) {
        error = std::format("truncated or padded pack ({} bytes, header says {})", b.size(), size);
        return false;
    }
    if (table != pack_header_size || table + count * pack_entry_size > names || names > data || data > size) {
        error = "corrupt pack header (section offsets)";
        return false;
    }
    if (options.verify) {
        const std::uint64_t actual = pack_content_hash(b.subspan(pack_header_size));
        if (actual != content_hash_) {
            error = std::format("corrupt pack: content hash {:016x}, header says {:016x}", actual, content_hash_);
            return false;
        }
    }
    entries_.clear();
    entries_.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::size_t row = static_cast<std::size_t>(table + i * pack_entry_size);
        const std::uint64_t name_at = get_u32(b, row + 0);
        const std::uint64_t name_size = get_u32(b, row + 4);
        const std::uint64_t at = get_u64(b, row + 8);
        const std::uint64_t n = get_u64(b, row + 16);
        const std::uint32_t compression = get_u32(b, row + 24);
        if (names + name_at + name_size > data) {
            error = std::format("corrupt pack: entry {} name out of range", i);
            return false;
        }
        if (at < data || at > size || n > size - at) {
            error = std::format("corrupt pack: entry {} data out of range", i);
            return false;
        }
        const std::string_view path(reinterpret_cast<const char*>(b.data() + names + name_at),
                                    static_cast<std::size_t>(name_size));
        if (!valid_pack_path(path, &error)) {
            error = std::format("corrupt pack: entry {}: {}", i, error);
            return false;
        }
        if (compression != 0) {
            error = std::format("'{}': unknown compression {} (a newer engine wrote this pack)", path, compression);
            return false;
        }
        if (!entries_.empty() && !pack_path_less(entries_.back().path, path)) {
            error = std::format("corrupt pack: table not sorted at '{}'", path);
            return false;
        }
        entries_.push_back({path, b.subspan(static_cast<std::size_t>(at), static_cast<std::size_t>(n))});
    }
    return true;
}

std::optional<std::span<const std::byte>> Pack::find(std::string_view path) const {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), path,
                               [](const PackEntry& e, std::string_view p) { return pack_path_less(e.path, p); });
    if (it == entries_.end() || it->path != path) return std::nullopt;
    return it->data;
}

} // namespace cuervo::assets
