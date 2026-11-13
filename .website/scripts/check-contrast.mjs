// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: FM4's tokens and
// surfaces (src/styles/main.css).
// WCAG contrast audit on rendered colours: each text colour against the surfaces it is used on.
// Keep the values in step with the tokens in src/styles/main.css.

const hex = (h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16))
const lin = (c) => ((c /= 255) <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4)
const lum = ([r, g, b]) => 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)
const ratio = (a, b) => {
  const [x, y] = [lum(a), lum(b)].sort((p, q) => q - p)
  return (x + 0.05) / (y + 0.05)
}
const over = (top, alpha, bottom) => top.map((c, i) => c * alpha + bottom[i] * (1 - alpha))

const text = {
  chalk: hex('#f1f2f4'), muted: hex('#a9b0bb'), link: hex('#ff9a87'), linkHover: hex('#ffc4b8'),
  code: hex('#e6d7b0'), white: hex('#ffffff'),
}
const ui = { signal: hex('#ff4429'), seam: hex('#2c313a') }
const asphalt = hex('#0e1013')
const tarmac = hex('#171a1f')
const tarmac2 = hex('#1e2228')
const footer = hex('#0a0b0d')
// The hero's lightest point: a 2.5% white wash over the page.
const heroTop = over([255, 255, 255], 0.025, asphalt)
// The sticky header: 94% asphalt over the lightest content it can scroll over (a hovered card).
const header = over(asphalt, 0.94, tarmac2)

const checks = [
  ['body text (chalk) on page', text.chalk, asphalt, 4.5],
  ['body text (chalk) on hero top', text.chalk, heroTop, 4.5],
  ['muted text on page', text.muted, asphalt, 4.5],
  ['muted text on hero top', text.muted, heroTop, 4.5],
  ['muted text on panel (card, callout, disclaimer)', text.muted, tarmac, 4.5],
  ['muted text on hovered card', text.muted, tarmac2, 4.5],
  ['muted text on footer', text.muted, footer, 4.5],
  ['muted nav link on header', text.muted, header, 4.5],
  ['chalk text on panel', text.chalk, tarmac, 4.5],
  ['chalk text on hovered card / button', text.chalk, tarmac2, 4.5],
  ['strong (white) on page', text.white, asphalt, 4.5],
  ['link on page', text.link, asphalt, 4.5],
  ['link on panel', text.link, tarmac, 4.5],
  ['link on footer', text.link, footer, 4.5],
  ['link on table header / hovered panel', text.link, tarmac2, 4.5],
  ['link (hover) on panel', text.linkHover, tarmac, 4.5],
  ['inline code on code background', text.code, tarmac2, 4.5],
  ['code block text on block', text.code, footer, 4.5],
  // Non-text UI colours (WCAG 1.4.11: 3:1 against the adjacent surface).
  ['UI: focus ring (signal) on page', ui.signal, asphalt, 3],
  ['UI: focus ring (signal) on panel', ui.signal, tarmac2, 3],
  ['UI: accent rule (signal) on panel', ui.signal, tarmac, 3],
]

let failed = 0
console.log('WCAG contrast (minimum required in brackets):')
for (const [name, fg, bg, min] of checks) {
  const r = ratio(fg, bg)
  if (r < min) failed++
  console.log(`  ${r >= min ? '✓' : '✗'} ${r.toFixed(2).padStart(5)}:1 [${min}]  ${name}`)
}
if (failed) {
  console.error(`\n${failed} contrast check(s) below WCAG AA.`)
  process.exit(1)
}
console.log('All measured text meets WCAG AA.')
