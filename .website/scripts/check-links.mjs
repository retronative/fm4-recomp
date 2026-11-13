// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: the Markdown-level link
// check is gone (plugins/docs.mjs resolves every relative link at build time and fails on one
// without a public page); dist/ is checked as before.
// Crawls dist/ for internal links and anchors.
// Fails (exit 1) on any broken internal link or anchor. External links are listed, not fetched.
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs'
import { join, relative, resolve } from 'node:path'
import { base, siteUrl } from './site-config.mjs'

const root = resolve(import.meta.dirname, '..')
const dist = join(root, 'dist')
const errors = []
const external = new Set()

if (!existsSync(dist)) {
  console.error('dist/ not found: run `npm run build` first.')
  process.exit(1)
}

function walk(dir) {
  return readdirSync(dir).flatMap((name) => {
    const path = join(dir, name)
    return statSync(path).isDirectory() ? walk(path) : [path]
  })
}

const decode = (s) => s.replace(/&amp;/g, '&').replace(/&quot;/g, '"').replace(/&#39;/g, "'")
const idCache = new Map()
function idsIn(file) {
  if (!idCache.has(file)) {
    const html = readFileSync(file, 'utf8')
    idCache.set(file, new Set([...html.matchAll(/\sid="([^"]+)"/g)].map((m) => decode(m[1]))))
  }
  return idCache.get(file)
}

// Map a site URL path (including base) to the file that serves it.
function fileFor(pathname) {
  if (!pathname.startsWith(base)) return null
  const rel = decodeURIComponent(pathname.slice(base.length))
  const candidates = rel === '' || rel.endsWith('/') ? [join(dist, rel, 'index.html')] : [join(dist, rel), join(dist, rel, 'index.html')]
  return candidates.find((c) => existsSync(c) && statSync(c).isFile()) ?? null
}

const htmlFiles = walk(dist).filter((f) => f.endsWith('.html'))
let checked = 0

for (const file of htmlFiles) {
  const html = readFileSync(file, 'utf8')
  const where = relative(dist, file)
  const ids = [...html.matchAll(/\sid="([^"]+)"/g)].map((m) => m[1])
  for (const id of new Set(ids.filter((id, i) => ids.indexOf(id) !== i))) errors.push(`${where}: duplicate id="${id}"`)
  const pagePath = base + relative(dist, file).replace(/index\.html$/, '').replace(/\\/g, '/')
  for (const [, attr, raw] of html.matchAll(/\s(href|src|content)="([^"]*)"/g)) {
    if (attr === 'content' && !raw.startsWith(siteUrl + base)) continue
    let url = decode(raw)
    // Absolute URLs on the canonical origin (canonical, og:url, og:image) must resolve in dist.
    if (url.startsWith(siteUrl + base)) url = url.slice(siteUrl.length)
    if (/^(https?:)?\/\//.test(url)) {
      external.add(url)
      continue
    }
    if (/^(mailto|tel|data):/.test(url)) continue
    if (url === '') {
      errors.push(`${where}: empty ${attr}`)
      continue
    }
    checked++
    const resolved = new URL(url, `http://site${pagePath}`)
    const target = fileFor(resolved.pathname)
    if (!target) {
      errors.push(`${where}: ${attr}="${url}" → no file for ${resolved.pathname}`)
      continue
    }
    const anchor = decodeURIComponent(resolved.hash.slice(1))
    if (anchor && target.endsWith('.html') && !idsIn(target).has(anchor)) {
      errors.push(`${where}: ${attr}="${url}" → missing anchor #${anchor} in ${relative(dist, target)}`)
    }
  }
}

// Built JS: lazy chunks and preload lists must point at files that exist.
const assetsDir = join(dist, 'assets')
let chunkRefs = 0
for (const file of walk(assetsDir).filter((f) => f.endsWith('.js'))) {
  const js = readFileSync(file, 'utf8')
  for (const [, ref] of js.matchAll(/["'`]\.\/([\w.-]+\.(?:js|css))["'`]/g)) {
    chunkRefs++
    if (!existsSync(join(assetsDir, ref))) errors.push(`assets/${relative(assetsDir, file)}: chunk ./${ref} missing`)
  }
  for (const [, ref] of js.matchAll(/["'`](assets\/[\w.-]+\.(?:js|css))["'`]/g)) {
    chunkRefs++
    if (!existsSync(join(dist, ref))) errors.push(`assets/${relative(assetsDir, file)}: ${ref} missing`)
  }
}

console.log(`Base ${base}: checked ${checked} internal links in ${htmlFiles.length} HTML files and ${chunkRefs} chunk references.`)
if (external.size) {
  console.log(`External links (listed, not fetched): ${external.size}`)
  for (const url of [...external].sort()) console.log(`  ${url}`)
}
if (errors.length) {
  console.error(`\n${errors.length} broken link(s):`)
  for (const e of errors) console.error(`  ✗ ${e}`)
  process.exit(1)
}
console.log('All internal links and anchors resolve.')
