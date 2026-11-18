// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: docs and reference
// notes looked up by route; the status is docs/status.md's release headline and link.
import { areas, docs, notes, readme, referenceIndex, status, thirdParty, type Doc, type Note, type Page, type Status } from 'virtual:docs'

export { areas, docs, notes, readme, referenceIndex, status, thirdParty }
export type { Doc, Note, Page, Status }

export function requirePage(route: string): Doc | Note {
  const page = docs.find((d) => d.route === route) ?? notes.find((n) => n.route === route)
  if (!page) throw new Error(`No document for ${route}`)
  return page
}
