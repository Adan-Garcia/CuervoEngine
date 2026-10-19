# Brand assets (generated: do not edit)

The Cuervo Mark, wordmark, lockups, icons and design tokens, copied from the Cuervo
design kit by `tools/sync_brand.py` (DESIGN.md 4.16, milestone 8.1). The kit is the
source; `SOURCE.txt` records the kit commit these files came from and each file's
SHA-256. **That script is the only way the files here change**: edit the kit, then

```sh
python3 -I tools/sync_brand.py            # copy from the kit (default /mnt/storage/CuervoSpine/design)
python3 -I tools/sync_brand.py --check    # exit 1 if this folder differs from the kit
python3 -I tools/sync_brand.py --verify   # no kit needed: files match SOURCE.txt (ctest brand_assets)
```

and commit the result. Only the visual identity applies to the engine (mark, wordmark,
palette, type); the kit's product copy and flows are for another product.

| Path | Use in the engine |
|---|---|
| `tokens.json` | W3C design tokens. CMake generates `cuervo/brand/brand_tokens.hpp` (light and dark themes) from it at configure time (`cmake/Brand.cmake`) |
| `png/app-icon-512.png` | Embedded in the engine: window icon of `cuervo` and `cuervo-editor` |
| `logo/mark-ember.svg`, `logo/mark-ember-dark.svg`, `logo/wordmark.svg` | The editor's splash, drawn by the engine's SVG importer (light / dark) |
| `logo/mark-ember*.svg` | README logo (light / dark) |
| the rest | Kept for the docs site (8.4) and exports |

The fonts (Outfit, Figtree, JetBrains Mono, SIL OFL) are not here: the kit only has
`.woff2`, so CMake fetches the TTF releases (`cmake/Dependencies.cmake`).
