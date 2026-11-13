<!-- FM4 Recomp website page (GPL-3.0-or-later): README.md's "Getting started (developers)" commands and legal notice, rendered from README.md. -->
<script setup lang="ts">
import { RouterLink } from 'vue-router'
import PageHero from '../components/PageHero.vue'
import Prose from '../components/Prose.vue'
import { readme } from '../docs'
</script>

<template>
  <div>
    <PageHero eyebrow="Developers" title="Getting started" />
    <div class="mx-auto max-w-4xl px-4 sm:px-8 py-12">
      <Prose :html="readme.notice" />
      <h2 id="setup" class="section-title mt-12">Setup</h2>
      <Prose class="mt-6" :html="readme.gettingStarted" />
      <p class="mt-12 border-t border-seam pt-6 text-[.95rem] text-muted">
        Next: <RouterLink to="/reference/">the reference notes</RouterLink> and
        <RouterLink to="/legal/">the legal notice</RouterLink>.
      </p>
    </div>
  </div>
</template>
