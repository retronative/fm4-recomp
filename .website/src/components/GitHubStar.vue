<script setup lang="ts">
// A plain link to the repository on GitHub, where a visitor can star it. No script, widget or
// request to GitHub: the site makes no third-party runtime requests (README.md, check:dist).
// `compact` shows only the star below the lg breakpoint, for the header.
import { REPO_URL } from '../data/links'

defineProps<{ compact?: boolean }>()
</script>

<template>
  <a
    :href="REPO_URL"
    target="_blank"
    rel="noopener noreferrer"
    class="btn gap-2"
    :class="compact ? 'w-11 justify-center px-0 lg:w-auto lg:px-3' : ''"
    aria-label="Star FM4 Recomp on GitHub"
  >
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round" aria-hidden="true">
      <path d="M12 2.8l2.84 5.76 6.36.92-4.6 4.48 1.08 6.33L12 17.3l-5.68 2.99 1.08-6.33-4.6-4.48 6.36-.92z" />
    </svg>
    <span :class="compact ? 'hidden lg:inline' : ''">{{ compact ? 'Star' : 'Star on GitHub' }}</span>
  </a>
</template>
