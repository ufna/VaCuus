import { defineConfig } from 'vitepress'

// Site root is site/, so index.md -> /, ai.md -> /ai, docs/*.md -> /docs/*.
// docs/*.md are copies synced from the repo's docs/buyer/ by scripts/sync-docs.mjs
// (wired to predev/prebuild) — never edit them here.

export default defineConfig({
  title: 'VaCuus',
  // First sentence of the descriptor a buyer reads in Edit -> Plugins (VaCuus.uplugin
  // "Description"); the storefront pitch and the site's meta description are the same claim.
  description:
    "Build your game's interface in HTML and CSS, rendered entirely off the game thread — " +
    'menus, HUDs and in-world panels that cost the game thread nothing but input.',

  cleanUrls: true,
  lastUpdated: true,

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
    nav: [
      { text: 'Get Started', link: '/docs/setup' },
      { text: 'Docs', link: '/docs/setup', activeMatch: '^/docs/' },
      { text: 'Built for AI', link: '/ai' }
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
