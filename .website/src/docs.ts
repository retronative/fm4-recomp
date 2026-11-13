// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: docs and reference
// notes looked up by route; no project-status data.
import { areas, docs, notes, readme, referenceIndex, thirdParty, type Doc, type Note, type Page } from 'virtual:docs'

export { areas, docs, notes, readme, referenceIndex, thirdParty }
export type { Doc, Note, Page }

export function requirePage(route: string): Doc | Note {
  const page = docs.find((d) => d.route === route) ?? notes.find((n) => n.route === route)
  if (!page) throw new Error(`No document for ${route}`)
  return page
}
