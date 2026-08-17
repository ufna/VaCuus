import { defineConfig } from 'vitepress'
import { METRIKA_ID } from './analytics.js'

// Site root is site/, so index.md -> /, ai.md -> /ai, license.md -> /license,
// docs/*.md -> /docs/*.
// docs/*.md are copies synced from the repo's docs/buyer/ by scripts/sync-docs.mjs
// (wired to predev/prebuild) — never edit them here.

// The Yandex Metrika tag as metrika.yandex.ru generates it, with the counter id
// interpolated in its two places and one deliberate change: `webvisor` is OFF. Session
// Replay records the visitor's whole session — pointer, scroll, clicks, form input — and
// that is a weight of consent a documentation site has no reason to carry; a pageview
// counter and a click map do not. Written as an explicit `false` rather than a deleted key,
// though false is the default, so that a regenerated snippet pasted over this one shows the
// difference in the diff instead of restoring recording silently. The counter's own
// Settings -> Tag checkbox is the other half of the same switch and has to match.
//
// `ecommerce:"dataLayer"` is part of the generated snippet — this site pushes no such
// array, so the tag watches for one that never appears.
//
// VitePress runs an inline <script> body through esbuild's minifier and inserts it
// unescaped — renderHead() in vitepress/dist/node/chunk-*.js, which escapes attributes but
// not children — so the `<` in the loop condition survives and the shipped tag is minified.
const METRIKA_TAG = `
(function(m,e,t,r,i,k,a){
    m[i]=m[i]||function(){(m[i].a=m[i].a||[]).push(arguments)};
    m[i].l=1*new Date();
    for (var j = 0; j < document.scripts.length; j++) {if (document.scripts[j].src === r) { return; }}
    k=e.createElement(t),a=e.getElementsByTagName(t)[0],k.async=1,k.src=r,a.parentNode.insertBefore(k,a)
})(window, document,'script','https://mc.yandex.ru/metrika/tag.js?id=${METRIKA_ID}', 'ym');

ym(${METRIKA_ID}, 'init', {ssr:true, webvisor:false, clickmap:true, ecommerce:"dataLayer", referrer: document.referrer, url: location.href, accurateTrackBounce:true, trackLinks:true});
`

