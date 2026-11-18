# FM4 Recomp website

The static documentation website for FM4 Recomp: Vite + Vue 3 + TypeScript + Tailwind CSS,
prerendered to plain HTML at build time. No server, no runtime data, no tracking, no third-party
requests (the font is self-hosted).

Adapted from the reFrozen project's website (GPL-3.0-or-later, like this repository). Each adapted
source file says so in its first line and names FM4 Recomp's changes; `package.json`,
`tsconfig.json` and `vercel.json` (JSON, no comments) are adapted too: same dependency pins and
compiler options, FM4's name and scripts.

## Structure

```
.website/
├── index.html              HTML template (head placeholders filled per route by the prerender)
├── vite.config.ts          Vite config; base path from SITE_BASE
├── vercel.json             Vercel build settings
├── plugins/docs.mjs        Vite plugin: the repository's public Markdown → `virtual:docs`
├── scripts/
│   ├── site-config.mjs     SITE_BASE / SITE_URL / SITE_CNAME handling
│   ├── slug.mjs            Heading-anchor slugs, repository path → route map, published reference areas
│   ├── private-content.mjs Patterns for text that must never be published (plugin and check-dist)
│   ├── prerender.mjs       Renders every route, writes 404.html, sitemap.xml, robots.txt, .nojekyll
│   ├── check-links.mjs     Link and anchor checker for dist/ (under SITE_BASE)
│   ├── check-dist.mjs      Artifact hygiene check for dist/ (file types, private content, third-party requests)
│   ├── check-status.mjs    Milestone states appear only in docs/status.md
│   ├── check-contrast.mjs  WCAG contrast audit of the text colours on each surface
│   ├── pages-sim.mjs       Local static server emulating GitHub Pages semantics
│   └── test-routing.mjs    Requests every route (with/without trailing slash) through pages-sim
├── public/
│   ├── brand/fm4-recomp-logo.svg   Copy of ../assets/fm4-recomp-logo.svg (header, footer, favicon)
│   └── fonts/              Inter (latin variable woff2 subset) and its OFL licence text
└── src/
    ├── app.ts, router.ts   App factory, routes and per-route titles/descriptions
    ├── entry-client.ts     Hydration and client-side meta updates
    ├── entry-server.ts     Server render used only by the prerender step
    ├── docs.ts             Access to the docs data
    ├── data/links.ts       The repository URL
    ├── components/         Header, footer, page hero, document layout, prose renderer, logo, download button
    ├── pages/              One lazy-loaded component per route
    └── styles/main.css     Design tokens and all custom styling
```

Routes: `/`, `/docs/` (the index), `/getting-started/`, `/compatibility/`, `/faq/`, `/releases/`
(`docs/status.md`), `/legal/` (and `/docs/<name>/` for any further `docs/*.md`, e.g.
`/docs/how-it-works/`), `/reference/`, `/reference/<area>/<note>/`, `/licences/`, plus a 404 page.

## Where the content comes from

The website never copies the repository's Markdown. `plugins/docs.mjs` reads it when Vite builds
(and watches it in dev), renders it with markdown-it and exposes it as `virtual:docs`:

| Source | Page |
|---|---|
| `README.md`: the introduction, the `[!IMPORTANT]` notice, the License paragraph | Home, Licences. Nothing else from `README.md` is published |
| `docs/getting-started.md`, `docs/compatibility.md`, `docs/faq.md`, `docs/LEGAL.md` | `/getting-started/`, `/compatibility/`, `/faq/`, `/legal/` (other `docs/*.md`: `/docs/<name>/`) |
| `docs/status.md` | `/releases/`; its front matter also gives the release headline and the download button (below) |
| `reference/README.md` | `/reference/` |
| `reference/{recompiler,kernel,gpu,perf}/*.md` | `/reference/<area>/<note>/` |
| `THIRD_PARTY_NOTICES.md` | `/licences/` |

- Only the four areas in `REFERENCE_AREAS` (`scripts/slug.mjs`) are read; no other directory
  under `reference/` is opened. Every note must have a row in the Notes table of
  `reference/README.md`, which gives its description and order; the build fails otherwise (and
  for a row without a note). A new area needs an entry in `REFERENCE_AREAS`.
- Titles are each file's `# ` heading. `docs/*.md` may start with front matter: `description`
  (the page's meta description and its line in the index; otherwise the first paragraph's leading
  sentences) and `order` (position in the index; 99 when absent).
- **Release status**: `docs/status.md` is the one place that says whether a release exists. Its
  front matter needs `release_headline` and `release_planned`; `release_version` (e.g.
  `v0.1.0`) and `release_url` are set together once that release is published, and
  `release_url` must be that tag's page on GitHub Releases. Until then the download button is a
  disabled "Coming soon"; after, it links the release page (never a file). Milestone states are
  written only in `docs/status.md` (`check:status`).
- `html: false` escapes any raw HTML in the Markdown and markdown-it's link validation rejects
  `javascript:`/`data:` URLs; that is the sanitisation.
- Headings get stable ids (`scripts/slug.mjs`, underscores kept as on GitHub).
- Relative links are resolved against the linking file's repository path, so the same files work
  on GitHub and on the site: links to the files above become site routes, links to `COPYING`,
  `NOTICE`, files and folders under `LICENSES/`, `CHANGELOG.md`, `CONTRIBUTING.md`, `SUPPORT.md`,
  `SECURITY.md` and `CODE_OF_CONDUCT.md` go to the repository on GitHub (the file must exist), and
  **any other relative link fails the build** (no page would exist for it). One exception:
  `THIRD_PARTY_NOTICES.md` also ships inside the app, and its `third-party/...` links name files
  of the app's `licenses/` folder; the site shows those as plain text.
