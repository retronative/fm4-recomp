<!-- Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: serves docs/*.md and the reference notes by route; no installation-guide callout. -->
<script setup lang="ts">
import { computed } from 'vue'
import DocLayout from '../components/DocLayout.vue'
import { requirePage } from '../docs'

const props = defineProps<{ docRoute: string }>()
const doc = computed(() => requirePage(props.docRoute))
const note = computed(() => ('area' in doc.value ? doc.value : null))
</script>

<template>
  <DocLayout
    v-if="note"
    :key="note.route"
    :doc="note"
    :eyebrow="`Reference notes · ${note.areaLabel}`"
    :lead="note.description"
    :back="{ to: '/reference/', label: 'All reference notes' }"
  />
  <DocLayout v-else :key="doc.route" :doc="doc" eyebrow="Documentation" />
</template>
