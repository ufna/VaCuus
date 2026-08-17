<script setup>
// The Free | Commercial readout. A component rather than markup in Home.vue because it is
// read twice: closing the landing page as section 08, and opening /license. Every figure and
// URL in it comes from commerce.js, so the price exists once in the source.
//
// It renders in two different CSS contexts — the landing (markdownStyles: false, no prose
// wrapper) and /license (inside .vp-doc, which styles ul/li/a/h3 of its own accord). That is
// why every rule in style.css under .vc-lic is two classes deep: .vp-doc's selectors are
// (0,1,1) and would otherwise win.
import { PRICE, PRICE_UNIT, DOWNLOAD_URL, CONTACT_EMAIL, FAB_URL, buyHref } from './commerce.js'

const buy = buyHref()
</script>

<template>
  <div class="vc-lic">
    <!-- ======================================================== free == -->
    <section class="vc-lic-col" aria-label="Free use">
      <span class="vc-ticks" aria-hidden="true"><i /><i /><i /><i /></span>
      <h3 class="vc-lic-h">Free</h3>
      <p class="vc-lic-fig">
        <b>$0</b>
        <span>no signup, no licence key</span>
      </p>

      <p class="vc-lic-when">
        Everything short of a commercial release, with no time limit and nothing withheld.
      </p>

      <ul class="vc-lic-list">
        <li>Read, fork and modify the source</li>
        <li>Prototypes, evaluation, internal tools, game jams</li>
        <li>Ship a free, hobby or academic game</li>
        <li>Full source, all three engines, the 227-test suite</li>
      </ul>

      <div class="vc-lic-cta">
        <a class="vc-btn" :href="DOWNLOAD_URL" target="_blank" rel="noreferrer">
          Download &mdash; free &rarr;
        </a>
      </div>

      <p class="vc-lic-foot">
        GitHub Releases &mdash; one archive per engine version, with
        <code>SHA256SUMS.txt</code> beside them.
      </p>
      <p class="vc-lic-foot">
        <strong>Not a trial.</strong> It is the same plugin as the paid one, and it does not
        stop working.
      </p>
    </section>

    <!-- ================================================== commercial == -->
    <section class="vc-lic-col vc-lic-buy" aria-label="Commercial licence">
      <span class="vc-ticks" aria-hidden="true"><i /><i /><i /><i /></span>
      <h3 class="vc-lic-h">Commercial</h3>
      <p class="vc-lic-fig">
        <b>{{ PRICE }}</b>
        <span>{{ PRICE_UNIT }}</span>
      </p>

      <p class="vc-lic-when">
        Needed when you ship or operate a product for commercial advantage or monetary
        compensation.
      </p>

      <ul class="vc-lic-list">
        <li>One title, any team size</li>
        <li>Perpetual &mdash; it covers the release you ship, and every 1.x update</li>
        <li>Direct from the author; invoice and custom terms on request</li>
      </ul>

      <div class="vc-lic-cta">
        <a class="vc-btn vc-btn-primary" :href="buy">Buy a licence &rarr;</a>
        <!-- Disabled, not hidden: the Fab route is real and its listing is in technical
             review, so it is stated and marked. `disabled` on a real <button> is what keeps
             it out of the tab order and unclickable — a styled <a> with no href would still
             be announced as a link to a screen reader. A non-null FAB_URL swaps in the
             live link below with no other change. -->
        <button v-if="!FAB_URL" class="vc-btn vc-btn-soon" type="button" disabled>
          Fab &middot; <span class="vc-lic-soon">soon</span>
        </button>
        <a v-else class="vc-btn" :href="FAB_URL" target="_blank" rel="noreferrer">
          Buy on Fab &rarr;
        </a>
      </div>

      <p class="vc-lic-foot">
        Consoles, source escrow or purchase-order terms &mdash;
        <a :href="`mailto:${CONTACT_EMAIL}`">{{ CONTACT_EMAIL }}</a>
      </p>
      <p class="vc-lic-foot">
        <strong>Nothing to activate.</strong> There is no licence key and no check in the
        plugin &mdash; the copy you develop with is the copy you ship.
      </p>
    </section>
  </div>
</template>
