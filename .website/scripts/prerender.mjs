// Adapted from reFrozen .website (GPL-3.0-or-later); FM4 Recomp changes: no screenshot manifest,
// hidden pages or aliases; JSON-LD names only the site and its source repository; the Open
// Graph image is optional (added only when public/og.png exists).
// Renders every route to static HTML after the client and SSR builds, then writes
// 404.html, sitemap.xml, robots.txt and .nojekyll. Static output only.
import { existsSync, readFileSync, writeFileSync, mkdirSync, rmSync } from 'node:fs'
import { dirname, join, resolve } from 'node:path'
import { pathToFileURL } from 'node:url'
import { base, siteUrl, cname } from './site-config.mjs'

const root = resolve(import.meta.dirname, '..')
const dist = join(root, 'dist')
const ssrDir = join(root, '.ssr')
const { render, staticPaths, REPO_URL } = await import(pathToFileURL(join(ssrDir, 'entry-server.js')).href)

const template = readFileSync(join(dist, 'index.html'), 'utf8')

const escapeAttr = (value) =>
  value.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;').replace(/>/g, '&gt;')

function absolute(path) {
  return siteUrl + base + path.replace(/^\//, '')
}

// Open Graph image: none until public/og.png is made from the logo (see README.md). When it
// exists, its pixel size is read from the PNG header.
const ogFile = join(dist, 'og.png')
let ogHead = ''
if (existsSync(ogFile)) {
  const png = readFileSync(ogFile)
  if (png.readUInt32BE(0) !== 0x89504e47) throw new Error('public/og.png is not a PNG file')
  const [width, height] = [png.readUInt32BE(16), png.readUInt32BE(20)]
  ogHead =
    `\n    <meta property="og:image" content="${absolute('/og.png')}" />` +
    `\n    <meta property="og:image:type" content="image/png" />` +
    `\n    <meta property="og:image:width" content="${width}" />` +
    `\n    <meta property="og:image:height" content="${height}" />` +
    `\n    <meta property="og:image:alt" content="The FM4 Recomp logo: the words FM4 RECOMP inside a racing circuit outline." />` +
    `\n    <meta name="twitter:image" content="${absolute('/og.png')}" />`
}

// JSON-LD with facts stated on the site only: the WebSite and its source repository on the
// home page, BreadcrumbList elsewhere. No versions, offers or ratings (nothing is released).
function structuredData(meta) {
  const url = absolute(meta.canonical)
  const graph = []
  if (meta.canonical === '/') {
    graph.push(
      { '@type': 'WebSite', name: 'FM4 Recomp', url, inLanguage: 'en', description: meta.description },
      { '@type': 'SoftwareSourceCode', name: 'FM4 Recomp', description: meta.description, codeRepository: REPO_URL, license: 'https://spdx.org/licenses/GPL-3.0-or-later.html' },
    )
  }
  if (meta.breadcrumbs) {
    graph.push({
      '@type': 'BreadcrumbList',
      itemListElement: meta.breadcrumbs.map((crumb, i) => ({ '@type': 'ListItem', position: i + 1, name: crumb.name, item: absolute(crumb.path) })),
    })
  }
  if (!graph.length) return ''
  const json = JSON.stringify({ '@context': 'https://schema.org', '@graph': graph }).replace(/</g, '\\u003c')
  return `\n    <script type="application/ld+json">${json}</script>`
}

function page({ html, meta, notFound }) {
  const title = escapeAttr(meta.title)
  const description = escapeAttr(meta.description)
  const extraHead = notFound
    ? '<meta name="robots" content="noindex" />'
    : `<link rel="canonical" href="${absolute(meta.canonical)}" />\n    <meta property="og:url" content="${absolute(meta.canonical)}" />` +
      structuredData(meta)
  return template
    .replace(/<title>.*?<\/title>/, () => `<title>${title}</title>`)
    .replace(/(<meta (?:name|property)="(?:description|og:description|twitter:description)" content=")[^"]*"/g, (_, attr) => `${attr}${description}"`)
    .replace(/(<meta (?:property|name)="(?:og:title|twitter:title)" content=")[^"]*"/g, (_, attr) => `${attr}${title}"`)
    .replace('<!--head-->', () => extraHead + ogHead)
    .replace('<!--app-html-->', () => html)
}

function write(file, contents) {
  mkdirSync(dirname(file), { recursive: true })
  writeFileSync(file, contents)
}

for (const path of staticPaths) {
  const result = await render(path)
  if (result.notFound) throw new Error(`Prerender: ${path} resolved to the 404 page`)
  if (result.meta.canonical !== path) throw new Error(`Prerender: ${path} has canonical ${result.meta.canonical}`)
  if (!result.meta.description) throw new Error(`Prerender: ${path} has no description`)
  write(join(dist, path, 'index.html'), page(result))
}

write(join(dist, '404.html'), page(await render('/404/')))

write(
  join(dist, 'sitemap.xml'),
  `<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n` +
    staticPaths.map((p) => `  <url><loc>${absolute(p)}</loc></url>`).join('\n') +
    '\n</urlset>\n',
)
write(join(dist, 'robots.txt'), `User-agent: *\nAllow: /\n\nSitemap: ${absolute('/sitemap.xml')}\n`)
write(join(dist, '.nojekyll'), '')
if (cname) {
  if (!/^[a-z0-9.-]+\.[a-z]{2,}$/i.test(cname)) throw new Error(`SITE_CNAME is not a domain name: ${cname}`)
  write(join(dist, 'CNAME'), `${cname}\n`)
}
rmSync(ssrDir, { recursive: true, force: true })

console.log(`Prerendered ${staticPaths.length} routes + 404.html (base ${base}, site ${siteUrl})`)
