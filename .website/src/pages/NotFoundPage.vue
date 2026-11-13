<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: FM4's wording and links. -->
<script setup lang="ts">
import { RouterLink } from 'vue-router'
import PageHero from '../components/PageHero.vue'
</script>

<template>
  <PageHero eyebrow="404" title="Page not found" lead="There is no page at this address.">
    <div class="mt-8 flex flex-wrap gap-4">
      <RouterLink to="/" class="btn btn--primary">Home</RouterLink>
      <RouterLink to="/reference/" class="btn">Reference notes</RouterLink>
    </div>
  </PageHero>
</template>
