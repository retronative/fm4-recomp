<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: the release headline and download button above docs/status.md rendered in full; no scope columns or milestone log. -->
<script setup lang="ts">
// Rendered from docs/status.md: its front matter gives the headline and the button's link, its
// body the milestones, the release's contents and the known issues.
import DocLayout from '../components/DocLayout.vue'
import Prose from '../components/Prose.vue'
import ReleaseCta from '../components/ReleaseCta.vue'
import { requirePage, status } from '../docs'

const props = defineProps<{ docRoute: string }>()
const doc = requirePage(props.docRoute)
</script>

<template>
  <DocLayout :doc="doc" eyebrow="Status">
    <section class="release-panel" aria-labelledby="release-headline">
      <p class="eyebrow">Release</p>
      <h2 id="release-headline" class="section-title mt-2">{{ status.releaseHeadline }}</h2>
      <p class="mt-2 mb-0 text-muted">{{ status.releasePlanned }}</p>
      <div class="mt-5 flex flex-wrap items-stretch gap-4"><ReleaseCta /></div>
    </section>
    <Prose class="mt-10" :html="doc.html" />
  </DocLayout>
</template>
