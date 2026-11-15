// Project wrapper around the pinned lv_font_conv 1.5.3 public conversion modules.
// Constrain rare tall glyphs to the existing 18px chat line; no SDK/node_modules edits.
'use strict';
const fs = require('fs');
const path = require('path');
const job = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const collect = require(path.join(job.converter_root, 'lib/collect_font_data'));
const write = require(path.join(job.converter_root, 'lib/writers/lvgl'));
const options = {
  font: job.fonts.filter(f => f.codepoints.length).map(f => ({
    source_path: f.source_path, source_bin: fs.readFileSync(f.source_path),
    ranges: [{symbols: f.codepoints.map(cp => String.fromCodePoint(cp)).join('')}],
    autohint_off: true, autohint_strong: false,
  })),
  size: 16, bpp: 2, no_compress: true, no_prefilter: true, no_kerning: true,
  lcd: false, lcd_v: false, use_color_info: false, format: 'lvgl',
  output: job.output, lv_include: 'lvgl.h', lv_font_name: 'hosino_chat_symbols_16',
  lv_fallback: 'lv_font_montserrat_16', opts_string: 'Pinned project 16px 2bpp fallback; see symbols-manifest.json',
};
(async () => {
  const data = await collect(options);
  const changed = [], hidden = [];
  for (const g of data.glyphs) {
    // Selectors/tags encode presentation or sequences, never visible square placeholders.
    if (g.code === 0x200D || g.code === 0xFE0E || g.code === 0xFE0F || (g.code >= 0xE0020 && g.code <= 0xE007F)) {
      g.pixels = []; g.bbox = {x: 0, y: 0, width: 0, height: 0}; g.advanceWidth = 0;
      hidden.push(g.code); continue;
    }
    const before = {...g.bbox};
    // Area-average only rare oversized rasters; retain all strokes instead of clipping.
    if (g.bbox.height > 18) {
      const oldH = g.bbox.height, oldW = g.bbox.width;
      const scale = 18 / oldH, newW = Math.max(1, Math.round(oldW * scale));
      const pixels = [];
      for (let y = 0; y < 18; ++y) {
        const row = [];
        for (let x = 0; x < newW; ++x) {
          const left = x * oldW / newW, right = (x + 1) * oldW / newW;
          const top = y * oldH / 18, bottom = (y + 1) * oldH / 18;
          let sum = 0, area = 0;
          for (let sy = Math.floor(top); sy < Math.ceil(bottom); ++sy) {
            for (let sx = Math.floor(left); sx < Math.ceil(right); ++sx) {
              const a = (Math.min(right, sx + 1) - Math.max(left, sx)) *
                        (Math.min(bottom, sy + 1) - Math.max(top, sy));
              sum += g.pixels[sy][sx] * a; area += a;
            }
          }
          row.push(Math.round(sum / area));
        }
        pixels.push(row);
      }
      g.pixels = pixels; g.bbox.width = newW; g.bbox.height = 18;
      g.bbox.x = Math.round(g.bbox.x * scale); g.advanceWidth *= scale;
    }
    g.bbox.y = Math.max(-3, Math.min(g.bbox.y, 15 - g.bbox.height));
    if (JSON.stringify(before) !== JSON.stringify(g.bbox)) changed.push({codepoint: g.code, before, after: g.bbox});
    if (g.bbox.y < -3 || g.bbox.y + g.bbox.height > 15) throw Error('Raster exceeds chat line');
  }
  data.ascent = 15; data.descent = -3;
  for (const [filename, content] of Object.entries(write(options, data))) fs.writeFileSync(filename, content);
  fs.writeFileSync(job.report, JSON.stringify({line_height: 18, baseline: 3,
    adjusted_glyph_count: changed.length, adjusted_glyphs: changed,
    zero_width_sequence_controls: hidden}, null, 2));
})().catch(e => {console.error(e.stack); process.exit(1);});
