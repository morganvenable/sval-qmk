// Regenerates keybard_vectors.txt from Keybard's own encoder (src/constants/point-controls.ts):
//   npx esbuild <keybard>/src/constants/point-controls.ts --format=esm --outfile=pc.mjs
//   node make_keybard_vectors.mjs   (prints the vectors, without the header comment)
// Each line: payload hex | expected SET status, then the four resolved slots.
import { encodePointControls, POINT_DISTANCES, effectiveSpeed } from './pc.mjs';
let seed = 0x5ba1b0a7;
const rnd = n => { seed = (seed + 0x6D2B79F5) >>> 0; let t = seed; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return (((t ^ (t >>> 14)) >>> 0) % n); }; // mulberry32
const speed = () => { const linked = !!rnd(2); const h = rnd(64); return { horizontal: h, vertical: linked ? h : rnd(64), linked }; };
const out = [];
for (let k = 0; k < 64; k++) {
  const profiles = Array.from({ length: 4 }, () => {
    const r = rnd(3); const interval = r === 0 ? 0 : r === 1 ? 4 + rnd(247) : [4, 250][rnd(2)];
    const sp = speed(); return { distance: POINT_DISTANCES[sp.horizontal], interval, reversal: 1 + rnd(64), speed: sp, useDefaults: !!rnd(2), action: rnd(8), invertHorizontal: !!rnd(2), invertVertical: !!rnd(2) };
  });
  const value = { version: 5, defaults: speed(), universal: !rnd(4), profiles };
  const bytes = encodePointControls(value);
  const slots = profiles.map((p, i) => { const s = effectiveSpeed(value, i); return [POINT_DISTANCES[s.horizontal], POINT_DISTANCES[s.vertical], p.interval, p.reversal, p.action, +p.invertHorizontal, +p.invertVertical].join(','); });
  out.push(bytes.map(b => b.toString(16)).join(' ') + '|0 ' + slots.join(' '));
}
console.log(out.join('\n'));
