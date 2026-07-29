# VaCuus — Web UI Middleware & Architecture Research (2026-07-29)

> **DECISION (2026-07-29, ufna): Variant A accepted.** Core = RmlUi 6.x (MIT, vendored
> fork with upstream tracking) + QuickJS-ng; custom RenderInterface over UE RHI
> (Noesis-style direct backend); dedicated UI thread owning RmlUi + JS with
> double-buffered command handoff — no end-of-tick fence. Differentiators to build:
> shader decorators mapped to UE Materials, scene-color backdrop-filter (glass over the
> 3D world). Decision informed by the live HUD demo (docs/research/hud-demo: 0.07 ms
> update / ~1 ms render, 657 LOC bindings) vs Servo live tests (real web-DX but ~700 MB
> PSS + gap matrix). Servo remains a deferred "browser-grade tier" option (VaCuus-akj.3).

Goal: pick the HTML render core and UE integration architecture for a Gameface-class,
ultra-optimized, multithreaded web UI plugin for UE on PC (Win/Linux/Mac), sellable on Fab.

Method: 12 research agents (web primary sources + local UE 5.8.1 source audit + **local
benchmarks on this machine**: Ryzen 9 7950X3D, RTX 3090 Ti, NVIDIA 610.43.03, Arch Linux).
Bench artifacts: RmlUi master built & profiled (nanobench), servoshell v0.3.0 profiled,
Chrome 150 headless PSS measured — identical animated game-HUD workload (1750 elements,
64 rAF minimap blips, keyframe buff icons, 24-row scoreboard, killfeed).

---

## 1. The benchmark to beat: Coherent Gameface

- **Stack**: Cohtml (proprietary HTML/CSS/JS engine, *not* WebKit/Chromium) + Renoir
  (command-recording GPU renderer). In UE, Renoir is implemented **over UE RHI**
  (`FCohRenoirBackend` in CoherentRenderingPlugin) — UI textures are normal engine resources.
- **Threading**: `View::Advance` (DOM+JS) on a caller-chosen "UI thread"; layout/resource
  tasks pumped by the *client* via `Library::ExecuteWork` (SDK never spawns threads);
  `ViewRenderer::Paint` on render thread replays async-recorded command buffers with
  dirty-region incremental rendering.
- **UE weakness (our wedge)**: `Advance` ticks on the **game thread** by default
  (TG_StartPhysics). Opt-in "Concurrent Advance" (UE 5.1+) moves it to the Task System but
  still **fences the game thread at end of World::Tick**, serializes multiple views, and
  forbids input/model-sync/resize during the concurrent window.
- **Data binding**: `data-bind-*` attributes + C++ models; `UpdateWholeModel` (dirty mark) →
  `SynchronizeModels` (one batched DOM sync per frame). UE: `CreateDataModelFromStruct/Object`.
- **UE components**: `UCohtmlHUD`/`ACohtmlGameHUD`, `UCohtmlComponent` (in-world + raycast
  input), `UCohtmlWidget` (UMG), `ACohtmlInputActor` + full-screen `SCohtmlInputForward`
  (steals input — known pain). Gamepad = HTML5 Gamepad API polling; spatial nav = JS library.
- **Known pains**: flex-only (NO CSS Grid), silent CSS ignore, TAA blurs UI (they recommend
  FXAA), WebM/VP8/VP9-only video, UE 5.6 Linux Vulkan deadlock workarounds, full-screen
  input-forward widget. DevTools = CDP on port 9444.
- **License**: per-title, per-platform, custom quotes, no public prices; NOT redistributable
  → competitor/benchmark, not a component. v3.1.1 (2026-07-17) supports UE 5.8.
- Ships: Borderlands 4, Spider-Man 2, Civ 7, Alan Wake 2, Minecraft, Sea of Thieves,
  Cities: Skylines 2 (React+TS).

## 2. Market (verified)

