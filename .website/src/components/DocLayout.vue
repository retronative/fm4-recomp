<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: no banner art; back link configurable. -->
<script setup lang="ts">
// Shared frame for a rendered document: hero, table of contents, prose.
import { RouterLink } from 'vue-router'
import PageHero from './PageHero.vue'
import Prose from './Prose.vue'
import type { Page } from '../docs'

defineProps<{
  doc: Page
  eyebrow: string
  lead?: string
  back?: { to: string; label: string }
}>()
</script>

<template>
  <div>
    <PageHero :eyebrow="eyebrow" :title="doc.title" :lead="lead" />
    <div class="mx-auto grid max-w-6xl gap-12 px-4 sm:px-8 py-12 lg:grid-cols-[15rem_1fr]">
      <aside v-if="doc.sections.length > 1" class="lg:sticky lg:top-[calc(var(--header-h)+2rem)] self-start">
        <nav :aria-label="`On this page: ${doc.title}`">
          <p class="eyebrow mb-3">On this page</p>
          <ul class="toc list-none m-0 p-0">
            <li v-for="s in doc.sections" :key="s.id">
              <a :href="`#${s.id}`" v-html="s.title" />
            </li>
          </ul>
        </nav>
      </aside>
      <article class="min-w-0" :class="{ 'lg:col-span-2': doc.sections.length <= 1 }">
        <slot :doc="doc">
          <Prose :html="doc.html" />
        </slot>
        <p v-if="back" class="mt-16 border-t border-seam pt-6 text-[.9rem]">
          <RouterLink :to="back.to">&larr; {{ back.label }}</RouterLink>
        </p>
      </article>
    </div>
  </div>
</template>
