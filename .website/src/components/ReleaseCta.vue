<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: links the published Mac disk image on GitHub; FM4's plain button styles. -->
<script setup lang="ts">
// The tag in docs/status.md supplies the direct download and the release notes.

import { status } from '../docs'

const url = status.releaseDownloadUrl
const version = status.releaseVersion
const number = version?.replace(/^v/, '')
</script>

<template>
  <a v-if="url" class="btn btn--primary btn--cta" :href="url" rel="noopener noreferrer" data-release-cta="active" :data-release-version="version ?? undefined">
    <span class="btn__label">Download for Apple silicon</span>
    <span class="btn__meta">FM4 Recomp {{ number }} &middot; macOS disk image</span>
  </a>
  <button v-else type="button" class="btn btn--cta btn--soon" aria-disabled="true" aria-label="Download for Apple silicon: coming soon" data-release-cta="soon" @click.prevent>
    <span class="btn__label">Coming soon</span>
    <span class="btn__meta">Download for Apple silicon</span>
  </button>
  <a v-if="status.releaseUrl" class="btn" :href="status.releaseUrl" data-release-notes>Release notes and checksums</a>
</template>
