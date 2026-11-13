<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: the reference notes grouped by area, then reference/README.md rendered in full. -->
<script setup lang="ts">
import { RouterLink } from 'vue-router'
import PageHero from '../components/PageHero.vue'
import Prose from '../components/Prose.vue'
import { areas, notes, referenceIndex } from '../docs'

const groups = areas
  .map((area) => ({ ...area, notes: notes.filter((n) => n.area === area.dir) }))
  .filter((group) => group.notes.length)
</script>

<template>
  <div>
    <PageHero eyebrow="Documentation" :title="referenceIndex.title" :lead="referenceIndex.description" />
    <div class="mx-auto max-w-6xl px-4 sm:px-8 py-12">
      <section v-for="group in groups" :key="group.dir" class="mb-12" :aria-labelledby="`area-${group.dir}`">
        <h2 :id="`area-${group.dir}`" class="section-title">{{ group.label }}</h2>
        <ul class="mt-5 grid gap-4 md:grid-cols-2 list-none m-0 p-0">
          <li v-for="note in group.notes" :key="note.route">
            <RouterLink :to="note.route" class="card">
              <span class="card__title">{{ note.title }}</span>
              <span class="card__text">{{ note.description }}</span>
            </RouterLink>
          </li>
        </ul>
      </section>
      <section class="border-t border-seam pt-10" aria-labelledby="about-notes">
        <h2 id="about-notes" class="section-title">About these notes</h2>
        <Prose class="mt-6" :html="referenceIndex.html" />
      </section>
    </div>
  </div>
</template>
