// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: the docs plugin takes
// the repository URL for licence-file links.
import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import tailwindcss from '@tailwindcss/vite'
import docsPlugin from './plugins/docs.mjs'
import { base } from './scripts/site-config.mjs'
import { REPO_URL } from './src/data/links'

export default defineConfig({
  base,
  plugins: [vue(), tailwindcss(), docsPlugin({ repoUrl: REPO_URL })],
  server: { fs: { allow: ['..'] } },
  build: { target: 'es2022', cssCodeSplit: true },
})
