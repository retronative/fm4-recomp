// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: the virtual:docs shape
// of plugins/docs.mjs (docs, reference notes, README parts, third-party notices); the status is
// the release headline and link only.
/// <reference types="vite/client" />

declare module 'virtual:docs' {
  export interface DocSection {
    id: string
    /** Inline HTML. */
    title: string
  }
  export interface Page {
    /** Repository-relative source file, e.g. docs/LEGAL.md. */
    path: string
    route: string
    title: string
    html: string
    sections: DocSection[]
  }
  export interface Doc extends Page {
    description: string
    /** Front matter `order` (docs/*.md); 99 when absent. */
    order?: number
  }
  /** docs/status.md's front matter. */
  export interface Status {
    releaseHeadline: string
    releasePlanned: string
    /** The published release's page on GitHub; null until a release is published. */
    releaseUrl: string | null
    releaseDownloadUrl: string | null
    releaseVersion: string | null
  }
  export interface Note extends Doc {
    area: string
    areaLabel: string
    order: number
  }
  export interface Area {
    dir: string
    label: string
  }
  /** README.md sections the site publishes, rendered to HTML. */
  export interface ReadmeParts {
    intro: string
    notice: string
    license: string
  }
  export const docs: Doc[]
  export const status: Status
  export const notes: Note[]
  export const areas: Area[]
  export const referenceIndex: Doc
  export const thirdParty: Doc
  export const readme: ReadmeParts
}

declare module '*.vue' {
  import type { DefineComponent } from 'vue'
  const component: DefineComponent<object, object, unknown>
  export default component
}
