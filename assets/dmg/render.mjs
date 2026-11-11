// SPDX-License-Identifier: GPL-3.0-or-later
// Renders FM4 Recomp's release art for a packaging step (outputs are not committed). No npm packages: Node 22+ (global
// WebSocket), Google Chrome, and macOS sips/tiffutil/iconutil.
// Adapted from reFrozen assets/dmg/render.mjs (GPL-3.0-or-later): the DevTools-protocol renderer at an exact viewport
// and the label-contrast check. FM4 Recomp changes: the app icon (.icns) and the logo PNG modes, the FM4 pages.
//
//   node assets/dmg/render.mjs --out DIR [--mock]
//
// Writes into DIR:
//   dmg-background.png, dmg-background@2x.png, dmg-background.tiff   the DMG window (660 x 400 pt; give Finder the TIFF)
//   FM4Recomp.icns, icon-1024.png                                     the app icon (assets/icon/app-icon.html)
//   VolumeIcon.icns, volume-1024.png                                  the DMG's volume icon (assets/icon/volume-icon.html)
//   logo@2x.png                                                       the logo for the setup window (1280 x 780 px)
// With --mock also dmg-mockup-dark@2x.png and dmg-mockup-light@2x.png (icons and labels drawn as Finder would; review only).
// Exits with status 1 if a label plate is below 4.5:1 for white or black text (Finder's Dark and Light labels).
import { spawn, execFileSync } from 'node:child_process'
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, resolve } from 'node:path'
import { pathToFileURL } from 'node:url'

const args = process.argv.slice(2)
const opt = (name, fallback) => (args.includes(name) ? args[args.indexOf(name) + 1] : fallback)
const here = import.meta.dirname
const out = resolve(opt('--out', join(here, '..', '..', 'build', 'release-art')))
const mock = args.includes('--mock')
const chromePath = process.env.CHROME ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
const ICONS = [[185, 250], [475, 250]]
const PLATES = ICONS.map(([x]) => ({ x: x - 68, y: 316, w: 136, h: 32 }))

mkdirSync(out, { recursive: true })
const profile = mkdtempSync(join(tmpdir(), 'fm4-art-'))
const port = 9400 + Math.floor(Math.random() * 400)
const chrome = spawn(chromePath, ['--headless=new', `--remote-debugging-port=${port}`, '--hide-scrollbars', '--force-color-profile=srgb',
  '--allow-file-access-from-files', `--user-data-dir=${profile}`, 'about:blank'], { stdio: 'ignore' })
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

let ws
for (let i = 0; i < 75 && !ws; i++) {
  await sleep(200)
  try {
    const target = (await (await fetch(`http://127.0.0.1:${port}/json/list`)).json()).find((t) => t.type === 'page')
    if (target) ws = new WebSocket(target.webSocketDebuggerUrl)
  } catch {}
}
if (!ws) throw new Error(`Chrome did not start (${chromePath})`)
await new Promise((r) => ws.addEventListener('open', r))
let id = 0
const pending = new Map()
ws.addEventListener('message', (e) => {
  const m = JSON.parse(e.data)
  if (pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id) }
})
const send = (method, params = {}) => new Promise((resolveSend, reject) => {
  pending.set(++id, (m) => (m.error ? reject(new Error(`${method}: ${m.error.message}`)) : resolveSend(m.result)))
  ws.send(JSON.stringify({ id, method, params }))
})
const evaluate = async (expression) => (await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true })).result.value

async function shoot(page, query, width, height, scale, transparent = false) {
  await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: scale, mobile: false })
  await send('Emulation.setDefaultBackgroundColorOverride', transparent ? { color: { r: 0, g: 0, b: 0, a: 0 } } : {})
  await send('Page.navigate', { url: `${pathToFileURL(page).href}?${query}` })
  await sleep(1000)
  await evaluate('document.fonts.ready.then(() => Promise.all([...document.images].map((i) => i.decode().catch(() => {}))))')
  await sleep(300)
  return Buffer.from((await send('Page.captureScreenshot', { format: 'png', clip: { x: 0, y: 0, width, height, scale: 1 } })).data, 'base64')
}
const run = (cmd, a) => execFileSync(cmd, a, { stdio: 'ignore' })

