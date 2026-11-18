<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: FM4's card list; the reference notes and licences listed after the documents. -->
<script setup lang="ts">
import { RouterLink } from 'vue-router'
import PageHero from '../components/PageHero.vue'
import { docs, notes, referenceIndex, thirdParty } from '../docs'

const more = [
  { route: '/reference/', title: referenceIndex.title, description: `${notes.length} technical notes behind FM4 Recomp's design.` },
  { route: '/licences/', title: 'Licences', description: thirdParty.description },
]
</script>

<template>
  <div>
    <PageHero eyebrow="Documentation" title="Documentation" lead="Installing and playing, how it works, what is supported, and the fine print." />
    <div class="mx-auto max-w-4xl px-4 sm:px-8 py-12">
      <h2 id="docs-list-title" class="sr-only">All documents</h2>
      <ol class="grid gap-4 list-none m-0 p-0">
        <li v-for="doc in [...docs, ...more]" :key="doc.route">
          <RouterLink :to="doc.route" class="card">
            <span class="card__title">{{ doc.title }}</span>
            <span class="card__text">{{ doc.description }}</span>
          </RouterLink>
        </li>
      </ol>
    </div>
  </div>
</template>
