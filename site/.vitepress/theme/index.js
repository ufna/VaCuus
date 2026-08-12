// VaCuus theme: the DEFAULT VitePress theme, re-skinned, plus one landing component.
//
// /docs/* pages keep the stock layout on purpose — a buyer reading the docs wants the
// sidebar, the outline and the search that come with it. The brand reaches those pages
// through VitePress's own CSS variables (see style.css), not through a fork.
//
// `theme-without-fonts` is the same theme with its Inter @font-face block left out
// (vitepress/dist/client/theme-default/index.js is just fonts.css + this). The site
// self-hosts Archivo and IBM Plex Mono from public/fonts instead, so a page render makes
// no third-party request.
import DefaultTheme from 'vitepress/theme-without-fonts'
import Home from './Home.vue'
import './style.css'

export default {
  extends: DefaultTheme,
  enhanceApp({ app }) {
    // index.md is `layout: home` with no `hero`/`features` frontmatter, so VPHome renders
    // nothing of its own and this component IS the page.
    app.component('Home', Home)
  }
}
