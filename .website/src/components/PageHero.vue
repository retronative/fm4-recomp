<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: no background paintings; a CSS track-stripe band instead. -->
<script setup lang="ts">
defineProps<{ eyebrow: string; title: string; lead?: string }>()
</script>

<template>
  <header class="page-hero">
    <div class="mx-auto max-w-6xl px-4 sm:px-8 py-12 sm:py-16">
      <p class="eyebrow">{{ eyebrow }}</p>
      <h1 class="page-title mt-3">{{ title }}</h1>
      <p v-if="lead" class="lead mt-4 max-w-3xl">{{ lead }}</p>
      <slot />
    </div>
  </header>
</template>
