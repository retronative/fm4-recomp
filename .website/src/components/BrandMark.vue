<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: FM4 Recomp's logo (SVG, copied from assets/), no raster sizes. -->
<script setup lang="ts">
// The project logo (public/brand/, a copy of assets/fm4-recomp-logo.svg; viewBox 640x390).
// Decorative: the link around it carries the name.
const props = defineProps<{ height?: number }>()
const height = props.height ?? 36
const width = Math.round((height * 640) / 390)
const src = `${import.meta.env.BASE_URL}brand/fm4-recomp-logo.svg`
</script>

<template>
  <img class="brand-mark" :src="src" :width="width" :height="height" alt="" decoding="async" />
</template>
