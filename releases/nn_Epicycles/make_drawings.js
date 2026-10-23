// Builds drawings.h, the card's built-in drawings, from the web app.
//
//   node make_drawings.js
//
// Runs the <script id="fourier"> block of web/index.html, the same code the
// page uses to turn a drawing into circles, so the card and the page agree
// exactly.  Run it again after changing a built-in drawing there.

'use strict';
const fs = require('fs');
const path = require('path');

const html = fs.readFileSync(path.join(__dirname, 'web', 'index.html'), 'utf8');
const m = html.match(/^<script id="fourier">$([\s\S]*?)^<\/script>$/m);
if (!m) throw new Error('No <script id="fourier"> in web/index.html');
const FOURIER = new Function(m[1] + '\nreturn FOURIER;')();

const lines = [];
lines.push('// The card\'s built-in drawings: made by make_drawings.js from the');
lines.push('// <script id="fourier"> block of web/index.html.  Don\'t edit by hand.');
lines.push('//');
lines.push('// Each circle turns k times per turn of the drawing (below 0, clockwise),');
lines.push('// with radius amp (16384 = 1) and start phase (16384 = one turn).  Biggest');
lines.push('// first.');
lines.push('');
lines.push('#ifndef EPICYCLES_DRAWINGS_H');
lines.push('#define EPICYCLES_DRAWINGS_H');
lines.push('');
lines.push('#include <stdint.h>');
lines.push('');
lines.push('namespace drawings');
lines.push('{');
lines.push('');
lines.push(`static constexpr int kMaxCircles = ${FOURIER.MAX_CIRCLES};`);
lines.push(`static constexpr int kNumDrawings = ${FOURIER.BUILTIN.length};`);
lines.push('');
lines.push('struct Circle');
lines.push('{');
lines.push('\tint8_t k;');
lines.push('\tuint16_t amp;');
lines.push('\tuint16_t phase;');
lines.push('};');
lines.push('');
lines.push('struct Drawing');
lines.push('{');
lines.push('\tint count;');
lines.push('\tCircle circles[kMaxCircles];');
lines.push('};');
lines.push('');
lines.push('static const Drawing kDrawings[kNumDrawings] = {');
FOURIER.BUILTIN.forEach((_, i) => {
  const d = FOURIER.builtin(i);
  lines.push(`\t// ${d.name}: ${d.circles.length} circle${d.circles.length === 1 ? '' : 's'}`);
  lines.push(`\t{${d.circles.length}, {`);
  for (let j = 0; j < d.circles.length; j += 4) {
    lines.push('\t\t' + d.circles.slice(j, j + 4).map(c => `{${c.k}, ${c.amp}, ${c.phase}}`).join(', ') + ',');
  }
  lines.push('\t}},');
});
lines.push('};');
lines.push('');
lines.push('} // namespace drawings');
lines.push('');
lines.push('#endif');

fs.writeFileSync(path.join(__dirname, 'drawings.h'), lines.join('\n') + '\n');
console.log('Wrote drawings.h: ' + FOURIER.BUILTIN.map((_, i) => FOURIER.BUILTIN[i].name).join(', '));
