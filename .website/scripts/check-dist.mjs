// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: FM4's private-content
// patterns (scripts/private-content.mjs, shared with the docs plugin); rasters only as the
// future icon/Open Graph files; no release-button check, instead no link to a .dmg anywhere.
// Artifact hygiene: dist/ is what gets published. Fail on unexpected file types, source maps,
// third-party requests or content that suggests private material.
import { readFileSync, readdirSync, statSync } from 'node:fs'
import { basename, extname, join, relative, resolve } from 'node:path'
import { FORBIDDEN } from './private-content.mjs'

const dist = resolve(import.meta.dirname, '../dist')
const ALLOWED_EXT = new Set(['.html', '.css', '.js', '.svg', '.woff2', '.txt', '.xml', '.json', '.ico', '.webmanifest'])
const ALLOWED_NAMES = new Set(['CNAME', '.nojekyll'])
const TEXT_EXT = new Set(['.html', '.css', '.js', '.svg', '.txt', '.xml', '.json', '.webmanifest'])
const MAX_FILE_BYTES = 1_000_000
// Raster images: only the icon and Open Graph files to be made from the logo (README.md, TODO).
const RASTER_FILE = /^(?:favicon-32|icon-192|icon-512|apple-touch-icon|og)\.png$/
const RASTER_MAX_BYTES = 500_000

const EXTRA = [
  [/href="[^"]*\.dmg(?:[?#][^"]*)?"/i, 'link to a DMG (none is published)'],
]

// No runtime requests to other origins: scripts, stylesheets, fonts, preconnects.
const THIRD_PARTY = [
  [/<script\b[^>]*\ssrc="(?:https?:)?\/\//i, 'external script'],
  [/<link\b(?=[^>]*\srel="(?:stylesheet|preload|modulepreload|preconnect|dns-prefetch|icon)")[^>]*\shref="(?:https?:)?\/\/[^"]*"/i, 'external stylesheet/font/preconnect'],
  [/<img\b[^>]*\ssrc="(?:https?:)?\/\//i, 'external image'],
  [/@import\s+(?:url\()?["']?(?:https?:)?\/\//i, 'external CSS @import'],
  [/url\(\s*["']?(?:https?:)?\/\/(?!www\.w3\.org)/i, 'external url() in CSS'],
  [/fonts\.(?:googleapis|gstatic)\.com|use\.typekit\.net|fonts\.bunny\.net/i, 'web-font service'],
]

function walk(dir) {
  return readdirSync(dir).flatMap((name) => {
    const path = join(dir, name)
    return statSync(path).isDirectory() ? walk(path) : [path]
  })
}

const errors = []
const files = walk(dist)
let bytes = 0
for (const file of files) {
  const rel = relative(dist, file).split('\\').join('/')
  const name = basename(file)
  const ext = extname(file).toLowerCase()
  const size = statSync(file).size
  bytes += size
  if (ext === '.map') errors.push(`${rel}: source map (not published)`)
  else if (RASTER_FILE.test(rel)) {
    if (size > RASTER_MAX_BYTES) errors.push(`${rel}: ${size} bytes exceeds ${RASTER_MAX_BYTES}`)
  } else if (!ALLOWED_NAMES.has(name) && !ALLOWED_EXT.has(ext)) errors.push(`${rel}: file type not on the allowlist`)
  if (size > MAX_FILE_BYTES) errors.push(`${rel}: ${size} bytes is larger than expected`)
  if (!TEXT_EXT.has(ext)) continue
  const text = readFileSync(file, 'utf8')
  for (const [pattern, why] of THIRD_PARTY) {
    const m = pattern.exec(text)
    if (m) errors.push(`${rel}: ${why}: "${m[0].slice(0, 120)}"`)
  }
  for (const [pattern, why] of [...FORBIDDEN, ...EXTRA]) {
    const m = pattern.exec(text)
    if (m) errors.push(`${rel}: ${why}: "…${text.slice(Math.max(0, m.index - 30), m.index + m[0].length + 30).replace(/\s+/g, ' ')}…"`)
  }
}

console.log(`Scanned ${files.length} files in dist/ (${(bytes / 1024).toFixed(1)} KiB).`)
if (errors.length) {
  console.error(`\n${errors.length} problem(s):`)
  for (const e of errors) console.error(`  ✗ ${e}`)
  process.exit(1)
}
console.log('Artifact contains only allowed file types and no private-looking content.')
