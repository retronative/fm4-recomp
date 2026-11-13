// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: the private-content
// patterns of check-dist.mjs moved here and rewritten for this repository, so the docs plugin
// (at build time) and check-dist (on dist/) refuse the same things.
//
// Text that must never be published. The generic patterns are below; maintainers add the
// repository-specific ones (local-only file and folder names, privately cited projects, machine
// names) in the git-ignored private-patterns.local.json next to package.json, so the list itself
// is never published: [["<regex source>", "<flags>", "<why>"], ...].
import { existsSync, readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

const GENERIC = [
  [/\bsub-?agents?\b/i, 'agent note'],
  [/\/Users\/|\/home\/[a-z]|[A-Z]:\\\\Users/, 'absolute local path'],
  [/[\w-]\.iso\b/i, 'disc image name'],
  [/sourceMappingURL=/, 'source map reference'],
]

const localFile = fileURLToPath(new URL('../private-patterns.local.json', import.meta.url))
const LOCAL = existsSync(localFile)
  ? JSON.parse(readFileSync(localFile, 'utf8')).map(([source, flags, why]) => [new RegExp(source, flags), why])
  : []

export const FORBIDDEN = [...GENERIC, ...LOCAL]
export const LOCAL_PATTERNS = LOCAL.length

/** The first forbidden match in `text`, as "why: …context…", or null. */
export function findPrivate(text) {
  for (const [pattern, why] of FORBIDDEN) {
    const m = pattern.exec(text)
    if (m) return `${why}: "…${text.slice(Math.max(0, m.index - 30), m.index + m[0].length + 30).replace(/\s+/g, ' ')}…"`
  }
  return null
}
