# Chat font checks

Run `python tests/chat_fonts/run_tests.py` from the project root. It uses the already
available local TCC (no download), compiles the production `hosino_chat_display.c`
unchanged in a temporary directory, and exercises ordinary UTF-8, selectors, ZWJ,
skin/tag display annotations, invalid UTF-8 and every small output capacity.

It also decodes the actual generated LVGL bitmap tables: checks all supplemental
glyph bounds against the unchanged 18px line box, verifies nonempty glyphs for the
requested kaomoji/emoji samples, and renders those exact rasters to `glyph-preview.png`.
The preview uses 2x nearest-neighbour scaling for inspection. It does not claim LVGL
complex-script shaping or device visual acceptance. Results and source hashes are
in `validation.json`; no firmware build, device or network access occurs.