- GitHub alert blockquotes (`> [!IMPORTANT]` etc.) become labelled callouts.
- The build fails if rendered text matches `scripts/private-content.mjs` (local-only files and
  folders, absolute local paths and similar); `check:dist` applies the same patterns to `dist/`.

## Commands

Run from `.website/` (Node 22.18+ or 24: `test:routing` imports `src/data/links.ts` directly):

```sh
npm install            # first time: installs the pinned versions and writes package-lock.json
npm ci                 # once package-lock.json is committed
npm run dev            # development server with hot reload
npm run build          # vue-tsc type-check, client build, SSR build, prerender into dist/
npm run preview        # serve dist/ locally
npm run check:links    # internal links and anchors in dist/
npm run check:dist     # artifact hygiene check of dist/
npm run check:status   # milestone states only in docs/status.md
npm run check:contrast # WCAG contrast of the design tokens
npm run test:routing   # every route through the GitHub Pages simulation
npm run validate       # check:status, check:contrast, build, check:links, check:dist, test:routing
```

### Configuration

| Variable | Default | Purpose |
|---|---|---|
| `SITE_BASE` | `/fm4-recomp/` | Path the site is served under. The default fits a GitHub project page `https://<user>.github.io/fm4-recomp/`; use `/` for a custom domain or Vercel |
| `SITE_URL` | `https://retronative.github.io` | Origin for canonical URLs, Open Graph and the sitemap |
| `SITE_CNAME` | unset | Custom domain; when set, `dist/CNAME` is written |

- **check:links** resolves every `href`/`src` in `dist/**/*.html` as if the site were mounted at
  `SITE_BASE` (a link outside the base fails), checks `#anchors` against the target page's ids,
  flags duplicate ids and checks lazy-chunk and preload references inside the built JS. External
  links are listed, never fetched.
- **check:dist**: only allowlisted file types (html, css, js, svg, woff2, txt, xml, json, ico,
  webmanifest, `CNAME`, `.nojekyll`; PNG only as the future icon and Open Graph files, ≤ 500 kB);
  no source maps; no file over 1 MB; no private-looking content (`scripts/private-content.mjs`);
  no link to a `.dmg`; the download button on the home and releases pages agrees with
  `docs/status.md` (a link to `release_url`, or disabled); no third-party runtime requests (external scripts, stylesheets, images,
  preconnects, CSS `@import`/`url()`, web-font services).
- **check:contrast** computes WCAG contrast for each text colour on each surface it is used on and
  fails below AA (3:1 for the red focus ring and rules).
- **test:routing** starts `pages-sim` and requests every prerendered route in both forms
  (`/fm4-recomp/legal` and `/fm4-recomp/legal/`), follows redirects, repeats each request, and
  checks the status, the final URL (trailing-slash form), the canonical tag (a sitemap URL) and
  that every referenced stylesheet, script, preload, icon, font and lazy chunk loads; deep links
  to each document's first heading; 404s with the site's `404.html` (including wrong-case paths);
  the Open Graph image when one is declared; and the head of every page (title, description,
  `og:`/`twitter:` tags, canonical = `og:url`, unique titles, valid JSON-LD, the footer's
  repository link, `robots.txt` naming the sitemap).

`pages-sim` emulates GitHub Pages' documented behaviour for a project site (mount at `SITE_BASE`,
301 to the trailing-slash form, `index.html` for directories, case-sensitive paths, `404.html`
with status 404). Run it by hand with `npm run pages-sim` (http://localhost:4790/fm4-recomp/).
Real GitHub Pages behaviour can only be confirmed after a deployment.

## Deploying (not set up)

`dist/` is fully static: one prerendered `index.html` per route, `404.html`, `sitemap.xml`,
`robots.txt`, `.nojekyll` and hashed assets. URLs use trailing slashes. No GitHub Pages workflow,
custom domain or deployment exists yet. For Vercel, import the repository with Root Directory
`.website` and keep "Include files outside the root directory in the Build Step" enabled (the
build reads the repository's Markdown); `vercel.json` builds with `SITE_BASE=/`.

## TODO: raster icons and Open Graph image

The site works without raster images: the favicon is the SVG logo and pages declare no Open Graph
image (`twitter:card` is `summary`). Still to do: a script that renders
`../assets/fm4-recomp-logo.svg` into `public/favicon-32.png`, `public/apple-touch-icon.png`,
`public/icon-192.png`, `public/icon-512.png` and a 1200×630 `public/og.png`, plus the matching
`<link>` tags (and a web manifest) in `index.html`. `scripts/prerender.mjs` adds the `og:image`
tags automatically once `public/og.png` exists, `check-dist` already allows exactly these file
names, and `test:routing` then checks the image's declared size.

## Asset and trademark policy

- **No game material**: no Forza artwork, logos, screenshots, sounds or game files. The only image
  is FM4 Recomp's own logo.
- **Nominative use only**: *Forza Motorsport 4* and Xbox 360 are named only to describe the
  project. The footer of every page carries `docs/LEGAL.md`'s first paragraph verbatim.
- **Font**: Inter (SIL Open Font License 1.1), self-hosted as a latin variable-weight woff2 subset,
  with its licence text beside the file in `public/fonts/` (also published). `font-display: swap`.
- **Download**: the site hosts no DMG and links no file; the download button goes to the GitHub
  Release page only, and only once `docs/status.md` names a published release.

## Accessibility

Skip link, landmarks, visible focus styles, keyboard-operable navigation, AA contrast, and no
motion.
