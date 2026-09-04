// VaCuus theme: the DEFAULT VitePress theme, re-skinned, plus one landing component.
//
// /docs/* pages keep the stock layout on purpose — a buyer reading the docs wants the
// sidebar, the outline and the search that come with it. The brand reaches those pages
// through VitePress's own CSS variables (see style.css), not through a fork.
//
// `theme-without-fonts` is the same theme with its Inter @font-face block left out
// (vitepress/dist/client/theme-default/index.js is just fonts.css + this). The site
// self-hosts Archivo and IBM Plex Mono from public/fonts instead, so no font, style or
// image on the page is fetched from anyone else. The Yandex Metrika tag wired up in
// .vitepress/config.mjs is the page's one third-party request.
import { h, nextTick } from 'vue'
import { useData } from 'vitepress'
import DefaultTheme from 'vitepress/theme-without-fonts'
import Home from './Home.vue'
import LicenseBox from './LicenseBox.vue'
import PrivacyLine from './PrivacyLine.vue'
import { METRIKA_ID } from '../analytics.js'
import './style.css'

// The privacy line rides the default Layout's `layout-bottom` slot rather than
// themeConfig.footer: the stock footer hides itself on any page with a sidebar
// (vitepress/dist/client/theme-default/components/VPFooter.vue sets `has-sidebar` from
// useSidebar() and its own stylesheet makes `.VPFooter.has-sidebar { display: none }`),
// which is every /docs/* page, i.e. the pages a visitor spends the most time on. The slot
// renders on every page; the landing page is left out because Home.vue's own footer
// carries the same link, and two privacy lines on one screen reads as a mistake.
const Layout = {
  setup() {
    const { frontmatter } = useData()
    return () =>
      h(DefaultTheme.Layout, null, {
        'layout-bottom': () => (frontmatter.value.layout === 'home' ? null : h(PrivacyLine))
      })
  }
}

export default {
  extends: DefaultTheme,
  Layout,
  enhanceApp({ app, router }) {
    // index.md is `layout: home` with no `hero`/`features` frontmatter, so VPHome renders
    // nothing of its own and this component IS the page.
    app.component('Home', Home)

    // The Free | Commercial readout, used by license.md — a markdown page, so the component
    // has to be global for it. Home.vue's section 08 renders the same one, which is why the
    // price lives in commerce.js and not in either caller.
    app.component('LicenseBox', LicenseBox)

    // VitePress is an SPA: after the first document load nothing navigates again, so the
    // tag's init-time hit would be the only pageview ever recorded and every step through
    // the docs sidebar would be invisible. onAfterRouteChange fires on an internal link
    // click (vitepress/dist/client/app/router.js:30) and on Back/Forward (router.js:175),
    // but NOT on the initial hydration — so the landing page is counted once, by init,
    // and not twice.
    let referer = typeof window === 'undefined' ? '' : window.location.href
    router.onAfterRouteChange = () => {
      // The build renders each page by driving this very router — renderPage() awaits
      // router.go(href) in Node — so the hook fires once per page with no window at all.
      // Verified the hard way: without this line `npm run build` dies on "window is not
      // defined" in .vitepress/.temp/app.js while rendering pages.
      if (typeof window === 'undefined') return
      // pushState runs before the page module loads (router.js:27) and Back/Forward has
      // already moved the address bar, so location IS the new page by the time this fires.
      const url = window.location.href
      // document.title is assigned by a watchEffect on route.data (client/app/composables/
      // head.js:36,44). Awaiting this hook only schedules that job; it has not run. Metrika
      // defaults a hit's title to document.title, so without the tick every pageview would
      // be filed under the title of the page the visitor just left.
      nextTick(() => {
        // Undefined under `vitepress dev`, where transformHead does not run and no tag is
        // served. That is what keeps a local session out of the statistics.
        if (typeof window.ym !== 'function') return
        window.ym(METRIKA_ID, 'hit', url, { referer })
        referer = url
      })
    }
  }
}
