// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: FM4's routes (home, the
// documentation index, one page per docs/*.md and reference note, the releases page from
// docs/status.md, reference index, licences); one static route per document instead of a slug
// parameter; no aliases.
import {
  createRouter,
  createMemoryHistory,
  createWebHistory,
  type RouteLocationNormalized,
  type RouteRecordRaw,
} from 'vue-router'
import { docs, notes, referenceIndex, thirdParty } from './docs'

export interface PageMeta {
  title: string
  description: string
  /** Canonical path (with trailing slash); undefined for the 404 page. */
  canonical?: string
  /** Breadcrumb trail (structured data), ending with the page itself. */
  breadcrumbs?: { name: string; path: string }[]
}

export const SITE = 'FM4 Recomp'
export const SITE_DESCRIPTION =
  'A static recompilation of Forza Motorsport 4 (Xbox 360, USA disc, title ID 4D530910) to native arm64 macOS, built on XenonRecomp. Unofficial fan project.'

const DocPage = () => import('./pages/DocPage.vue')

const routes: RouteRecordRaw[] = [
  {
    path: '/',
    name: 'home',
    component: () => import('./pages/HomePage.vue'),
    meta: { title: `${SITE} — Forza Motorsport 4, recompiled for arm64 macOS`, description: SITE_DESCRIPTION, crumb: 'Home' },
  },
  {
    path: '/docs/',
    name: 'docs',
    component: () => import('./pages/DocsIndexPage.vue'),
    meta: {
      title: `Documentation — ${SITE}`,
      description: 'Getting started, how static recompilation works, status and releases, compatibility, FAQ, the legal notice and the reference notes.',
      crumb: 'Documentation',
    },
  },
  ...docs.map(
    (doc): RouteRecordRaw => ({
      path: doc.route,
      name: `doc:${doc.path}`,
      component: doc.path === 'docs/status.md' ? () => import('./pages/ReleasesPage.vue') : DocPage,
      props: { docRoute: doc.route },
      meta: { title: `${doc.title} — ${SITE}`, description: doc.description, crumb: doc.title },
    }),
  ),
  {
    path: '/reference/',
    name: 'reference',
    component: () => import('./pages/ReferenceIndexPage.vue'),
    meta: { title: `${referenceIndex.title} — ${SITE}`, description: referenceIndex.description, crumb: referenceIndex.title },
  },
  ...notes.map(
    (note): RouteRecordRaw => ({
      path: note.route,
      name: `note:${note.path}`,
      component: DocPage,
      props: { docRoute: note.route },
      meta: { title: `${note.title} — ${referenceIndex.title} — ${SITE}`, description: note.description, crumb: note.title, parent: '/reference/' },
    }),
  ),
  {
    path: '/licences/',
    name: 'licences',
    component: () => import('./pages/LicencesPage.vue'),
    meta: {
      title: `Licences — ${SITE}`,
      description: `FM4 Recomp is licensed under the GPL-3.0-or-later. ${thirdParty.description}`,
      crumb: 'Licences',
    },
  },
  {
    path: '/:pathMatch(.*)*',
    name: 'not-found',
    component: () => import('./pages/NotFoundPage.vue'),
    meta: { title: `Page not found — ${SITE}`, description: 'This page does not exist.' },
  },
]

/** Every prerendered path. */
export const staticPaths = routes.map((r) => r.path).filter((p) => !p.includes(':'))

const crumbOf = (path: string) => String(routes.find((r) => r.path === path)?.meta?.crumb ?? '')

export function metaFor(route: RouteLocationNormalized): PageMeta {
  const meta = route.meta as unknown as PageMeta & { crumb?: string; parent?: string }
  if (route.name === 'not-found') return { title: meta.title, description: meta.description }
  const canonical = route.matched[route.matched.length - 1]?.path ?? route.path
  const breadcrumbs =
    canonical === '/'
      ? undefined
      : [
          { name: 'Home', path: '/' },
          ...(meta.parent ? [{ name: crumbOf(meta.parent), path: meta.parent }] : []),
          { name: meta.crumb ?? meta.title, path: canonical },
        ]
  return { title: meta.title, description: meta.description, canonical, breadcrumbs }
}

export function createAppRouter(base: string) {
  return createRouter({
    history: import.meta.env.SSR ? createMemoryHistory(base) : createWebHistory(base),
    routes,
    scrollBehavior(to, _from, saved) {
      if (saved) return saved
      if (to.hash) return { el: to.hash, top: 96 }
      return { top: 0 }
    },
  })
}
