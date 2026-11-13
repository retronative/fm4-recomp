<!-- FM4 Recomp website page (GPL-3.0-or-later): README.md's License paragraph and THIRD_PARTY_NOTICES.md, rendered from the repository; the website's own credits. -->
<script setup lang="ts">
import PageHero from '../components/PageHero.vue'
import Prose from '../components/Prose.vue'
import { readme, thirdParty } from '../docs'

const fontLicence = `${import.meta.env.BASE_URL}fonts/OFL-Inter.txt`
</script>

<template>
  <div>
    <PageHero eyebrow="Fine print" title="Licences" />
    <div class="mx-auto max-w-4xl px-4 sm:px-8 py-12">
      <h2 id="licence" class="section-title">FM4 Recomp</h2>
      <Prose class="mt-6" :html="readme.license" />

      <h2 id="third-party" class="section-title mt-14">{{ thirdParty.title }}</h2>
      <Prose class="mt-6" :html="thirdParty.html" />

      <h2 id="website" class="section-title mt-14">This website</h2>
      <div class="prose mt-6">
        <p>
          The website's code is adapted from the reFrozen project's website (GPL-3.0-or-later). Text is set in
          Inter, self-hosted under the SIL Open Font License 1.1 (<a :href="fontLicence">licence text</a>).
        </p>
      </div>
    </div>
  </div>
</template>