export default defineConfig({
  title: 'VaCuus',
  // First sentence of the descriptor a buyer reads in Edit -> Plugins (VaCuus.uplugin
  // "Description"); the storefront pitch and the site's meta description are the same claim.
  description:
    "Build your game's interface in HTML and CSS, rendered entirely off the game thread — " +
    'menus, HUDs and in-world panels that cost the game thread nothing but input.',

  cleanUrls: true,
  lastUpdated: true,

  // Favicon (rendered from the wordmark in Archivo; sources regenerable per docs/fab/src
  // pattern) and the social preview card. og:image must be an ABSOLUTE url — scrapers do
  // not resolve relative paths; og.jpg is the Fab thumbnail at 1200×630.
  head: [
    ['link', { rel: 'icon', type: 'image/png', sizes: '32x32', href: '/favicon-32.png' }],
    ['link', { rel: 'icon', type: 'image/png', sizes: '512x512', href: '/favicon-512.png' }],
    ['link', { rel: 'apple-touch-icon', href: '/apple-touch-icon.png' }],
    ['meta', { property: 'og:type', content: 'website' }],
    ['meta', { property: 'og:site_name', content: 'VaCuus' }],
    ['meta', { property: 'og:title', content: 'VaCuus — Game UI in HTML/CSS, off the game thread' }],
    [
      'meta',
      {
        property: 'og:description',
        content:
          "Build your game's interface in HTML and CSS, rendered entirely off the game " +
          'thread. Plain-text screens, live reload, UPROPERTY data binding — built for AI agents.'
      }
    ],
    ['meta', { property: 'og:url', content: 'https://vacuus.ufna.dev/' }],
    ['meta', { property: 'og:image', content: 'https://vacuus.ufna.dev/og.jpg' }],
    ['meta', { property: 'og:image:width', content: '1200' }],
    ['meta', { property: 'og:image:height', content: '630' }],
    ['meta', { name: 'twitter:card', content: 'summary_large_image' }],
    ['meta', { name: 'twitter:image', content: 'https://vacuus.ufna.dev/og.jpg' }],
    // Site-ownership verification for Heleket. It must be served on every page of the
    // origin, so it belongs in the global head, not on one page.
    ['meta', { name: 'heleket', content: '4aea48a5' }]
  ],

  // The counter is injected here rather than appended to `head` above because transformHead
  // runs only in the SSG render pass — `vitepress build`, never `vitepress dev`. A local dev
  // session therefore serves no tag at all and localhost cannot land in the statistics.
  transformHead: () => [['script', {}, METRIKA_TAG]],

  // The snippet's no-JS pixel, which cannot ride along in the head: the HTML parser's "in
  // head noscript" insertion mode admits only link/style/meta, so an <img> there is a parse
  // error that ejects the element into the body regardless. Put it in the body to begin
  // with. It lands after #app — the element Vue hydrates — so hydration never sees it.
  transformHtml: (code) =>
    code.replace(
      '</body>',
      `<noscript><div><img src="https://mc.yandex.ru/watch/${METRIKA_ID}" style="position:absolute; left:-9999px;" alt="" /></div></noscript></body>`
    ),

  markdown: {
    // VitePress marks fenced blocks `v-pre` but not INLINE code, so Vue's template
    // compiler interpolates `{{ ... }}` inside a `<code>` span. localization.md documents
    // VaCuus's own `{{ t.key }}` syntax in prose — `{{ t.2p_mode }}` is not valid
    // JavaScript, and the build fails on it rather than mis-rendering. Marking every
    // inline span v-pre keeps the source text verbatim; nothing here wants interpolation.
    config: (md) => {
      md.renderer.rules.code_inline = (tokens, idx) =>
        `<code v-pre>${md.utils.escapeHtml(tokens[idx].content)}</code>`

      // ...and an inline span may WRAP a line: ai-guide.md line 114 opens a code span that
      // closes on line 115, which begins `<file>: <line>`. This renderer's html_block rule
      // lets an unknown tag at the start of a continuation line terminate the paragraph
      // (CommonMark type 7 may not), so the span never forms and the raw `<file>` reaches
      // Vue as an unclosed element. Refusing to terminate a paragraph is the narrow fix; an
      // HTML block that starts after a blank line — the only kind these pages use, e.g.
      // localization.md's `<br>` cells — still parses exactly as before.
      const htmlBlock = md.block.ruler.__rules__.find((r) => r.name === 'html_block').fn
      md.block.ruler.at('html_block', (state, startLine, endLine, silent) =>
        silent ? false : htmlBlock(state, startLine, endLine, silent)
      )
    }
  },

  // Apex domain (vacuus.ufna.dev via public/CNAME), so the site is served from the root.
  base: '/',

  themeConfig: {
    // License is last, next to search and the GitHub icon: it is the commercial entry the
    // landing page's section 08 closes on, and the one thing a visitor may want to reach
    // without scrolling a long page to find it.
    nav: [
      { text: 'Get Started', link: '/docs/setup' },
      { text: 'Docs', link: '/docs/setup', activeMatch: '^/docs/' },
      { text: 'Built for AI', link: '/ai' },
      { text: 'License', link: '/license' }
    ],

    sidebar: {
      '/docs/': [
        {
          // README.md "Read these, in this order" — the order is the page order.
          text: 'Documentation',
          items: [
            { text: 'Setup', link: '/docs/setup' },
            { text: 'Gotchas', link: '/docs/gotchas' },
            { text: 'RCSS Matrix', link: '/docs/rcss-matrix' },
            { text: 'Performance Guide', link: '/docs/perf-guide' },
            { text: 'AI Guide', link: '/docs/ai-guide' },
            { text: 'Localization', link: '/docs/localization' }
          ]
        }
      ]
    },

    socialLinks: [{ icon: 'github', link: 'https://github.com/ufna/VaCuus' }],

    search: { provider: 'local' },

    outline: { level: [2, 3] }
  }
})
