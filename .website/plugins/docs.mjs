// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: reads FM4's public
// Markdown (docs/*.md with optional front matter, the allowlisted reference/<area>/*.md notes,
// reference/README.md, THIRD_PARTY_NOTICES.md and selected README.md sections); docs/status.md's
// front matter gives the release headline and, once a release is published, the release page the
// download button links (never a file); no milestone or list parsing; relative links resolved
// against each file's repository path (fail-closed); GitHub alert blockquotes rendered as
// callouts; typographer off; the build fails on private-looking content.
//
// Vite plugin exposing the repository's public Markdown as `virtual:docs`, rendered to HTML at
// build time. The Markdown files stay the single source of truth; nothing is copied into the site.
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs'
import { join, resolve } from 'node:path'
import MarkdownIt from 'markdown-it'
import { REFERENCE_AREAS, REPOSITORY_FILES, resolveRepoLink, routeForRepoPath, slugify } from '../scripts/slug.mjs'
import { findPrivate } from '../scripts/private-content.mjs'

const VIRTUAL_ID = 'virtual:docs'
const RESOLVED_ID = '\0' + VIRTUAL_ID
const ALERTS = { NOTE: 'Note', TIP: 'Tip', IMPORTANT: 'Important', WARNING: 'Warning', CAUTION: 'Caution' }

const decode = (s) =>
  s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&#39;/g, "'").replace(/&amp;/g, '&')
const plain = (html) => decode(html.replace(/<[^>]*>/g, '')).replace(/\s+/g, ' ').trim()

export function parseFrontmatter(source) {
  const match = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/.exec(source)
  if (!match) return { data: {}, body: source }
  const data = {}
  for (const line of match[1].split(/\r?\n/)) {
    const kv = /^(\w+):\s*(.*)$/.exec(line)
    if (kv) data[kv[1]] = kv[2].trim()
  }
  return { data, body: source.slice(match[0].length) }
}

// The download button links only to the page of a published GitHub release of this repository
// (the site links no file). docs/status.md sets it with `release_url` and `release_version`
// (both or neither); without them the button shows "Coming soon". scripts/check-dist.mjs checks
// the built page too.
export const RELEASE_VERSION = /^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/
export const releasePageUrl = (repoUrl, version) => `${repoUrl}/releases/tag/${version}`

/** docs/status.md's front matter: the single source of the release headline and link. */
export function parseStatus(source, repoUrl) {
  const { data } = parseFrontmatter(source)
  if (!data.release_headline || !data.release_planned) throw new Error('docs/status.md: release_headline and release_planned are required')
  const releaseUrl = data.release_url || null
  const releaseVersion = data.release_version || null
  if (!releaseUrl !== !releaseVersion) throw new Error('docs/status.md: set release_url and release_version together, or neither')
  if (releaseVersion && !RELEASE_VERSION.test(releaseVersion)) throw new Error(`docs/status.md: release_version must look like v0.1.0 (got ${releaseVersion})`)
  if (releaseUrl && releaseUrl !== releasePageUrl(repoUrl, releaseVersion)) {
    throw new Error(`docs/status.md: release_url must be ${releasePageUrl(repoUrl, releaseVersion)} (got ${releaseUrl})`)
  }
  return { releaseHeadline: data.release_headline, releasePlanned: data.release_planned, releaseUrl, releaseVersion }
}

