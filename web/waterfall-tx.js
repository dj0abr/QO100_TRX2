const bands = 9;
const columns = 7;
const binsPerBand = 9;
const tonesPerBand = 7;

export function waterfallRaster(text, font) {
  const message = text.replace(/^ +| +$/g, '');
  if (!message.length || /[^a-z0-9 /?.+_-]/i.test(message)) return [];
  const rows = [];
  for (const letter of message.toUpperCase()) {
    if (rows.length) rows.push(0);
    const glyph = font[letter];
    for (let column = columns - 1; column >= 0; column--) {
      let mask = 0;
      for (let band = 0; band < glyph.length; band++) {
        if ((glyph[band] >> column) & 1) mask |= 1 << band;
      }
      rows.push(mask);
    }
  }
  return rows;
}

export function drawWaterfallPreview(canvas, raster, rowMs) {
  // About two display pixels per 50 ms at the default 20 spectrum frames/s.
  const rowHeight = rowMs / 25;
  canvas.height = Math.max(40, Math.round(raster.length * rowHeight));
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#121b2a';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = '#81ddff';
  const toneWidth = canvas.width / (bands * binsPerBand);
  for (const [row, mask] of raster.entries()) {
    const y = Math.round((raster.length - 1 - row) * rowHeight);
    const height = Math.round((raster.length - row) * rowHeight) - y;
    for (let band = 0; band < bands; band++) {
      if (mask & (1 << band)) ctx.fillRect(toneWidth * (binsPerBand * band + 1), y, tonesPerBand * toneWidth, height);
    }
  }
}
