// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: defaults for the
// fm4-recomp GitHub project page.
// Deployment settings, read from the environment at build time.
//   SITE_BASE  path the site is served under. Default "/fm4-recomp/" (GitHub project page
//              https://<user>.github.io/fm4-recomp/); use "/" for a custom domain or Vercel.
//   SITE_URL   public origin for canonical URLs, Open Graph and the sitemap.
//              Default https://retronative.github.io (canonical https://retronative.github.io/fm4-recomp/).
//   SITE_CNAME optional custom domain; when set, dist/CNAME is written for GitHub Pages.
export const DEFAULT_BASE = '/fm4-recomp/'
export const DEFAULT_SITE_URL = 'https://retronative.github.io'

export function normalizeBase(value) {
  let base = value || DEFAULT_BASE
  if (!base.startsWith('/')) base = `/${base}`
  if (!base.endsWith('/')) base = `${base}/`
  return base
}

export const base = normalizeBase(process.env.SITE_BASE)
export const siteUrl = (process.env.SITE_URL || DEFAULT_SITE_URL).replace(/\/+$/, '')
export const cname = (process.env.SITE_CNAME || '').trim()