function createRenderer(base, repoUrl, repoRoot) {
  // html: false escapes any raw HTML in the Markdown; markdown-it's default validateLink
  // rejects javascript:, vbscript: and data: URLs. Together that is the sanitisation.
  // typographer: false keeps the text exactly as written (legal wording, `->`, quotes).
  const md = new MarkdownIt({ html: false, linkify: false, typographer: false })

  md.core.ruler.push('heading_ids', (state) => {
    const seen = new Map()
    state.tokens.forEach((token, i) => {
      if (token.type !== 'heading_open') return
      const id = slugify(state.tokens[i + 1].content)
      const n = seen.get(id) ?? 0
      seen.set(id, n + 1)
      token.attrSet('id', n ? `${id}-${n + 1}` : id)
    })
  })

  // Relative links are repository paths (they work on GitHub). Each becomes the site route of
  // its target, or a GitHub link for the licence files; anything else fails the build.
  md.core.ruler.push('repo_links', (state) => {
    const from = state.env.from
    for (const block of state.tokens) {
      const children = block.children ?? []
      for (const [index, token] of children.entries()) {
        if (token.type !== 'link_open') continue
        const href = token.attrGet('href') ?? ''
        // THIRD_PARTY_NOTICES.md also ships inside the app (licenses/), where third-party/ holds
        // the bundled libraries' own licence files. Those paths exist only there: plain text here.
        if (from === 'THIRD_PARTY_NOTICES.md' && /^third-party\/[\w./-]+$/.test(href)) {
          const close = children.find((t, i) => i > index && t.type === 'link_close')
          token.tag = close.tag = 'span'
          token.attrs = [['class', 'in-app-path'], ['title', 'A file in the app: licenses/' + href]]
          continue
        }
        if (/^https?:\/\//.test(href)) {
          token.attrSet('rel', 'noopener noreferrer')
          continue
        }
        if (href.startsWith('#') || /^mailto:/.test(href)) continue
        const { target, hash } = resolveRepoLink(href, from)
        if (target.startsWith('..') || /^reference\/private(?:\/|$)/i.test(target)) {
          throw new Error(`${from}: link "${href}" points outside the published files`)
        }
        const route = routeForRepoPath(target)
        if (route) {
          token.attrSet('href', base + route.slice(1) + hash)
        } else if (REPOSITORY_FILES.test(target)) {
          const file = join(repoRoot, target)
          if (!existsSync(file)) throw new Error(`${from}: link "${href}" (${target}) is not a file in the repository`)
          token.attrSet('href', `${repoUrl}/${statSync(file).isDirectory() ? 'tree' : 'blob'}/main/${target}${hash}`)
          token.attrSet('rel', 'noopener noreferrer')
        } else {
          throw new Error(`${from}: link "${href}" (${target}) has no public page`)
        }
      }
    }
  })

  // GitHub alerts (> [!IMPORTANT] ...) become labelled callouts.
  md.core.ruler.push('alerts', (state) => {
    state.tokens.forEach((token, i) => {
      if (token.type !== 'blockquote_open') return
      const inline = state.tokens[i + 2]
      const m = inline?.type === 'inline' && /^\[!(\w+)\]\s*/.exec(inline.content)
      if (!m || !ALERTS[m[1]]) return
      const kind = m[1].toLowerCase()
      token.attrSet('class', `callout callout--${kind}`)
      token.attrSet('role', 'note')
      token.attrSet('aria-label', ALERTS[m[1]])
      token.attrSet('data-label', ALERTS[m[1]])
      // Drop the marker from the leading text tokens (it may span several), then its line break.
      let marker = `[!${m[1]}]`
      while (marker && inline.children[0]?.type === 'text') {
        const t = inline.children[0]
        if (marker.startsWith(t.content)) {
          marker = marker.slice(t.content.length)
          inline.children.shift()
        } else if (t.content.startsWith(marker)) {
          t.content = t.content.slice(marker.length).trimStart()
          marker = ''
        } else break
      }
      if (marker) throw new Error(`${state.env.from}: could not strip the [!${m[1]}] marker`)
      if (inline.children[0]?.type === 'softbreak') inline.children.shift()
    })
  })

  md.renderer.rules.table_open = () => '<div class="table-wrap" tabindex="0"><table>\n'
  md.renderer.rules.table_close = () => '</table></div>\n'
  return md
}

function sectionsOf(html) {
  return [...html.matchAll(/<h2 id="([^"]+)">([\s\S]*?)<\/h2>/g)].map((m) => ({ id: m[1], title: m[2] }))
}

/** The first paragraph's leading sentences, at most ~200 characters, as plain text. */
function summary(html) {
  const text = plain(/<p>([\s\S]*?)<\/p>/.exec(html)?.[1] ?? '')
  let out = ''
  for (const sentence of text.match(/[^.!?]+[.!?]+(?:\s|$)|[^.!?]+$/g) ?? []) {
    if (out && (out + sentence).length > 200) break
    out += sentence
  }
  out = out.trim()
  return out.length <= 200 ? out : `${out.slice(0, out.lastIndexOf(' ', 197))}…`
}

