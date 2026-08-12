<script setup>
import { onMounted, onUnmounted, ref } from 'vue'

// The one-liner a buyer pastes into their own CLAUDE.md / AGENTS.md. Verbatim from
// README.md:35-37 — the site must not paraphrase an instruction the buyer will paste.
const AGENT_SNIPPET = `UI is VaCuus (HTML/CSS off the game thread). Before touching anything under
Content/DevUI, read Plugins/VaCuus/docs/buyer/ai-guide.md and
Plugins/VaCuus/docs/buyer/rcss-matrix.md.`

const copied = ref(false)
let copyTimer = null

async function copySnippet() {
  try {
    await navigator.clipboard.writeText(AGENT_SNIPPET)
    copied.value = true
    clearTimeout(copyTimer)
    copyTimer = setTimeout(() => (copied.value = false), 1600)
  } catch {
    // Clipboard is permission-gated and absent over plain http; the text is selectable
    // right there, so a failure needs no error UI.
  }
}

// Scroll reveal. IntersectionObserver only — no scroll listener, nothing per frame.
// Guarded to onMounted because this component is server-rendered at build time.
let observer = null
onMounted(() => {
  if (typeof IntersectionObserver === 'undefined') return
  if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) return
  // The hidden state is opt-in via this class: without JS (or when either guard
  // above returns) the CSS never hides anything and the page reads top to bottom.
  document.documentElement.classList.add('vc-motion')
  observer = new IntersectionObserver(
    (entries) => {
      for (const e of entries) {
        if (!e.isIntersecting) continue
        e.target.classList.add('is-in')
        observer.unobserve(e.target)
      }
    },
    { rootMargin: '0px 0px -8% 0px', threshold: 0.06 }
  )
  document.querySelectorAll('.vc-rise').forEach((el) => observer.observe(el))
})
onUnmounted(() => {
  observer?.disconnect()
  clearTimeout(copyTimer)
})
</script>

