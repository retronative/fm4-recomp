<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: rewritten for FM4: README introduction and legal notice (verbatim, rendered from README.md), the release headline and download button from docs/status.md, links to the documentation; no artwork or status log. -->
<script setup lang="ts">
import { RouterLink } from 'vue-router'
import BrandMark from '../components/BrandMark.vue'
import GitHubStar from '../components/GitHubStar.vue'
import Prose from '../components/Prose.vue'
import ReleaseCta from '../components/ReleaseCta.vue'
import { areas, notes, readme, status } from '../docs'
import { REPO_URL } from '../data/links'

const noteCount = notes.length
const areaNames = areas.filter((a) => notes.some((n) => n.area === a.dir)).map((a) => a.label)

const cards = [
  { to: '/getting-started/', title: 'Getting started', text: 'What you need, installing the beta from your own disc, controls and known issues.' },
  { to: '/docs/how-it-works/', title: 'How it works', text: 'Static recompilation in plain terms, and why this is not an emulator.' },
  { to: '/compatibility/', title: 'Compatibility', text: 'The supported game version, Macs, controllers and parts of the game.' },
  { to: '/faq/', title: 'FAQ', text: 'Common questions: the game files, performance, online play, saves.' },
  { to: '/releases/', title: 'Status and releases', text: 'Milestones, what the beta contains and its known issues.' },
  { to: '/reference/', title: 'Reference notes', text: `${noteCount} technical notes: ${areaNames.join(', ')}.` },
  { to: '/legal/', title: 'Legal notice and contents', text: 'What the repository and the macOS DMG contain, and what they do not.' },
  { to: '/licences/', title: 'Licences', text: 'GPL-3.0-or-later, and the third-party code FM4 Recomp uses.' },
]
</script>

<template>
  <div>
    <section class="home-hero" aria-labelledby="home-title">
      <div class="mx-auto max-w-6xl px-4 sm:px-8 py-14 sm:py-20 grid gap-10 md:grid-cols-[1fr_minmax(0,22rem)] items-center">
        <div>
          <p class="eyebrow">Xbox 360 &rarr; arm64 macOS</p>
          <h1 id="home-title" class="page-title mt-3">FM4 Recomp</h1>
          <Prose class="lead mt-5" :html="readme.intro" />
          <p class="mt-6 mb-0 text-muted" data-release-status>
            <strong>{{ status.releaseHeadline }}.</strong> {{ status.releasePlanned }}.
            <RouterLink to="/releases/">Status and releases</RouterLink>
          </p>
          <div class="mt-6 flex flex-wrap items-stretch gap-4">
            <ReleaseCta />
            <GitHubStar />
            <RouterLink to="/getting-started/" class="btn">Getting started</RouterLink>
            <a :href="`${REPO_URL}/tree/main/ports/v0.3.0`" class="btn">0.3.0 source on GitHub</a>
          </div>
        </div>
        <div class="hidden md:block">
          <BrandMark :height="214" />
        </div>
      </div>
    </section>

    <section class="mx-auto max-w-6xl px-4 sm:px-8 pt-12" aria-label="Legal notice">
      <Prose :html="readme.notice" />
    </section>

    <section class="mx-auto max-w-6xl px-4 sm:px-8 py-12" aria-labelledby="home-docs">
      <h2 id="home-docs" class="section-title">Documentation</h2>
      <ul class="mt-6 grid gap-4 sm:grid-cols-2 list-none m-0 p-0">
        <li v-for="card in cards" :key="card.to">
          <RouterLink :to="card.to" class="card">
            <span class="card__title">{{ card.title }}</span>
            <span class="card__text">{{ card.text }}</span>
          </RouterLink>
        </li>
      </ul>
    </section>
  </div>
</template>
