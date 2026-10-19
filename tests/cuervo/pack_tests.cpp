// The release archive and the VFS (cuervo/assets, docs/EXPORT.md): format
// round trip, byte determinism, corruption detection, path rules, mount
// resolution order and the export manifest.

#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cuervo/assets/manifest.hpp"
#include "cuervo/assets/pack.hpp"
#include "cuervo/assets/vfs.hpp"

using namespace cuervo;
namespace fs = std::filesystem;

namespace {

std::vector<std::byte> bytes_of(std::string_view s) {
    const auto* b = reinterpret_cast<const std::byte*>(s.data());
    return {b, b + s.size()};
}

std::string text_of(std::span<const std::byte> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

fs::path temp_dir(std::string_view name) {
    fs::path d = fs::temp_directory_path() / "cuervo_pack_tests" / std::string(name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_text(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::vector<std::byte> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<char> c((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(c.size());
    if (!c.empty()) std::memcpy(out.data(), c.data(), c.size());
    return out;
}

// A small pack with nested paths, an empty file and binary data.
assets::PackBuilder sample_builder(bool reversed = false) {
    std::vector<std::pair<std::string, std::vector<std::byte>>> files = {
        {"project.toml", bytes_of("name = \"T\"\n")},
        {"scripts/a.plm", bytes_of("class A {}\n")},
        {"scripts/sub/b.plm", bytes_of("class B {}\n")},
        {"assets/empty.txt", {}},
        {"assets/bin.dat", {std::byte{0}, std::byte{255}, std::byte{7}}},
        {"a-b", bytes_of("dash")},
    };
    if (reversed) std::reverse(files.begin(), files.end());
    assets::PackBuilder b(assets::PackPlatform::Linux);
    std::string error;
    for (auto& [path, data] : files) REQUIRE_MESSAGE(b.add(path, data, error), error);
    return b;
}

} // namespace

TEST_CASE("pack: round trip through bytes and through a file") {
    const std::vector<std::byte> bytes = sample_builder().build();
    std::string error;
    auto pack = assets::Pack::from_bytes(bytes, error);
    REQUIRE_MESSAGE(pack, error);
    CHECK(pack->version() == assets::pack_version);
    CHECK(pack->platform() == assets::PackPlatform::Linux);
    CHECK(pack->entries().size() == 6);
    CHECK(text_of(*pack->find("project.toml")) == "name = \"T\"\n");
    CHECK(text_of(*pack->find("scripts/sub/b.plm")) == "class B {}\n");
    CHECK(pack->find("assets/empty.txt")->empty());
    CHECK(pack->find("assets/bin.dat")->size() == 3);
    CHECK((*pack->find("assets/bin.dat"))[1] == std::byte{255});
    // Sorted bytewise; data 16-byte aligned for typed views.
    for (std::size_t i = 1; i < pack->entries().size(); ++i) {
        CHECK(pack->entries()[i - 1].path < pack->entries()[i].path);
    }
    for (const assets::PackEntry& e : pack->entries()) {
        CHECK((e.data.data() - pack->entries().front().data.data()) % assets::pack_alignment == 0);
    }

    const fs::path dir = temp_dir("roundtrip");
    REQUIRE_MESSAGE(sample_builder().write(dir / "t.pak", error), error);
    auto mapped = assets::Pack::open(dir / "t.pak", error);
    REQUIRE_MESSAGE(mapped, error);
    CHECK(mapped->content_hash() == pack->content_hash());
    CHECK(text_of(*mapped->find("scripts/a.plm")) == "class A {}\n");
    CHECK(!fs::exists(dir / "t.pak.tmp"));
}

TEST_CASE("pack: missing entries are reported, not invented") {
    std::string error;
    auto pack = assets::Pack::from_bytes(sample_builder().build(), error);
    REQUIRE(pack);
    CHECK_FALSE(pack->find("scripts/c.plm"));
    CHECK_FALSE(pack->find("scripts"));       // a directory is not an entry
    CHECK_FALSE(pack->find("Project.toml"));  // exact, case-sensitive
    CHECK_FALSE(pack->find(""));

    auto missing = assets::Pack::open(temp_dir("missing") / "nope.pak", error);
    CHECK_FALSE(missing);
    CHECK(error.find("can't open pack") != std::string::npos);
}

TEST_CASE("pack: identical inputs give byte-identical packs") {
    const std::vector<std::byte> a = sample_builder(false).build();
    const std::vector<std::byte> b = sample_builder(true).build(); // insertion order doesn't matter
    CHECK(a == b);
    const fs::path dir = temp_dir("determinism");
    std::string error;
    REQUIRE(sample_builder().write(dir / "1.pak", error));
    REQUIRE(sample_builder(true).write(dir / "2.pak", error));
    CHECK(read_file(dir / "1.pak") == read_file(dir / "2.pak"));
    CHECK(read_file(dir / "1.pak") == a);
    // A different platform tag or content is a different pack.
    assets::PackBuilder web(assets::PackPlatform::Web);
    REQUIRE(web.add("project.toml", bytes_of("name = \"T\"\n"), error));
    CHECK(web.build() != a);
}

TEST_CASE("pack: corruption is detected by the content hash and the header checks") {
    const std::vector<std::byte> good = sample_builder().build();
    std::string error;

    // A flipped byte in the data region.
    std::vector<std::byte> bad = good;
    bad[bad.size() - 20] ^= std::byte{0x40};
    CHECK_FALSE(assets::Pack::from_bytes(bad, error));
    CHECK(error.find("content hash") != std::string::npos);
    // ...still openable for inspection without verification.
    CHECK(assets::Pack::from_bytes(bad, error, {.verify = false}));

    // A flipped byte in a file on disk.
    const fs::path dir = temp_dir("corrupt");
    std::vector<std::byte> on_disk = good;
    on_disk[assets::pack_header_size + 3] ^= std::byte{1}; // the table
    std::ofstream(dir / "bad.pak", std::ios::binary)
        .write(reinterpret_cast<const char*>(on_disk.data()), static_cast<std::streamsize>(on_disk.size()));
    CHECK_FALSE(assets::Pack::open(dir / "bad.pak", error));
    CHECK(error.find("bad.pak") != std::string::npos);
    CHECK(error.find("corrupt") != std::string::npos);

    // Truncated, bad magic, newer version.
    std::vector<std::byte> cut(good.begin(), good.end() - 16);
    CHECK_FALSE(assets::Pack::from_bytes(cut, error));
    CHECK(error.find("truncated") != std::string::npos);
    std::vector<std::byte> magic = good;
    magic[0] = std::byte{'X'};
    CHECK_FALSE(assets::Pack::from_bytes(magic, error));
    CHECK(error.find("magic") != std::string::npos);
    std::vector<std::byte> newer = good;
    newer[4] = std::byte{99};
    CHECK_FALSE(assets::Pack::from_bytes(newer, error));
    CHECK(error.find("version 99") != std::string::npos);
    CHECK_FALSE(assets::Pack::from_bytes({}, error));
}

TEST_CASE("pack: path rules") {
    std::string why;
    CHECK(assets::valid_pack_path("scripts/game.plm"));
    CHECK(assets::valid_pack_path(".cuervo/export.toml"));
    for (const char* bad : {"", "/abs", "a//b", "a/", "../x", "a/./b", "a\\b", "C:/x"}) {
        CHECK_MESSAGE(!assets::valid_pack_path(bad, &why), bad);
    }
    assets::PackBuilder b;
    std::string error;
    CHECK(b.add("x", {}, error));
    CHECK_FALSE(b.add("x", {}, error));
    CHECK(error.find("already") != std::string::npos);
    CHECK_FALSE(b.add("../escape", {}, error));
}

TEST_CASE("vfs: normalize") {
    std::string out;
    std::string error;
    CHECK(assets::Vfs::normalize("./scenes//a.scene", out, error));
    CHECK(out == "scenes/a.scene");
    CHECK(assets::Vfs::normalize("scenes\\b.scene", out, error));
    CHECK(out == "scenes/b.scene");
    CHECK_FALSE(assets::Vfs::normalize("../x", out, error));
    CHECK_FALSE(assets::Vfs::normalize("a/../../x", out, error));
    CHECK_FALSE(assets::Vfs::normalize("/etc/passwd", out, error));
    CHECK_FALSE(assets::Vfs::normalize("", out, error));
    CHECK_FALSE(assets::Vfs::normalize(".", out, error));
}

TEST_CASE("vfs: mounts resolve in order, lists merge, hidden files stay hidden") {
    const fs::path dir = temp_dir("vfs");
    write_text(dir / "first/project.toml", "first");
    write_text(dir / "first/scripts/z.plm", "z");
    write_text(dir / "first/.import/cache.bin", "hidden");
    write_text(dir / "second/project.toml", "second");
    write_text(dir / "second/scripts/a.plm", "a");
    assets::PackBuilder b;
    std::string error;
    REQUIRE(b.add("project.toml", bytes_of("pack"), error));
    REQUIRE(b.add("scripts/m.plm", bytes_of("m"), error));
    REQUIRE(b.add("scripts/a.plm", bytes_of("pack a"), error));
    REQUIRE(b.add(".cuervo/export.toml", bytes_of("x"), error));
    auto pack = assets::Pack::from_bytes(b.build(), error);
    REQUIRE(pack);

    assets::Vfs vfs;
    vfs.mount_dir(dir / "first");
    vfs.mount_pack(pack);
    vfs.mount_dir(dir / "second");
    std::string text;
    REQUIRE(vfs.read_text("project.toml", text, error));
    CHECK(text == "first");                 // first mount wins
    REQUIRE(vfs.read_text("scripts/a.plm", text, error));
    CHECK(text == "pack a");                // the pack is before `second`
    REQUIRE(vfs.read_text("./scripts/m.plm", text, error));
    CHECK(text == "m");
    CHECK(vfs.list("scripts", ".plm") == std::vector<std::string>{"scripts/a.plm", "scripts/m.plm", "scripts/z.plm"});
    CHECK(vfs.list("") == std::vector<std::string>{"project.toml", "scripts/a.plm", "scripts/m.plm", "scripts/z.plm"});
    CHECK(vfs.exists(".cuervo/export.toml")); // readable, never listed
    CHECK(vfs.real_path("scripts/z.plm") == dir / "first/scripts/z.plm");
    CHECK_FALSE(vfs.real_path("scripts/m.plm")); // a pack entry has no real file
    CHECK(vfs.packed());

    // Missing files name what was searched.
    CHECK_FALSE(vfs.open("scripts/none.plm", error));
    CHECK(error.find("scripts/none.plm") != std::string::npos);
    CHECK(error.find("not found") != std::string::npos);
    CHECK_FALSE(vfs.open("../outside", error));
    CHECK(error.find("leave") != std::string::npos);
}

TEST_CASE("vfs: listing order is path-component order in dev and release alike") {
    // "a/b" sorts before "a-b" component-wise (std::filesystem order), not bytewise.
    CHECK(assets::path_component_less("a/b", "a-b"));
    CHECK_FALSE(assets::path_component_less("a-b", "a/b"));
    CHECK(assets::path_component_less("a", "a/b"));
    const fs::path dir = temp_dir("order");
    write_text(dir / "s/a/b.plm", "1");
    write_text(dir / "s/a-b.plm", "2");
    write_text(dir / "s/a.plm", "3");
    auto dev = assets::Vfs::dev(dir);
    assets::PackBuilder b;
    std::string error;
    for (const char* p : {"s/a/b.plm", "s/a-b.plm", "s/a.plm"}) REQUIRE(b.add(p, bytes_of("x"), error));
    auto rel = assets::Vfs::release(assets::Pack::from_bytes(b.build(), error), {});
    CHECK(dev->list("s") == rel->list("s"));
    CHECK(dev->list("s").front() == "s/a/b.plm");
}

TEST_CASE("export manifest: round trip and version checks") {
    assets::ExportManifest m;
    m.game = "Shmup \"Deluxe\"";
    m.engine = "0.1.0";
    m.platform = assets::PackPlatform::Web;
    m.scripts = assets::ScriptForm::Source;
    m.stripped = true;
    const std::string text = assets::write_manifest(m);
    CHECK(assets::write_manifest(m) == text); // stable
    std::string error;
    auto back = assets::parse_manifest(text, error);
    REQUIRE_MESSAGE(back, error);
    CHECK(back->game == m.game);
    CHECK(back->platform == assets::PackPlatform::Web);
    CHECK(back->scripts == assets::ScriptForm::Source);
    CHECK(back->stripped);
    CHECK_FALSE(assets::parse_manifest("format = 99\n", error));
    CHECK(error.find("newer") != std::string::npos);
    CHECK_FALSE(assets::parse_manifest("scripts = \"magic\"\nformat = 1\n", error));
}