<template>
  <div class="vc-home">
    <!-- ======================================================== hero == -->
    <section class="vc-hero">
      <div class="vc-wrap">
        <div class="vc-hero-top vc-anim vc-anim-1">
          <span class="vc-eyebrow">Pre-release &middot; Unreal Engine 5.6 / 5.8</span>
          <span class="vc-dash" />
          <span class="vc-eyebrow">Full source included &middot; verified on Linux</span>
        </div>

        <h1 class="vc-wordmark vc-anim vc-anim-1">Va<span class="vc-w-a">C</span>uus</h1>

        <div class="vc-hero-grid">
          <div>
            <p class="vc-hero-claim vc-anim vc-anim-2">
              Your game's interface in HTML and CSS, rendered
              <em>entirely off the game thread</em>.
            </p>
            <div class="vc-chip vc-anim vc-anim-4">
              <b>0.012 ms</b>
              <span
                >game thread per frame at the 1,732-node reference worst case &mdash;
                budget 0.10 ms</span
              >
            </div>
          </div>

          <div>
            <p class="vc-hero-sub vc-anim vc-anim-3">
              You author screens as <code>.rml</code> / <code>.rcss</code> documents &mdash;
              the web languages, not a UMG graph &mdash; and the plugin runs them on its own
              UI thread: layout, styling and draw-command recording all happen away from the
              game thread, which only enqueues input and reads back a snapshot. Full-screen
              HUD, menu, or a panel on a quad in the world.
            </p>
            <div class="vc-actions vc-anim vc-anim-4">
              <a class="vc-btn vc-btn-primary" href="/docs/setup">Get started</a>
              <a class="vc-btn vc-btn-accent" href="/ai">Built for AI</a>
              <a
                class="vc-btn"
                href="https://github.com/ufna/VaCuus"
                target="_blank"
                rel="noreferrer"
              >
                <svg viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
                  <path
                    d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38
                       0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13
                       -.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66
                       .07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15
                       -.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82a7.4 7.4 0 0 1 2-.27c.68 0
                       1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82
                       1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01
                       1.93-.01 2.2 0 .21.15.46.55.38A8.01 8.01 0 0 0 16 8c0-4.42-3.58-8-8-8Z"
                  />
                </svg>
                GitHub
              </a>
            </div>
          </div>
        </div>

        <!-- Hero visual. Not lazy: this is the LCP element. -->
        <figure class="vc-shot vc-shot-hero vc-anim vc-anim-5">
          <span class="vc-ticks" aria-hidden="true"><i /><i /><i /><i /></span>
          <span class="vc-shot-frame">
            <img
              src="/shots/refhud.png"
              width="1920"
              height="1080"
              fetchpriority="high"
              decoding="async"
              alt="A dense combat HUD over a desert scene: two 24-row scoreboards, a
                   killfeed, floating damage numbers, a compass, a minimap, an ammo counter
                   and a settings panel."
            />
          </span>
          <figcaption class="vc-shot-bar">
            <span class="vc-tag">vacuus.RefHud</span>
            <span
              >the 1,732-node reference HUD the plugin ships, rendered headless by the
              plugin itself at 1920&times;1080</span
            >
          </figcaption>
        </figure>
      </div>
    </section>

    <!-- ====================================================== status == -->
    <div class="vc-wrap" style="padding-bottom: clamp(20px, 3vw, 34px)">
      <aside class="vc-status vc-rise">
        <div class="vc-status-hazard" aria-hidden="true" />
        <div class="vc-status-body">
          <div class="vc-status-head">
            <strong>Status &mdash; pre-release (beta)</strong>
            <a class="vc-cite" href="https://github.com/ufna/VaCuus#readme">README.md</a>
          </div>
          <p>
            <strong>UE 5.6 and UE 5.8</strong> are both built and tested from this one
            source tree &mdash; editor and packaged-game targets, plus the
            <strong>227-test automation suite</strong> that ships with the plugin and that
            you can run yourself, green on each. A package is stamped with the engine that
            built it, so take the download that matches yours. Verified on Linux;
            <strong>5.7 is untested</strong>.
          </p>
          <p>
            The API is <strong>not yet stable</strong> &mdash; expect renames between
            pre-release versions.
          </p>
        </div>
      </aside>
    </div>

    <!-- ===================================================== pillar 01 == -->
    <section class="vc-sec">
      <div class="vc-wrap">
        <header class="vc-sec-head vc-rise">
          <span class="vc-idx">01</span>
          <h2>Off the game thread, measured</h2>
          <span class="vc-rule" />
          <a class="vc-cite" href="/docs/perf-guide">docs/perf-guide</a>
        </header>

        <div class="vc-two vc-two-narrow">
          <div class="vc-rise">
            <p class="vc-lede">
              One UI thread owns every document: layout, styling and draw-command recording.
              The render thread replays the resulting command buffer into a persistent render
              target, and Slate composites that target every engine frame. What is left on the
              game thread is <strong>enqueueing input and reading one published snapshot</strong>.
            </p>
            <p class="vc-lede">
              Publication is withheld when a frame's content hash is unchanged and no resource
              traffic occurred, so a static UI stops costing anything to draw: the static M1 HUD
              published <strong>1 frame in 6,910</strong>, and the glass demo
              <strong>0 of 3,363</strong>. The reference HUD below publishes 100% of frames by
              design &mdash; it animates every one of them, which is why it is the worst case.
            </p>
            <p class="vc-lede">
              Every figure here is measured and re-runnable in your own project:
              <code>vacuus.RefHud</code> to load the scene, <code>vacuus.M1HUD.PerfLog 1</code>
              to print the timings and the publish ratio.
            </p>
          </div>

          <div class="vc-rise">
            <table class="vc-readout">
              <caption>
                Reference worst case &mdash; 1,732 live nodes, 908 draws/frame, 1080p
              </caption>
              <tbody>
                <tr>
                  <th scope="row">
                    Game thread / frame
                    <span class="vc-budget">budget 0.10 ms &middot; ~8&ndash;11&times;</span>
                  </th>
                  <td>
                    <b>0.012</b> ms dev<br />
                    0.008&ndash;0.009 ms shipping
                  </td>
                </tr>
                <tr>
                  <th scope="row">
                    Composite only, idle UI
                    <span class="vc-budget">budget 0.05 ms &middot; ~12&ndash;25&times;</span>
                  </th>
                  <td>
                    <b>0.002</b> ms shipping<br />
                    0.003 ms dev
                  </td>
                </tr>
                <tr>
                  <th scope="row">
                    UI thread, update + record
                    <span class="vc-budget">budget 1.2 ms steady avg</span>
                  </th>
                  <td>
                    <b>1.050</b> ms shipping<br />
                    1.077 ms dev
                  </td>
                </tr>
                <tr>
                  <th scope="row">
                    Added RAM
                    <span class="vc-budget">budget 32 MB &middot; A/B median</span>
                  </th>
                  <td><b>+14.3</b> MB</td>
                </tr>
                <tr>
                  <th scope="row">
                    Added disk, shipping
                    <span class="vc-budget">budget 10 MB &middot; Linux proxy</span>
                  </th>
                  <td><b>+3.22</b> MiB</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </section>

    <!-- ===================================================== pillar 02 == -->
    <section class="vc-sec">
      <div class="vc-wrap">
        <header class="vc-sec-head vc-rise">
          <span class="vc-idx">02</span>
          <h2>Plain text, live reload</h2>
          <span class="vc-rule" />
          <a class="vc-cite" href="/docs/setup">docs/setup</a>
        </header>

        <div class="vc-two">
          <div class="vc-rise">
            <p class="vc-lede">
              <code>.rml</code> is the markup, <code>.rcss</code> the styles, and both are
              plain files on disk. There is <strong>no editor asset to open and no graph to
              wire</strong> &mdash; your edit is complete when the file is saved, and it
              diffs, reviews and merges like the rest of your source.
            </p>
            <p class="vc-lede">
              Screens live under <code>Content/DevUI</code>: the plugin's own tree ships the
              demos, and your project's tree is an extension point beside it, not an
              override.
            </p>
          </div>
          <ol class="vc-steps vc-rise">
            <li>
              <strong>Save the file.</strong> Any editor. No import step, no reimport prompt,
              no asset registry entry.
            </li>
            <li>
              <strong>The watcher reloads the document in place</strong> while PIE keeps
              running &mdash; the editor watches <code>Content/DevUI</code>.
            </li>
            <li>
              <strong>At runtime, <code>vacuus.ReloadUI</code></strong> does the same thing
              from the console.
            </li>
            <li>
              <strong>No recompile.</strong> C++ is what hosts the document; it is not what
              the document is made of.
            </li>
          </ol>
        </div>
      </div>
    </section>

    <!-- ============================================ pillar 03 — accent == -->
    <section class="vc-sec vc-ai">
      <div class="vc-wrap">
        <header class="vc-sec-head vc-rise">
          <span class="vc-idx">03</span>
          <h2>Built for AI agents</h2>
          <span class="vc-rule" />
          <a class="vc-cite" href="/docs/ai-guide">docs/ai-guide</a>
        </header>

        <div class="vc-two">
          <div class="vc-rise">
            <p class="vc-lede">
              The entire UI surface is <strong>text an agent can read and write</strong>. That
              is the easy half. The hard half is that most authoring mistakes here produce no
              error and <strong>a screen that renders</strong> &mdash; just not the one you
              asked for. A human notices immediately; an agent, which cannot see the screen,
              does not.
            </p>
            <p class="vc-lede">
              So the package ships the other half too: a front door the agent finds on its own,
              a generated ground truth to check itself against, and a verification loop that
              works without eyes.
            </p>
          </div>

          <div class="vc-snippet vc-rise">
            <div class="vc-snippet-head">
              <span>One line in your project's CLAUDE.md / AGENTS.md</span>
              <button class="vc-copy" type="button" @click="copySnippet">
                {{ copied ? 'Copied' : 'Copy' }}
              </button>
            </div>
            <pre class="vc-code" v-text="AGENT_SNIPPET" />
          </div>
        </div>

        <div class="vc-ships vc-rise">
          <div class="vc-ship">
            <h3>AGENTS.md</h3>
            <p>
              At the package root, where an agent looks first. Short by design: the five
              things that would otherwise cost the session, and links to the real pages.
            </p>
          </div>
          <div class="vc-ship">
            <h3>ai-guide.md</h3>
            <p>
              The full brief. Which mistakes warn, which are drawn wrong in silence, and how
              to tell the two apart.
            </p>
          </div>
          <div class="vc-ship">
            <h3>rcss-matrix.md</h3>
            <p>
              99 properties, 20 shorthands, 16 decorators &mdash; parsed out of the exact
              RmlUi vendored in the package, not out of a model's training data. If a property
              is not in that file, it does not exist.
            </p>
          </div>
          <div class="vc-ship">
            <h3>The verification loop</h3>
            <p>
              A declaration RmlUi cannot parse logs a warning naming <code>file:line</code>,
              in every configuration <strong>including Shipping</strong>. Plus a headless
              1920&times;1080 render recipe and the 227-test suite, so an agent can check its
              own work.
            </p>
          </div>
        </div>

        <div class="vc-actions vc-rise">
          <a class="vc-btn vc-btn-accent" href="/ai">Read the full page &rarr;</a>
        </div>
      </div>
    </section>

    <!-- ===================================================== pillar 04 == -->
    <section class="vc-sec">
      <div class="vc-wrap">
        <header class="vc-sec-head vc-rise">
          <span class="vc-idx">04</span>
          <h2>Engine-native data</h2>
          <span class="vc-rule" />
          <a class="vc-cite" href="/docs/setup">docs/setup</a>
        </header>

        <div class="vc-two">
          <div class="vc-rise">
            <p class="vc-lede">
              Data models bind
              <strong>straight from your <code>UPROPERTY</code> fields</strong>. The plugin
              reads them through the engine's own reflection &mdash; no mirror
              struct to keep in sync, no serialisation step between your game state and the
              document.
            </p>
            <p class="vc-lede">
              JavaScript is optional, not assumed. QuickJS-ng is vendored in-tree for the
              projects that want scripting, and a TypeScript/Preact workflow sits on top of
              it when you want that too.
            </p>
          </div>
          <div class="vc-rise">
            <ol class="vc-steps">
              <li>
                <strong>UVaCuusWidget</strong> &mdash; "VaCuus View" in the UMG palette. Screen
                space: HUD, menu, overlay. Drop it into any UMG tree and point it at a
                document.
              </li>
              <li>
                <strong>UVaCuusWorldComponent</strong> &mdash; "VaCuus World Panel". A pixel
                panel on a quad in the world.
              </li>
              <li>
                <strong>Both work from Blueprint and from C++.</strong> Either one hands you a
                view, and the view is what you feed and drive.
              </li>
            </ol>
          </div>
        </div>
      </div>
    </section>

    <!-- ====================================================== gallery == -->
    <section class="vc-sec">
      <div class="vc-wrap">
        <header class="vc-sec-head vc-rise">
          <span class="vc-idx">05</span>
          <h2>Rendered by the plugin</h2>
          <span class="vc-rule" />
          <span class="vc-cite">headless, 1920&times;1080</span>
        </header>

        <div class="vc-gallery">
          <figure class="vc-shot vc-rise">
            <span class="vc-ticks" aria-hidden="true"><i /><i /><i /><i /></span>
            <span class="vc-shot-frame">
              <img
                src="/shots/deco.png"
                width="1920"
                height="1080"
                loading="lazy"
                decoding="async"
                alt="Six panels demonstrating decorators: a red-to-blue linear gradient,
                     gold and black hazard stripes, a radial gradient, a conic hue wheel, a
                     translucent glass shader panel, and a plain background-colour control."
              />
            </span>
            <figcaption class="vc-shot-bar">
              <span class="vc-tag">vacuus.M5Deco</span>
              <span
                >decorators: linear, repeating-linear, radial, conic, and a built-in shader
                &mdash; each beside a no-decorator control</span
              >
            </figcaption>
          </figure>

          <figure class="vc-shot vc-rise">
            <span class="vc-ticks" aria-hidden="true"><i /><i /><i /><i /></span>
            <span class="vc-shot-frame">
              <img
                src="/shots/glass.png"
                width="1920"
                height="1080"
                loading="lazy"
                decoding="async"
                alt="Three translucent panels over desert dunes; the first two blur the
                     scene behind them, the third does not."
              />
            </span>
            <figcaption class="vc-shot-bar">
              <span class="vc-tag">vacuus.M5Glass</span>
              <span
                >backdrop blur over the live scene, rounded and square, with an unblurred
                control panel for comparison</span
              >
            </figcaption>
          </figure>

          <figure class="vc-shot vc-shot-wide vc-rise">
            <span class="vc-ticks" aria-hidden="true"><i /><i /><i /><i /></span>
            <span class="vc-shot-frame">
              <img
                src="/shots/m2demo.png"
                width="1920"
                height="1080"
                loading="lazy"
                decoding="async"
                alt="Four interaction panels: hover and focus buttons, a scrollable
                     inventory list, a text input with tab order, and a click-through
                     pass-through region."
              />
            </span>
            <figcaption class="vc-shot-bar">
              <span class="vc-tag">vacuus.M2Demo</span>
              <span
                >the interaction demo that ships in the package: buttons with
                <code>:hover</code>/<code>:active</code>/<code>:focus</code>, a wheel-scroll
                list, a text field with tab order, and a region that lets clicks reach the
                game</span
              >
            </figcaption>
          </figure>
        </div>
      </div>
    </section>

    <!-- ============================================= why not a browser == -->
    <!-- The competitive question, and the one section that names a competitor class.
         Every browser-side figure comes from docs/research/2026-07-29-webui-middleware.md
         (§3 candidate table and head-to-head, §4 "anti-pattern found in Epic's own code");
         every VaCuus-side figure from docs/buyer/perf-guide.md's budget table. That
         research is repo-internal, so its cite is text, not a link. -->
    <section id="why-not-a-browser" class="vc-sec vc-why">
      <div class="vc-wrap">
        <header class="vc-sec-head vc-rise">
          <span class="vc-idx">06</span>
          <h2>Why not a browser?</h2>
          <span class="vc-rule" />
          <span class="vc-cite">middleware research &middot; 2026-07</span>
        </header>

        <p class="vc-lede vc-why-lede vc-rise">
          Every other web-UI plugin for Unreal on Fab is a Chromium wrapper &mdash; CEF under
          one name or another &mdash; so this is the comparison that decides the purchase.
          Here is what a browser costs inside a game, beside what this costs, with both
          columns measured.
        </p>

        <div class="vc-vs vc-rise">
          <div class="vc-vs-col vc-vs-them">
            <span class="vc-ticks" aria-hidden="true"><i /><i /><i /><i /></span>
            <h3>A browser in your game</h3>
            <table class="vc-readout">
              <caption>
                CEF 128/147 &mdash; measured in the plugin's middleware research, 2026-07
              </caption>
              <tbody>
                <tr>
                  <th scope="row">Added to the build</th>
                  <td><b>330&ndash;390</b> MB<br />shipped payload</td>
                </tr>
                <tr>
                  <th scope="row">Runtime RAM</th>
                  <td><b>150&ndash;300+</b> MB</td>
                </tr>
                <tr>
                  <th scope="row">Processes</th>
                  <td><b>4&ndash;5</b><br />separate, plus their IPC</td>
                </tr>
                <tr>
                  <th scope="row">Input path</th>
                  <td><b>+1&ndash;2</b> frames<br />across the process boundary</td>
                </tr>
                <tr>
                  <th scope="row">Game thread</th>
                  <td>
                    pumps the browser's<br />message loop
                    <code>CefDoMessageLoopWork</code>
                  </td>
                </tr>
                <tr>
                  <th scope="row">Upkeep</th>
                  <td>a <b>two-week</b><br />Chromium security cycle</td>
                </tr>
                <tr>
                  <th scope="row">HTML/CSS core on disk</th>
                  <td><b>256</b> MB<br />Chrome-class, stripped</td>
                </tr>
              </tbody>
            </table>
            <p class="vc-vs-foot">
              And in UE specifically: the message-loop pump above was found in Epic's own
              integration, and accelerated off-screen rendering on Linux/NVIDIA is broken.
            </p>
          </div>

          <div class="vc-vs-col vc-vs-us">
            <span class="vc-ticks" aria-hidden="true"><i /><i /><i /><i /></span>
            <h3>VaCuus, measured</h3>
            <table class="vc-readout">
              <caption>
                the shipped reference workload &mdash;
                <code>vacuus.RefHud</code>, 1,732 live nodes
              </caption>
              <tbody>
                <tr>
                  <th scope="row">Added to the build</th>
                  <td><b>+3.22</b> MiB<br />Linux Shipping proxy</td>
                </tr>
                <tr>
                  <th scope="row">Runtime RAM</th>
                  <td><b>+14.3</b> MB<br />A/B median</td>
                </tr>
                <tr>
                  <th scope="row">Processes</th>
                  <td><b>0</b> extra<br />in-process, one UI thread</td>
                </tr>
                <tr>
                  <th scope="row">Input path</th>
                  <td>answered in-process<br />from the frame's snapshot</td>
                </tr>
                <tr>
                  <th scope="row">Game thread</th>
                  <td><b>0.012</b> ms / frame<br />enqueue input, read snapshot</td>
                </tr>
                <tr>
                  <th scope="row">Upkeep</th>
                  <td>no Chromium<br />in your build</td>
                </tr>
                <tr>
                  <th scope="row">HTML/CSS core on disk</th>
                  <td><b>3.8</b> MB<br />the vendored RmlUi library</td>
                </tr>
              </tbody>
            </table>
            <p class="vc-vs-foot">
              Disk, RAM and game-thread rows are the plugin's own
              <a href="/docs/perf-guide">budget table</a>, re-runnable in your project:
              <code>vacuus.RefHud</code> then <code>vacuus.M1HUD.PerfLog 1</code>. The
              game-thread figure is the dev build; cooked Shipping measures 0.008&ndash;0.009
              ms.
            </p>
          </div>
        </div>

        <div class="vc-two vc-why-two">
          <div class="vc-rise">
            <h3 class="vc-why-h">The part you need survives the cut</h3>
            <p class="vc-lede">
              Dropping the browser is not dropping motion. RCSS has
              <strong>transitions and <code>@keyframes</code></strong>, decorators &mdash;
              linear, repeating, radial and conic gradients, plus a built-in shader &mdash;
              <strong>backdrop blur over the live 3D scene</strong>, and updates driven
              straight from your <code>UPROPERTY</code> fields, all inside a deterministic
              in-process core.
            </p>
            <p class="vc-lede">
              The screenshots directly above this section are not mockups and not a
              competitor's demo reel: they are this pipeline rendering itself, headless, at
              1920&times;1080.
            </p>
          </div>

          <div class="vc-why-ai vc-rise">
            <h3 class="vc-why-h">A bounded surface is the AI feature</h3>
            <p>
              An agent can only build a UI it can hold in its head. The supported language
              here is small, closed and <strong>generated</strong> &mdash; 99 properties, 20
              shorthands, 16 decorators, parsed out of the exact RmlUi vendored in your
              package &mdash; so the whole surface fits in an agent's context as ground
              truth. A mistake surfaces as a log warning naming <code>file:line</code>, in
              every configuration including Shipping, instead of a devtools session nobody is
              sitting in front of; a screen verifies with one headless render.
            </p>
            <p>
              That is why an agent can build flashy, animated UI <em>here</em>: the surface is
              bounded and instrumented. Against a full browser it is guessing across the whole
              web platform, and it cannot see the screen it guessed wrong on.
            </p>
            <a class="vc-btn vc-btn-accent" href="/ai">How that works &rarr;</a>
          </div>
        </div>

        <aside class="vc-why-honest vc-rise">
          <strong>The counterpoint, stated</strong>
          <p>
            If what you need is <em>the actual web</em> &mdash; arbitrary sites, embedded
            video, WebGL, a page somebody else controls &mdash; then a browser is the right
            tool and this plugin is not one. VaCuus is for game UI you author yourself.
          </p>
        </aside>
      </div>
    </section>

    <!-- ====================================================== numbers == -->
    <section class="vc-numbers vc-rise">
      <div class="vc-num">
        <b>227</b>
        <em>automation tests, shipped with the plugin</em>
        <small>Automation RunTests VaCuus</small>
      </div>
      <div class="vc-num">
        <b>99<i> / 20 / 16</i></b>
        <em>RCSS properties, shorthands, decorators</em>
        <small>generated from the vendored RmlUi</small>
      </div>
      <div class="vc-num">
        <b>0.012<i> ms</i></b>
        <em>game thread per frame at 1,732 nodes</em>
        <small>budget 0.10 ms &middot; dev build</small>
      </div>
      <div class="vc-num">
        <b>5.6<i> &middot; </i>5.8</b>
        <em>engines, built and tested from one tree</em>
        <small>5.7 untested &middot; verified on Linux</small>
      </div>
    </section>

    <!-- ========================================================== not == -->
    <section class="vc-sec">
      <div class="vc-wrap">
        <header class="vc-sec-head vc-rise">
          <span class="vc-idx">07</span>
          <h2>What it is not</h2>
          <span class="vc-rule" />
          <a class="vc-cite" href="/docs/rcss-matrix">docs/rcss-matrix</a>
        </header>

        <div class="vc-not vc-rise">
          <div class="vc-not-item">
            <h3>Not a browser engine</h3>
            <p>
              No <code>&lt;a href&gt;</code> navigation, no CSS Grid, no <code>fetch</code>, no
              DOM library you remember. Navigation is the host calling
              <code>LoadDocument</code>; layout is flexbox and absolute positioning.
            </p>
          </div>
          <div class="vc-not-item">
            <h3>RCSS is not CSS</h3>
            <p>
              It is RmlUi's style language: a deliberate subset with its own additions. The
              generated matrix is the exact difference, keyed to the engine in your package
              &mdash; read it before you write a property.
            </p>
          </div>
          <div class="vc-not-item">
            <h3>Not every property is drawn</h3>
            <p>
              A few parse and are then not rendered &mdash; <code>box-shadow</code> is the
              headline case, and it warns per view and names a substitute. The matrix marks
              them; nothing here is a surprise you find at ship time.
            </p>
          </div>
          <div class="vc-not-item">
            <h3>Not a project-folder install</h3>
            <p>
              A Fab install lands <strong>in the engine</strong>, with binaries Epic built
              &mdash; a Blueprint-only project can enable it and go. The full source ships
              regardless; drop it into <code>&lt;Project&gt;/Plugins</code> instead and it
              builds like any C++ plugin (that route does need a C++ project).
            </p>
          </div>
        </div>
      </div>
    </section>

    <!-- ======================================================= footer == -->
    <footer class="vc-foot">
      <div class="vc-wrap vc-foot-grid">
        <div>
          <div class="vc-foot-mark">VaCuus</div>
          <p style="margin-top: 8px">
            HTML/CSS user interface for Unreal Engine, rendered off the game thread.
          </p>
        </div>
        <p>
          Issues and questions:
          <a href="https://github.com/ufna/VaCuus/issues" target="_blank" rel="noreferrer"
            >github.com/ufna/VaCuus/issues</a
          >
        </p>
        <p>
          MIT-licensed <strong>RmlUi 6.x</strong> and <strong>QuickJS-ng</strong> are vendored
          in-tree, each with its license beside it.
        </p>
      </div>
    </footer>
  </div>
</template>