await send('Page.enable')
// The app icon: 1024 px, then every .iconset size, then the .icns.
const iconPage = join(here, '..', 'icon', 'app-icon.html')
const icon = join(out, 'icon-1024.png')
writeFileSync(icon, await shoot(iconPage, '', 1024, 1024, 1, true))
const set = join(out, 'FM4Recomp.iconset')
mkdirSync(set, { recursive: true })
for (const s of [16, 32, 128, 256, 512]) {
  run('sips', ['-z', `${s}`, `${s}`, icon, '--out', join(set, `icon_${s}x${s}.png`)])
  run('sips', ['-z', `${2 * s}`, `${2 * s}`, icon, '--out', join(set, `icon_${s}x${s}@2x.png`)])
}
run('iconutil', ['-c', 'icns', set, '-o', join(out, 'FM4Recomp.icns')])
rmSync(set, { recursive: true, force: true })
// The DMG's volume icon: a drive enclosure with the app icon inset (assets/icon/volume-icon.html).
const volume = join(out, 'volume-1024.png')
writeFileSync(volume, await shoot(join(here, '..', 'icon', 'volume-icon.html'), `icon=${encodeURIComponent(pathToFileURL(icon).href)}`, 1024, 1024, 1, true))
const vset = join(out, 'VolumeIcon.iconset')
mkdirSync(vset, { recursive: true })
for (const s of [16, 32, 128, 256, 512]) {
  run('sips', ['-z', `${s}`, `${s}`, volume, '--out', join(vset, `icon_${s}x${s}.png`)])
  run('sips', ['-z', `${2 * s}`, `${2 * s}`, volume, '--out', join(vset, `icon_${s}x${s}@2x.png`)])
}
run('iconutil', ['-c', 'icns', vset, '-o', join(out, 'VolumeIcon.icns')])
rmSync(vset, { recursive: true, force: true })
// The logo for the setup window (2x of the 640 x 390 viewBox).
const logoPage = join(out, 'logo.html')
writeFileSync(logoPage, `<!doctype html><html><body style="margin:0;background:transparent"><img src="${pathToFileURL(join(here, '..', 'fm4-recomp-logo.svg')).href}" style="width:640px;height:390px;display:block"></body></html>`)
writeFileSync(join(out, 'logo@2x.png'), await shoot(logoPage, '', 640, 390, 2, true))
rmSync(logoPage)
// The DMG background.
const bgPage = join(here, 'dmg-background.html')
const one = await shoot(bgPage, '', 660, 400, 1)
writeFileSync(join(out, 'dmg-background.png'), one)
writeFileSync(join(out, 'dmg-background@2x.png'), await shoot(bgPage, '', 660, 400, 2))
run('sips', ['-s', 'dpiWidth', '144', '-s', 'dpiHeight', '144', join(out, 'dmg-background@2x.png')])
run('tiffutil', ['-cathidpicheck', join(out, 'dmg-background.png'), join(out, 'dmg-background@2x.png'), '-out', join(out, 'dmg-background.tiff')])
if (mock)
  for (const appearance of ['dark', 'light'])
    writeFileSync(join(out, `dmg-mockup-${appearance}@2x.png`),
      await shoot(bgPage, `mock=1&appearance=${appearance}&icon=${encodeURIComponent(pathToFileURL(icon).href)}`, 660, 400, 2))
// Label contrast, measured on the 1x render in the browser.
const measured = await evaluate(`(async () => {
  const img = new Image(); img.src = 'data:image/png;base64,${one.toString('base64')}'; await img.decode()
  const c = document.createElement('canvas'); c.width = 660; c.height = 400
  const x = c.getContext('2d'); x.drawImage(img, 0, 0)
  const lin = (v) => ((v /= 255) <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4)
  const lum = (d, i) => 0.2126 * lin(d[i]) + 0.7152 * lin(d[i + 1]) + 0.0722 * lin(d[i + 2])
  const stats = (rx, ry, rw, rh) => {
    const d = x.getImageData(rx, ry, rw, rh).data; let min = 1, max = 0
    for (let i = 0; i < d.length; i += 4) { const l = lum(d, i); min = Math.min(min, l); max = Math.max(max, l) }
    return { min, max }
  }
  return ${JSON.stringify(PLATES)}.map((p) => stats(p.x + 3, p.y + 3, p.w - 6, p.h - 6))
})()`)
ws.close()
await new Promise((r) => { chrome.once('exit', r); chrome.kill() })
rmSync(profile, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 })

const ratio = (a, b) => (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05)
let failed = false
measured.forEach((p, i) => {
  const white = ratio(1, p.max), black = ratio(0, p.min)
  if (white < 4.5 || black < 4.5) failed = true
  console.log(`label plate ${i + 1}: luminance ${p.min.toFixed(4)}-${p.max.toFixed(4)}; white text ${white.toFixed(2)}:1, black text ${black.toFixed(2)}:1`)
})
console.log(`wrote ${out}: dmg-background.{png,@2x.png,tiff}, FM4Recomp.icns, icon-1024.png, VolumeIcon.icns, volume-1024.png, logo@2x.png${mock ? ', mock-ups' : ''}`)
if (failed) { console.error('A label plate is below 4.5:1 for white or black text.'); process.exit(1) }