export function loadContent(repoRoot, base, repoUrl) {
  const md = createRenderer(base, repoUrl, repoRoot)
  const files = []
  const read = (path) => {
    files.push(join(repoRoot, path))
    return readFileSync(join(repoRoot, path), 'utf8')
  }
  const render = (source, from) => {
    const html = md.render(source, { from })
    const leak = findPrivate(html)
    if (leak) throw new Error(`${from}: ${leak}`)
    return html
  }
  const page = (path) => {
    const { data, body: source } = parseFrontmatter(read(path))
    const h1 = /^# (.+)$/m.exec(source)
    if (!h1) throw new Error(`${path}: no "# " title`)
    const html = render(source, path).replace(/^<h1[^>]*>[\s\S]*?<\/h1>\n?/, '')
    return { path, route: routeForRepoPath(path), title: plain(md.renderInline(h1[1], { from: path })), html, sections: sectionsOf(html), data }
  }

  const docs = readdirSync(join(repoRoot, 'docs'))
    .filter((file) => file.endsWith('.md'))
    .sort()
    .map((file) => {
      const { data, ...doc } = page(`docs/${file}`)
      const leak = findPrivate(data.description ?? '')
      if (leak) throw new Error(`docs/${file}: ${leak}`)
      return { ...doc, description: data.description || summary(doc.html), order: Number(data.order ?? 99) }
    })
    .sort((a, b) => a.order - b.order || a.path.localeCompare(b.path))
  const status = parseStatus(readFileSync(join(repoRoot, 'docs/status.md'), 'utf8'), repoUrl)

  // reference/README.md's Notes table is the list of published notes: their order and
  // descriptions. Every note file needs a row, and every row a file.
  const { data: _indexData, ...referenceIndex } = page('reference/README.md')
  referenceIndex.description = summary(referenceIndex.html)
  const rows = new Map()
  const indexSource = readFileSync(join(repoRoot, 'reference/README.md'), 'utf8')
  for (const line of indexSource.split('\n')) {
    const cells = line.split('|').slice(1, -1).map((c) => c.trim())
    if (cells.length !== 3) continue
    for (const [, href] of cells[1].matchAll(/\]\(([^)]+)\)/g)) {
      const { target } = resolveRepoLink(href, 'reference/README.md')
      rows.set(target, { order: rows.size, description: plain(md.renderInline(cells[2], { from: 'reference/README.md' })) })
    }
  }
  const notes = REFERENCE_AREAS.flatMap((area, areaIndex) => {
    const dir = join(repoRoot, 'reference', area.dir)
    if (!existsSync(dir)) return []
    return readdirSync(dir)
      .filter((file) => file.endsWith('.md'))
      .map((file) => {
        const path = `reference/${area.dir}/${file}`
        const row = rows.get(path)
        if (!row) throw new Error(`${path}: not listed in the Notes table of reference/README.md`)
        if (!row.description) throw new Error(`${path}: empty description in reference/README.md`)
        rows.delete(path)
        const { data: _data, ...note } = page(path)
        return { ...note, area: area.dir, areaLabel: area.label, areaIndex, ...row }
      })
  }).sort((a, b) => a.areaIndex - b.areaIndex || a.order - b.order)
  for (const path of rows.keys()) throw new Error(`reference/README.md lists ${path}, which is not a published note`)

  const { data: _noticesData, ...thirdParty } = page('THIRD_PARTY_NOTICES.md')
  thirdParty.description = summary(thirdParty.html)

  // README.md: only its introduction, the [!IMPORTANT] legal notice and the License paragraph.
  // The rest (status, build commands, contributor pointers) is not published: docs/ has the
  // public pages for it.
  const readme = read('README.md')
  const sectionText = (heading) => {
    const m = new RegExp(`^## ${heading.replace(/[()]/g, '\\$&')}\\s*$([\\s\\S]*?)(?=^## |(?![\\s\\S]))`, 'm').exec(readme)
    if (!m) throw new Error(`README.md: missing section "## ${heading}"`)
    return m[1].trim()
  }
  const head = readme.slice(0, readme.search(/^## /m)).replace(/^# .*\n/, '').trim()
  const blocks = head.split(/\n\s*\n/)
  const intro = blocks.find((b) => !b.startsWith('>'))
  const notice = blocks.find((b) => b.startsWith('> [!IMPORTANT]'))
  if (!intro || !notice) throw new Error('README.md: introduction or [!IMPORTANT] notice not found')
  const readmeParts = {
    intro: render(intro, 'README.md'),
    notice: render(notice, 'README.md'),
    license: render(sectionText('License'), 'README.md'),
  }

  return { content: { docs, status, notes, areas: REFERENCE_AREAS, referenceIndex, thirdParty, readme: readmeParts }, files }
}

export default function docsPlugin({ repoUrl, repoRoot = resolve(import.meta.dirname, '../..') }) {
  let base = '/'
  return {
    name: 'fm4-recomp-docs',
    configResolved(config) {
      base = config.base
    },
    resolveId(id) {
      if (id === VIRTUAL_ID) return RESOLVED_ID
    },
    load(id) {
      if (id !== RESOLVED_ID) return
      const { content, files } = loadContent(repoRoot, base, repoUrl)
      for (const file of files) this.addWatchFile(file)
      return Object.entries(content)
        .map(([name, value]) => `export const ${name} = ${JSON.stringify(value)}`)
        .join('\n')
    },
    handleHotUpdate({ file, server }) {
      if (file.startsWith(repoRoot) && file.endsWith('.md')) {
        const mod = server.moduleGraph.getModuleById(RESOLVED_ID)
        if (mod) server.reloadModule(mod)
      }
    },
  }
}
