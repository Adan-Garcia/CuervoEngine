# Brand assets -> C++ (DESIGN.md 4.16, milestone 8.1).
#
# assets/brand/ is synced from the design kit by tools/sync_brand.py and never
# edited. At configure time this generates, into <out_dir>:
#   cuervo/brand/brand_tokens.hpp  the palette and the light (Bone) / dark (Raven)
#                                  semantic themes of assets/brand/tokens.json
#   cuervo/brand/app_icon.inc      assets/brand/png/app-icon-512.png as bytes (the
#                                  window icon, so exported players need no file)
# Generated rather than checked in: tokens.json is the only copy in the repo, so
# nothing can drift from it, and CMake's string(JSON) needs no Python at build
# time. Editing tokens.json (i.e. re-running the sync) re-runs configure.

# The semantic colors every theme must define (tokens.css "Semantic tokens").
# A missing one fails the configure: the editor reads all of them.
set(CUERVO_BRAND_THEME_COLORS
    bg bg-subtle surface surface-raised surface-inverse
    text text-secondary text-muted text-inverse
    border border-strong focus
    accent accent-hover accent-subtle on-accent link
    success success-subtle info info-subtle warning warning-subtle danger danger-subtle)

# "#RRGGBB" / "#RRGGBBAA" -> "0xRRGGBBAA" in out_var.
function(_cuervo_brand_hex value out_var where)
    if(value MATCHES "^#([0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f])$")
        set(${out_var} "0x${CMAKE_MATCH_1}FF" PARENT_SCOPE)
    elseif(value MATCHES "^#([0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f])$")
        set(${out_var} "0x${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "assets/brand/tokens.json: ${where} is '${value}', not a #RRGGBB color")
    endif()
endfunction()

# A token's $value, following "{group.name.step}" references.
function(_cuervo_brand_resolve json value out_var where)
    set(depth 0)
    while(value MATCHES "^{(.+)}$")
        string(REPLACE "." ";" path "${CMAKE_MATCH_1}")
        string(JSON value ERROR_VARIABLE err GET "${json}" ${path} "$value")
        if(err)
            message(FATAL_ERROR "assets/brand/tokens.json: ${where} refers to a missing token: ${err}")
        endif()
        math(EXPR depth "${depth} + 1")
        if(depth GREATER 8)
            message(FATAL_ERROR "assets/brand/tokens.json: ${where}: reference cycle")
        endif()
    endwhile()
    set(${out_var} "${value}" PARENT_SCOPE)
endfunction()

function(cuervo_brand_generate out_dir)
    set(brand "${PROJECT_SOURCE_DIR}/assets/brand")
    set(tokens_file "${brand}/tokens.json")
    set(icon_file "${brand}/png/app-icon-512.png")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${tokens_file}" "${icon_file}")
    file(READ "${tokens_file}" json)

    # ---- palette: every color ramp in "color"
    set(palette "")
    string(JSON ngroups LENGTH "${json}" color)
    math(EXPR last_group "${ngroups} - 1")
    foreach(g RANGE ${last_group})
        string(JSON group MEMBER "${json}" color ${g})
        string(JSON nsteps LENGTH "${json}" color ${group})
        math(EXPR last_step "${nsteps} - 1")
        foreach(s RANGE ${last_step})
            string(JSON step MEMBER "${json}" color ${group} ${s})
            string(JSON value GET "${json}" color ${group} ${step} "$value")
            _cuervo_brand_resolve("${json}" "${value}" value "color.${group}.${step}")
            _cuervo_brand_hex("${value}" hex "color.${group}.${step}")
            string(APPEND palette "inline constexpr rhi::Color ${group}_${step} = rgba(${hex});\n")
        endforeach()
    endforeach()

    # ---- semantic themes
    set(fields "")
    set(names "")
    set(as_array "")
    foreach(name IN LISTS CUERVO_BRAND_THEME_COLORS)
        string(REPLACE "-" "_" id "${name}")
        string(APPEND fields "    rhi::Color ${id};\n")
        string(APPEND as_array "t.${id}, ")
        string(APPEND names "    \"${name}\",\n")
    endforeach()
    list(LENGTH CUERVO_BRAND_THEME_COLORS ncolors)
    foreach(theme light dark)
        set(init_${theme} "")
        set(list_${theme} "")
        foreach(name IN LISTS CUERVO_BRAND_THEME_COLORS)
            string(JSON value ERROR_VARIABLE err GET "${json}" semantic ${theme} ${name} "$value")
            if(err)
                message(FATAL_ERROR "assets/brand/tokens.json: the ${theme} theme has no '${name}' color "
                                    "(semantic.${theme}.${name}); every theme needs: ${CUERVO_BRAND_THEME_COLORS}")
            endif()
            _cuervo_brand_resolve("${json}" "${value}" value "semantic.${theme}.${name}")
            _cuervo_brand_hex("${value}" hex "semantic.${theme}.${name}")
            string(APPEND init_${theme} "    rgba(${hex}), // ${name}\n")
            string(APPEND list_${theme} "    ${hex}u,\n")
        endforeach()
    endforeach()

    # ---- type
    set(fonts "")
    foreach(role display body mono)
        string(JSON family ERROR_VARIABLE err GET "${json}" fontFamily ${role} "$value" 0)
        if(err)
            message(FATAL_ERROR "assets/brand/tokens.json: no fontFamily.${role}")
        endif()
        string(APPEND fonts "inline constexpr std::string_view font_${role} = \"${family}\";\n")
    endforeach()

    file(CONFIGURE OUTPUT "${out_dir}/cuervo/brand/brand_tokens.hpp" @ONLY CONTENT [=[
#pragma once
// GENERATED by cmake/Brand.cmake from assets/brand/tokens.json (the design kit's
// tokens, synced by tools/sync_brand.py). Do not edit; DESIGN.md 4.16 (8.1).
// Colors are sRGB, straight alpha (DESIGN.md 7.2 "Color"), as rhi::Color.

#include <array>
#include <cstdint>
#include <string_view>

#include "cuervo/rhi/rhi.hpp"

namespace cuervo::brand {

// 0xRRGGBBAA -> rhi::Color.
constexpr rhi::Color rgba(std::uint32_t v) {
    return {static_cast<float>((v >> 24) & 0xFFu) / 255.0f, static_cast<float>((v >> 16) & 0xFFu) / 255.0f,
            static_cast<float>((v >> 8) & 0xFFu) / 255.0f, static_cast<float>(v & 0xFFu) / 255.0f};
}

// The primitive palette ("color" in tokens.json).
namespace palette {
@palette@
// The two named neutrals of the kit.
inline constexpr rhi::Color bone = graphite_50;
inline constexpr rhi::Color raven = graphite_950;
} // namespace palette

// The semantic colors of one theme ("semantic.light" / "semantic.dark").
struct ThemeColors {
@fields@};

inline constexpr std::size_t theme_color_count = @ncolors@;
// Field names in declaration order, as tokens.json spells them.
inline constexpr std::array<std::string_view, theme_color_count> theme_color_names{
@names@};

// The fields in theme_color_names order.
constexpr std::array<rhi::Color, theme_color_count> as_array(const ThemeColors& t) {
    return {@as_array@};
}

// Light: Bone background. Dark: Raven background.
inline constexpr ThemeColors light{
@init_light@};
inline constexpr ThemeColors dark{
@init_dark@};

// The same values as 0xRRGGBBAA, in theme_color_names order (tests, SVG fills).
inline constexpr std::array<std::uint32_t, theme_color_count> light_rgba{
@list_light@};
inline constexpr std::array<std::uint32_t, theme_color_count> dark_rgba{
@list_dark@};

// Type families (first of each CSS stack): display, UI (body) and code.
@fonts@
} // namespace cuervo::brand
]=])

    # ---- the window icon, embedded
    file(READ "${icon_file}" icon HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," icon "${icon}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){24})" "\\1\n" icon "${icon}")
    file(CONFIGURE OUTPUT "${out_dir}/cuervo/brand/app_icon.inc" @ONLY CONTENT
         "// GENERATED by cmake/Brand.cmake from assets/brand/png/app-icon-512.png. Do not edit.\n@icon@\n")
endfunction()
