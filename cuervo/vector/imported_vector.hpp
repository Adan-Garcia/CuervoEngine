#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cuervo/vector/document.hpp"
#include "cuervo/vector/mesh.hpp"

// ImportedVector: the engine's binary vector asset (DESIGN.md 4.11), written
// by the importer into .import/ so shipping builds never parse SVG.
//
// Layout (little-endian):
//   header  magic "EVEC", u32 format version, u32 flags, u32 reserved
//   body    document (size, gradients, clip paths, shapes; curves kept)
//           [flags & has_base_mesh] i32 level + the mesh pre-tessellated at that zoom level
//
// Curves are stored so the runtime can re-flatten at any zoom; the base mesh
// makes the common case (1x) load without tessellating. Every format change
// bumps `imported_vector_version`; the reader keeps loading older versions
// (DESIGN.md 4.12 migration rule) and rejects newer ones.
namespace cuervo::vector {

inline constexpr std::uint32_t imported_vector_version = 1;
inline constexpr char imported_vector_magic[4] = {'E', 'V', 'E', 'C'};

struct LodMesh {
    int level = 0;
    VectorMesh mesh;
};

struct ImportedVector {
    VectorDocument document;
    std::optional<LodMesh> base_mesh;
};

std::vector<std::byte> serialize_imported_vector(const ImportedVector& asset);
// Returns false (with `error`) on truncated, corrupt or newer-version data.
bool deserialize_imported_vector(std::span<const std::byte> data, ImportedVector& out, std::string* error = nullptr);

// True if `data` starts with the ImportedVector magic.
bool is_imported_vector(std::span<const std::byte> data);

// SVG text -> ImportedVector, with the base mesh tessellated at `base_level`.
std::optional<ImportedVector> import_svg(std::string_view svg_text, std::string_view name, int base_level = 0);

bool read_file(const std::string& path, std::vector<std::byte>& out);
bool write_file(const std::string& path, std::span<const std::byte> data);

// Loads an ImportedVector file, or (unless built with CUERVO_NO_SVG_IMPORT,
// meant for shipping builds) an SVG file, detected by content.
std::optional<ImportedVector> load_vector_file(const std::string& path);

} // namespace cuervo::vector