- The slot "Gameface-class web UI, $50–$250, on Fab, Win/Linux/Mac" is **empty**. Every Fab
  offering is a CEF WebView wrapper: Tracer Web UI ($49.99/$149.99, 26% 1-star share),
  YetiTech ($99.99/$199.99), UCefView (solo dev), "CEF 126+" ($299.99/$399.99), BLUI (free).
  None has off-thread layout, data binding, gamepad nav, or low memory.
- Demand documented: decade of UE CEF staleness (Chromium 59→90→128 only in 5.7),
  15fps-widget threads, official React-support feature requests.
- Fab economics: $24M/yr total creator earnings, sweet spot $25–60, observed ceiling
  $200–400, 88% rev share, Personal/Professional dual tiers native.
- Risks: Epic modernizing CEF (commoditizes WebView low-end only); Noesis moved down-market
  (€195 indie); no sign of Coherent doing so.

### Fab seller-side rules (primary-source verified, Mar 2026 texts)

- Prebuilt third-party binaries **allowed** (rule 4.3.6.1.a) if they don't reference UE
  source; **your UE-facing C++ modules ship as source** to every buyer, Fab Standard License
  only (custom EULAs void, per-seat).
- **`.exe`/`.msi` ban** (4.3.6.1.e) — kills CEF's Windows helper exe (competitors ship
  renamed-`.bin` hacks). In-process engines avoid this entirely.
