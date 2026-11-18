<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: FM4's links; the disclaimer is docs/LEGAL.md's first paragraph, verbatim. -->
<script setup lang="ts">
import { RouterLink } from 'vue-router'
import BrandMark from './BrandMark.vue'
import { docs } from '../docs'
import { REPO_URL } from '../data/links'

const legal = docs.find((d) => d.path === 'docs/LEGAL.md')
const disclaimer = /<p>[\s\S]*?<\/p>/.exec(legal?.html ?? '')?.[0]
if (!disclaimer) throw new Error('docs/LEGAL.md: no first paragraph for the footer disclaimer')
</script>

<template>
  <footer class="site-footer">
    <div class="mx-auto max-w-6xl px-4 sm:px-8 py-12">
      <div class="grid gap-10 md:grid-cols-[1.4fr_1fr]">
        <div>
          <div class="flex items-center gap-3">
            <BrandMark :height="30" />
            <span class="wordmark text-base">FM4 Recomp</span>
          </div>
          <p class="mt-4 mb-0 text-muted text-[.95rem] max-w-md">
            A static recompilation of <em>Forza Motorsport 4</em> (Xbox 360) to native arm64 macOS.
          </p>
          <p class="mt-4 mb-0 text-[.95rem]">
            <a :href="REPO_URL" class="inline-flex items-center gap-2">
              <svg aria-hidden="true" width="16" height="16" viewBox="0 0 16 16" fill="currentColor">
                <path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z" />
              </svg>
              Source on GitHub
            </a>
          </p>
        </div>
        <nav aria-label="Footer">
          <p class="eyebrow mb-3">Pages</p>
          <ul class="list-none m-0 p-0 space-y-1.5 text-[.95rem]">
            <li><RouterLink to="/getting-started/">Getting started</RouterLink></li>
            <li><RouterLink to="/compatibility/">Compatibility</RouterLink></li>
            <li><RouterLink to="/faq/">FAQ</RouterLink></li>
            <li><RouterLink to="/releases/">Status and releases</RouterLink></li>
            <li><RouterLink to="/docs/">Documentation</RouterLink></li>
            <li><RouterLink to="/reference/">Reference notes</RouterLink></li>
            <li><RouterLink to="/legal/">Legal notice and contents</RouterLink></li>
            <li><RouterLink to="/licences/">Licences and third-party notices</RouterLink></li>
          </ul>
        </nav>
      </div>

      <div class="disclaimer" role="note" aria-label="Disclaimer">
        <div class="disclaimer__text" v-html="disclaimer" />
        <p class="mt-3 mb-0">
          FM4 Recomp is free software under the GNU General Public License version 3 or later
          (<RouterLink to="/licences/">GPL-3.0-or-later</RouterLink>).
        </p>
      </div>
    </div>
  </footer>
</template>
