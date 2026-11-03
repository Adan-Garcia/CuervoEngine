# Golden screenshots of the RmlUi editor

`*.qoi` here are crops of `cuervo-editor --rml-ui` screenshots (800x520, dark theme, UI scale 1.0, offscreen GL,
`examples/editor_level` copy). `tests/editor_golden.cmake` renders each case again and `golden_diff` compares the
crop pixel by pixel: a pixel matches when every channel is within 24 of the golden, and the test fails when more
than 1% of the crop does not match. That absorbs anti-aliasing and driver differences and fails on a moved row,
a missing icon or a changed colour token.

Cases: `shell` (whole window), `inspector` (selected node, right column), `tree_drag` (a drag over a row: ghost
and drop-into outline, posted with `--ui-events`), `welcome` (New project card), `modal` (Open scene dialog), `sprite` (an SVG on a Sprite2D: rasterized preview over the transparency checkerboard).

Regenerate after an intended visual change, then look at the result and commit it:

    CUERVO_UPDATE_GOLDENS=1 ctest --test-dir build/<name> -R editor_golden
    git diff --stat tests/golden

On a failure the actual screenshot and a diff (magenta = differing pixels) are left in
`<build>/tests/editor_golden/<case>.ppm` and `<case>.diff.ppm`. Add a case in `tests/editor_golden.cmake`
(`golden(name "x,y,w,h" <editor args>)`), keep crops small so the repository stays light.
