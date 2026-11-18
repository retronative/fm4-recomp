// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: underscores kept in
// heading slugs (as GitHub does), repository path -> site route map for docs/, reference/ and
// THIRD_PARTY_NOTICES.md; the community files link to the repository.
// Shared by the docs plugin and the checks so heading anchors and routes agree.
import { posix } from 'node:path'

export function slugify(text) {
  return text
    .toLowerCase()
    .normalize('NFKD')
    .replace(/[̀-ͯ]/g, '')
    .replace(/<[^>]*>/g, '')
    .replace(/&[a-z]+;|&#\d+;/g, '')
    .replace(/[^a-z0-9\s_-]/g, '')
    .trim()
    .replace(/\s+/g, '-')
    .replace(/-+/g, '-')
}

// The reference areas the site publishes, in index order. Only these directories are read;
// nothing else under reference/ is ever opened.
export const REFERENCE_AREAS = [
  { dir: 'recompiler', label: 'Recompiler' },
  { dir: 'kernel', label: 'Kernel' },
  { dir: 'gpu', label: 'GPU' },
  { dir: 'perf', label: 'Performance' },
]

// Repository files with a dedicated page.
export const DEDICATED_ROUTES = {
  'README.md': '/',
  'docs/getting-started.md': '/getting-started/',
  'docs/status.md': '/releases/',
  'docs/compatibility.md': '/compatibility/',
  'docs/faq.md': '/faq/',
  'docs/LEGAL.md': '/legal/',
  'reference/README.md': '/reference/',
  'THIRD_PARTY_NOTICES.md': '/licences/',
}

const pageSlug = (name) => name.replace(/\.md$/, '').toLowerCase().replace(/_/g, '-')

/** Site route for a repository-relative Markdown path, or null if it has no page. */
export function routeForRepoPath(path) {
  if (DEDICATED_ROUTES[path]) return DEDICATED_ROUTES[path]
  let m = /^docs\/([\w-]+\.md)$/.exec(path)
  if (m) return `/docs/${pageSlug(m[1])}/`
  m = /^reference\/([\w-]+)\/([\w-]+\.md)$/.exec(path)
  if (m && REFERENCE_AREAS.some((a) => a.dir === m[1])) return `/reference/${m[1]}/${pageSlug(m[2])}/`
  return null
}

// Licence and community files linked from the content; they have no page of their own and link
// to the repository on GitHub.
export const REPOSITORY_FILES = /^(?:COPYING|NOTICE|LICENSES(?:\/[\w.-]+){0,2}|CHANGELOG\.md|CONTRIBUTING\.md|SUPPORT\.md|SECURITY\.md|CODE_OF_CONDUCT\.md)$/

/** Repository-relative target of a relative link `href` found in the file `from`. */
export function resolveRepoLink(href, from) {
  const [path, fragment = ''] = href.split('#')
  const hash = fragment ? `#${fragment}` : ''
  const target = path === '' ? from : posix.normalize(posix.join(posix.dirname(from), path)).replace(/\/$/, '')
  return { target, hash }
}
