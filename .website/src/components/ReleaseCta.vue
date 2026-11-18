<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: links the published release's page on GitHub (never a file); FM4's plain button styles. -->
<script setup lang="ts">
// The download button. It links only to the published release's page named in docs/status.md
// (`release_url`, `release_version`); until then it is a disabled "Coming soon" button that is
// not a link. scripts/check-dist.mjs fails the build if the built page disagrees with
// docs/status.md.
import { status } from '../docs'

const url = status.releaseUrl
const version = status.releaseVersion
const number = version?.replace(/^v/, '')
</script>

<template>
  <a v-if="url" class="btn btn--primary btn--cta" :href="url" rel="noopener noreferrer" data-release-cta="active" :data-release-version="version ?? undefined">
    <span class="btn__label">Download for Apple silicon</span>
    <span class="btn__meta">FM4 Recomp {{ number }} &middot; release page on GitHub</span>
  </a>
  <button v-else type="button" class="btn btn--cta btn--soon" aria-disabled="true" aria-label="Download for Apple silicon: coming soon" data-release-cta="soon" @click.prevent>
    <span class="btn__label">Coming soon</span>
    <span class="btn__meta">Download for Apple silicon</span>
  </button>
</template>
