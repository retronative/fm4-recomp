// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: no screenshots, hidden
// pages or aliases.
import { renderToString } from 'vue/server-renderer'
import { createApp } from './app'
import { metaFor, staticPaths } from './router'

export { staticPaths }
export { REPO_URL } from './data/links'

export async function render(url: string) {
  const { app, router } = createApp()
  await router.push(url)
  await router.isReady()
  const html = await renderToString(app)
  const route = router.currentRoute.value
  return { html, meta: metaFor(route), notFound: route.name === 'not-found' }
}
