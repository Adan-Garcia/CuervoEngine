#include <doctest.h>

#include <algorithm>

#include "cuervo/script/script_host.hpp"
#include "pluma/format.hpp"
#include "pluma/lsp.hpp"

using namespace cuervo;
using namespace pluma::lsp;

namespace {

bool has_item(const std::vector<CompletionItem>& items, std::string_view label) {
    return std::any_of(items.begin(), items.end(),
                       [&](const CompletionItem& item) { return item.label == label; });
}

} // namespace

TEST_CASE("cuervo lsp: engine prelude completions, hover, and shmup workspace analysis") {
    Server server(script::default_lsp_options());

    std::string uri = "file:///game/ship.plm";
    server.open_document(uri,
                         "class Ship extends Node {\n"
                         "    fn update(dt: float) {\n"
                         "        if Input.held(#shoot) {\n"
                         "            position += vec2(0.0, -100.0) * dt\n"
                         "        }\n"
                         "    }\n"
                         "    fn draw(g: Canvas) {\n"
                         "        g.\n"
                         "    }\n"
                         "}\n");

    // `g.` inside `draw(g: Canvas)` completes `Canvas` methods from the engine prelude.
    auto canvas_items = server.complete(uri, Position{7, 10});
    CHECK(has_item(canvas_items, "circle"));
    CHECK(has_item(canvas_items, "rect"));
    CHECK(has_item(canvas_items, "line"));
    CHECK(has_item(canvas_items, "text"));

    // Hover on `Input.held` at line 2, col 18 shows the engine prelude signature.
    auto hov = server.hover(uri, Position{2, 18});
    REQUIRE(hov.has_value());
    CHECK(hov->contents.find("fn held(action: name) -> bool") != std::string::npos);

#if CUERVO_FEATURE_AUDIO && CUERVO_FEATURE_UI && CUERVO_FEATURE_PARTICLES && CUERVO_FEATURE_COLLISION
    // Loading the shipped `examples/shmup` workspace produces zero errors. The
    // shmup needs every optional prelude, so stripped builds skip it (DESIGN.md 6, 7.5).
    Server shmup_server(script::default_lsp_options(std::string(CUERVO_EXAMPLES_DIR) + "/shmup", false));
    auto all_diags = shmup_server.all_diagnostics();
    CHECK_FALSE(all_diags.empty());
    for (const auto& [file_uri, diags] : all_diags) {
        for (const auto& d : diags) {
            CHECK_MESSAGE(d.severity != pluma::Severity::Error, file_uri << ": " << d.message);
        }
    }
#endif
}
