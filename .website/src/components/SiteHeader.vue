<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: FM4's navigation and wordmark, no game clock. -->
<script setup lang="ts">
import { ref, watch } from 'vue'
import { RouterLink, useRoute } from 'vue-router'
import BrandMark from './BrandMark.vue'

const links = [
  { to: '/', label: 'Home' },
  { to: '/getting-started/', label: 'Getting started' },
  { to: '/compatibility/', label: 'Compatibility' },
  { to: '/faq/', label: 'FAQ' },
  { to: '/releases/', label: 'Releases' },
  { to: '/docs/', label: 'Docs' },
]

const route = useRoute()
const open = ref(false)
watch(() => route.fullPath, () => (open.value = false))

function isCurrent(to: string) {
  if (to === '/docs/') return ['/docs/', '/reference/', '/legal/', '/licences/'].some((p) => route.path.startsWith(p))
  return to === '/' ? route.path === '/' : route.path.startsWith(to)
}
</script>

<template>
  <header class="site-header">
    <div class="mx-auto flex h-full max-w-6xl items-center gap-4 px-4 sm:px-8">
      <RouterLink to="/" class="flex items-center gap-3 no-underline text-chalk mr-auto" aria-label="FM4 Recomp home">
        <BrandMark :height="34" />
        <span class="wordmark">FM4 Recomp</span>
      </RouterLink>

      <nav aria-label="Main" class="hidden md:block">
        <ul class="flex items-center gap-6 list-none m-0 p-0">
          <li v-for="l in links" :key="l.to">
            <RouterLink :to="l.to" class="nav-link" :aria-current="isCurrent(l.to) ? 'page' : undefined">{{ l.label }}</RouterLink>
          </li>
        </ul>
      </nav>

      <button
        type="button"
        class="md:hidden inline-flex items-center justify-center w-11 h-11 border border-seam bg-black/40 text-chalk"
        :aria-expanded="open"
        aria-controls="mobile-nav"
        @click="open = !open"
      >
        <span class="sr-only">{{ open ? 'Close menu' : 'Open menu' }}</span>
        <svg width="20" height="20" viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.6" aria-hidden="true">
          <path v-if="!open" d="M2 5h16M2 10h16M2 15h16" />
          <path v-else d="M4 4l12 12M16 4L4 16" />
        </svg>
      </button>
    </div>

    <nav v-show="open" id="mobile-nav" aria-label="Main (mobile)" class="md:hidden border-b border-seam bg-asphalt shadow-2xl">
      <ul class="list-none m-0 px-4 py-3">
        <li v-for="l in links" :key="l.to" class="border-b border-white/5 last:border-0">
          <RouterLink :to="l.to" class="nav-link block py-3" :aria-current="isCurrent(l.to) ? 'page' : undefined">{{ l.label }}</RouterLink>
        </li>
      </ul>
    </nav>
  </header>
</template>
