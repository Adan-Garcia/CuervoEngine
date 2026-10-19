#pragma once

// `cuervo-tools docs`: the engine API reference (docs/API.md) generated from
// the script prelude (cuervo/script/prelude/*.plm, DESIGN.md 5.14 "docs").
//
// The prelude is the single source of truth for what scripts can call, so the
// reference is derived from it rather than written by hand: every top-level
// type, constant and function with its members (fields, signals, methods with
// full signatures and defaults, enum variants), described by the `//` comment
// lines right above a declaration (as the LSP's hover shows them) or a
// trailing `//` comment on a field's or variant's line. Members whose names
// start with `_` are private to their class (LANGUAGE.md 1.1) and left out.

#include <optional>
#include <string>
#include <vector>

#include "pluma/source.hpp"

namespace cuervo::tools {

// Markdown for `preludes` (as cuervo::script::engine_preludes() returns them),
// one section per part in the given order. Returns nullopt and sets `error`
// when a part doesn't parse.
std::optional<std::string> generate_api_docs(const std::vector<pluma::SourceFile::Part>& preludes,
                                             std::string& error);

} // namespace cuervo::tools