- GPL/LGPL banned (CEF's static FFmpeg = gray zone); **MIT/BSD/MPL-2.0 clean**.
- Size is a non-issue (Epic pulls a zip from seller hosting; ~15GB soft cap).

## 3. Candidates

| Core | License/Fab | Footprint (measured/verified) | Threading | Coverage | Verdict |
|---|---|---|---|---|---|
| **RmlUi 6.2** + QuickJS-ng | MIT + MIT ✅ | **3.8 MB lib, ~20 MB RSS**; Update 19–21 µs, cmd-gen ~150 µs @1750 elems (measured) | No internal threads, thread-agnostic → own the whole engine on one UI thread | RCSS: flex (no Grid), animations, filters, box-shadow, data binding, spatial nav, touch; no JS (add QuickJS-ng), no BiDi (HarfBuzz sample exists) | **Recommended core** |
| **Servo** (0.1 LTS Apr 2026) | MPL-2.0 ✅ | 153 MB PSS / ~350–400 MB RSS, 80–126 MB binary (measured); layout 0.57 ms + paint ~0.5 ms *on its own threads* | Runs off-main (Slint precedent), parallel Stylo/layout, RefreshDriver frame pacing | ~62% WPT; real CSS/JS/React; SpiderMonkey (GC non-incremental by default) | Browser-grade tier option; too heavy for "ultra-light" claim |
| **CEF 128/147** | BSD-3 ✅ (FFmpeg gray) | 330–390 MB payload, 150–300+ MB RAM, 4–5 processes, +1–2 frames input latency | Web work in other processes; UE pumps browser process **on game thread** (fixable) | Full web | Disqualified by premise; Linux accel OSR broken on NVIDIA; maintenance treadmill (2-week Chromium cycle from Sep 2026) |
| Ultralight 1.4 | ❌ no plugin-resale right; bus factor 1 (Awesomium history); 1.4.1 15 mo late | "10x lighter than Chromium" (vendor) | Single-thread-affine, off-main OK | Safari 16.4-era WebKit; no video/WebGL/IME/Gamepad; no Vulkan GPUDriver | Design reference only |
| Blitz (Dioxus) | MIT/Apache ✅ | small, wgpu-texture-native | — | pre-alpha, **no JS by design** | Watch for future "lite" mode |
| WPE WebKit | — | — | — | Linux-only (Windows port years away) | No |
| Sciter | closed, bus factor 1, resale unconfirmed | — | — | — | No |
| Native webviews (WebView2/WKWebView/WebKitGTK) | — | — | — | **No render-to-texture** (verified open issues) | No |
| NoesisGUI | per-project €195–€25K, competitor | — | View thread-affine + render-thread RenderDevice **snapshot handoff** | XAML | Architecture blueprint (open UE plugin source) |

### Measured head-to-head (same machine, same HUD workload)

| | RmlUi | Servo (servoshell) | Chrome 150 (CEF-class) |
|---|---|---|---|
| Per-frame UI cost | 0.02 ms update + 0.15 ms cmd-gen | 0.57 ms layout + 0.43–0.59 ms paint (own threads) | — |
| Memory | ~20 MB RSS | 153 MB PSS / ~350–400 MB RSS | 318 MB PSS, 17 processes |
| Binary | 3.8 MB | 126 MB (80 MiB stripped floor) | 256 MB (Linux libcef stripped) |

RmlUi pathologies to guard (measured): bare attribute selector 2.09 ms, font-effect glyph
gen up to 32.5 ms (make async), layout-thrash docs 5–16 ms (authoring guidance/linting).

## 4. UE integration mechanics (verified in local 5.8.1 source)

- **Native import APIs exist on all PC RHIs**: `ID3D12DynamicRHI::RHICreateTexture2DFromResource`,
  `IVulkanDynamicRHI::RHICreateTexture2DFromResource(VkImage)`,
  `IMetalDynamicRHI::RHICreateTexture2DFromCVMetalTexture` (IOSurface). D3D12 manual fences
  exposed; Vulkan `TexCreate_External` exports OPAQUE_FD/WIN32; plugins can enable extra VK
  device extensions (`FVulkanRHIExternalDeviceExtensionBase`, PostConfigInit).
- **Shared-texture interop is GO on all 3 platforms** (Slint+Servo precedent + local
  verification): Win = ANGLE-D3D11 NT handle → `OpenSharedHandle` → RHI wrap; Linux =
  *inverted*: allocate exportable VkImage on UE's device → `GL_EXT_memory_object_fd` import
  into engine GL → one blit (all extensions verified present on RTX 3090 Ti / 610.43.03);
  macOS = IOSurface → CVMetalTexture (production API, AvfMedia/Electra).
  Epic's disabled paths (TextureShare Vulkan-off, CEF Linux/Mac accel) are product
  priorities, not API failures.
- **Direct-RHI paint backend** (Noesis model, open source blueprint): implement the UI
  engine's render backend over `FRHICommandList`; present via `ICustomSlateElement`
  (RDG-based since 5.5: `Draw_RenderThread(FRDGBuilder&, FDrawPassInputs)`) and
  `FSceneViewExtensionBase::PrePostProcessPass_RenderThread` (pre-tonemap, world-space).
  Zero copies, one codebase across D3D12/Vulkan/Metal. Cost: UE API churn (budget shims for
  ~3 engine versions).
- **CPU fallback is bad**: Slate updatable texture path = 2 full-frame copies, ignores dirty
  rects (8.3 MB/frame @1080p, 33 MB @4K). `UTexture2D::UpdateTextureRegions` does partial.
- **Anti-pattern found in Epic's own code**: CEF pumped via `CefDoMessageLoopWork` on the
  game thread + accelerated-paint copy with a **busy-wait spin on the game thread**.
- Composition gotchas: Slate default blend is straight-alpha (web output is premultiplied —
  use premultiplied blend state or custom element); display-gamma vs linear-HDR paths
  (`FDrawPassInputs.bOutputIsHDRDisplay`); TexCreate_SRGB sampling.
- Input/IME patterns proven in engine: `ISlateViewport`/FReply capture, `IInputProcessor`,
  `FAnalogCursor`/CommonAnalogCursor, `FNavigationConfig`, `ITextInputMethodContext` (CEF
  IME handler is the full reference).

## 5. Architecture variants

### A. RmlUi + QuickJS-ng, direct-RHI, dedicated UI thread — **recommended**
- Dedicated **UI thread owns everything**: RmlUi Context + QuickJS runtime (both
  single-thread-affine — perfect match). Parse/style/layout/cmd-gen never touch game thread.
- Game thread: input event queue + data-binding dirty flags only. **No end-of-tick fence**
  (beats Gameface structurally).
- Double/triple-buffered command list handoff; render thread replays via RHI backend
  (ICustomSlateElement + SceneViewExtension). Snapshot model = Noesis blueprint.
- JS tiers: T1 QuickJS-ng embed + bindings (~2–4 mo, RmlSolLua = 2.5k LOC proxy);
  T2 vacuus-preact + TSX + esbuild (OneJS precedent in Unity) → "TSX+hooks" in ~6–12 mo;
  T3 (stock react-dom) explicitly out of v1. C++/BP data binding = no-JS tier from day one.
- Defensible claims: <0.2 ms/frame UI thread, ~20 MB RAM, <5 MB binary, zero game-thread
  stalls. Honest marketing: "React-style components + game-CSS", not "paste your website".
- Risks: RCSS ≠ full CSS (no Grid — Gameface also flex-only), JS devtools story open
  (QuickJS CDP adapters unverified; Hermes has CDP), BiDi = HarfBuzz backend work,
  mikke89 bus factor (mitigate: tracked fork + sponsorship).

### B. Servo via shared-texture interop — "browser-grade" tier
- Real web platform + React out of the box; MPL clean; parallel layout; interop recipes
  proven per platform. But 350–400 MB RSS / 100+ MB binary contradicts the premise; Rust
  FFI layer forever; 2 LTS migrations/yr; GC tuning needed; zero shipped games.
- Position: optional premium tier later, behind the same plugin-facing API as A.

### C. "CEF done right" — fastest to market, wrong premise
- Would beat incumbents (dedicated pump thread, accel OSR Win+Mac, Linux missing), but
  ships 300+ MB, 4–5 processes, input latency, CVE treadmill. It's the thing VaCuus
  positions against. Floor product at best.

**Recommendation**: A as the core; design the plugin-facing API engine-agnostic
(view/input/binding/render-handoff interfaces) so B can slot in later as a tier.

## 6. Next steps

1. USER DECISION: variant (A / B / C / A-with-B-roadmap) + v1 JS scope (T1+T2 vs binding-only MVP).
2. Spike (A): RmlUi RHI backend + dedicated UI thread in UE 5.8.1; windowed GPU cost @1080p/4K.
3. Spike (B, optional): Linux opaque-FD GL↔VK import into UE (1 day, all prereqs verified
   locally); Windows D3D11-fence↔D3D12 sync design.
4. Remaining unknown: Gameface trial profiling needs a Windows box (Unreal Insights).

## Addendum (2026-07-29): live compatibility test — busto.games in Servo 0.3.0

Driven via servoshell `--webdriver` + instrumentation userscript (fetch/WS/error hooks) on
this machine. Result: **the full production SPA works end-to-end**.

- Login screen (React SPA, Vite bundle, TON Connect script) rendered correctly in ~1 s
  headless; full interactive flow passed: click "Play without registration" → nickname
  modal → CONTINUE → game preloader → **live game**.
- In-game verified: Socket.IO WebSocket `wss://server.busto.games/lws/` opened (WS-OPEN),
  live round data in DOM (multiplier history, round #), Spine WebGL loading animation,
  **123 fps** by rAF, `/auth` 200, MP3 `decodeAudioData` OK (0.55 s buffer), fonts.ready OK,
  localStorage OK, 21-locale i18n, Howler audio stack loaded.
- **Caveats found**:
  1. `document.visibilityState` stays `hidden` in servoshell (even windowed) — the game's
     preloader waited for visibility; forcing `visible` + `visibilitychange` unblocked it
     instantly. In a UE plugin *we* control the embedder-side visibility signal, so this is
     a servoshell quirk, not an engine incompatibility — but must be handled in our shim.
  2. WebGL2 is off by default; **works behind `--pref dom_webgl2_enabled=true`**
     (WebGL 1.0 on by default; renderer "Mozilla/Servo").
  3. Missing on 0.3.0: `indexedDB` (undefined), `IntersectionObserver` (undefined) —
     busto.games didn't need them; framework-heavy sites might.
  4. Memory on the real game: **857 MB RSS / 718 MB PSS** in-game (long session, WebGL +
     Spine + React). Confirms Servo is "half-a-CEF", not lightweight. Fair Chrome-on-busto
     comparison still TODO.
- WebDriver screenshot times out on continuously-animating pages (waits for stable image) —
  known servoshell behavior, cosmetic for testing.

Implication: variant B (Servo) is *functionally* validated for "classic web dev" workloads —
a real production React+WebGL+WebSocket game runs unmodified. The cost side (memory, binary,
Rust FFI, API churn, GC tuning) is unchanged and now has a real-game data point.

## Addendum 2 (2026-07-29): root cause of "broken UI" in Servo on busto.games / kiwipoker.io

User-observed breakage (UI ~2x oversized, clipped right; giant GitHub ribbon) diagnosed to
TWO narrow Servo 0.3.0 gaps, not general "modern web impossible":

1. **CSS `zoom` property ignored** (verified: `CSS.supports('zoom','0.5')=false`, zoomed div
   renders unscaled). Both sites share one codebase (same `appProperties`, same i18n
   loading tips) that scales a design canvas via `el.style.zoom = k` (busto computed
   zoom=0.469, canvas 1152×1920). Zoom ignored → UI renders at full design scale, clipped.
2. **Interaction media features report touch-phone**: `(hover: hover)=false`,
   `(pointer: fine)=false` in Servo → the app classifies the device as a touch phone and
   picks the portrait mobile canvas even in a 1280×900 desktop window.

Counter-evidence that the rest of the pipeline is fine: viewport units and clamp() compute
correctly (10vmin=90px @1280×900, clamp(16px,2vmin,30px)=18px); kiwipoker's pre-zoom
loading screen renders pixel-correct; Chrome ground-truth screenshots captured for both
sites (`bench/` chrome-busto.png, chrome-kiwi.png vs servo-kiwi.png).

Secondary root-font observation: rootFS=44px on busto is the app's own rem-scaling driven
by the same mobile classification, not a Servo font bug.

Implications: (a) "renders arbitrary production sites pixel-perfect" is Chromium-class
territory (CEF) — Servo today = "most of the web, with a known gap matrix"; (b) both gaps
are well-defined upstream items (fix/contribute/pay Igalia) and MUST go into a public
supported-features matrix if variant B ships; (c) note Gameface would fail these sites
even harder (flex-only, no zoom, curated subset) — "full web" was never the middleware bar.

Upstream status (agent report + local verification on nightly 0.4.0-a80b5ee57, 2026-07-28):
- hover/pointer media features: FIXED upstream (PR #45681, 2026-06-15) — in nightly, not in 0.3.0.
- CSS Grid: implemented (taffy), OFF by default (`layout_grid_enabled`); enabling the pref
  fixes kiwipoker's lobby (verified visually). Default-on blocked by taffy panics (#46083).
- Wayland fractional scaling snaps to integer 2 (#41369 open) → use `--device-pixel-ratio`.
- CSS `zoom`: no upstream tracker; verified ignored. Site-side fix = transform:scale.
- **Blur status (verified on nightly)**: element `filter` (blur/drop-shadow/hue-rotate)
  WORKS (visual + CSS.supports true). `backdrop-filter` NOT implemented: CSS.supports
  false, visual no-op with graceful tint fallback. Issue #41567 (opened 2025-12-29, open,
  no assignee/PRs, labels A-gfx/displaylist + A-stylo + E-more-complex/mentor-available).
  Key fact: WebRender itself ships backdrop-filter for Firefox (FF103+), so the gap is
  Servo-side display-list/stacking-context plumbing, not renderer capability — a bounded,
  fundable contribution (join@servo.org for funded features; Igalia consultancy).
  Product note: "glass over the 3D scene" can be done engine-side in UE regardless
  (Slate post buffers scene-color sampling / own SceneViewExtension pass), and RmlUi
  (variant A) has both filter and backdrop-filter natively since 6.0.
- Nightly channel: servo.org/download, daily builds (servo-nightly-builds).

## Source digests

Full per-agent reports (12, with primary-source URLs) archived from workflow
`wf_acfe9f43-edd`; bench artifacts: RmlUi nanobench results, servoshell profiles, HUD
workload HTML (session scratchpad `bench/`).
