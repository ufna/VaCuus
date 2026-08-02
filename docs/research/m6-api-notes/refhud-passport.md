All evidence gathered. Composing the deliverable.

# Task C — Reference HUD + Perf Passport: design from what exists

## 1. The ~1,750 figure: provenance first, then the honest arithmetic

**Finding: the bench HTML file does not contain 1,750 elements.** Counted from the file itself (`/w/Unreal/VaCuus/docs/research/bench/hud.html`):

| Piece | Source lines | Elements | Text nodes (RmlUi `ElementText`) |
|---|---|---|---|
| Static scaffold (#root, 2 bars, #minimap, #killfeed, table+thead+tr+6th+tbody, .ammo, #buffs) | hud.html:26-33 | 17 | 7 (6 th + ammo) |
| Minimap blips | hud.html:36 (`for i<64`) | 64 | 0 |
| Killfeed rows | hud.html:38 (`for i<12`) | 12 | 12 |
| Scoreboard rows | hud.html:40 (`for i<24`, 6 td each) | 24 + 144 | 144 |
| Buff icons | hud.html:42 (`for i<18`) | 18 | 0 |
| **Total** | | **279** | **163** → **~442 nodes** |

Where 1,750 actually comes from: it is **RmlUi's own benchmark suite's scale**, not the HUD file — "Element construction and destruction of 1750 total elements" (`/w/Unreal/VaCuus/docs/research/bench/rmlui_benchresults.txt:105`, again at :134) and "Element update … with 1750 descendant elements and 260 unique RCSS rules" (:333). The research summary then attached that number to the HUD workload description ("identical animated game-HUD workload (1750 elements, 64 rAF minimap blips…)", `/w/Unreal/VaCuus/docs/research/2026-07-29-webui-middleware.md:18`; also "cmd-gen ~150 µs @1750 elems", :76), and arch §11 inherited it (`docs/superpowers/specs/2026-07-29-vacuus-architecture-design.md:361-364`). The M4 spec already flagged the document's nonexistence but not the count's provenance (`2026-07-31-vacuus-m4-js-tier1.md:373-378`). **Consequence: building "the bench workload ported" yields ~440 nodes, 4× short of the promise. M6 must either scale the composition to genuinely hold ~1,750 nodes, or re-baseline §11's sentence. Recommendation: scale — the budgets' headroom commentary ("~10× the M1 element count", arch:370-371) and the passport's marketing value both assume the big number.**

### Inventory of parts that exist

| Asset | What it contributes |
|---|---|
| `Content/DevUI/m1_hud.rml` (94 lines) | Static 24-row scoreboard (2 spans/row, m1_hud.rml:33-56), 4-row killfeed (:61-66), compass (:21-26), player plate with bars (:9-17), 4-slot ability bar (:87-92), overflow-clip scissor precedent (:28-29) |
| `docs/research/hud-demo/data/hud.rml` (78 lines) | Ability bar with 5 sub-elements/slot (hud.rml:47-50), minimap chrome ×6 (:55-60), crosshair+hitmarker (:36-43), settings panel (:67-76), damage layer (:33) |
| `docs/research/hud-demo/data/hud.js` (245 lines) | The JS drivers: rAF minimap tick (hud.js:166-173), damage-number spawn/remove via timers (:56-77), killfeed push/fade/cap (:95-111), ability cooldowns (:135-153) — but only **8 blips** (:158-161), not 64 |
| `Content/DevUI/m3_demo.rml` / `m4_demo.rml` + `m4_hud_logic.js` | The two driver paths side by side: data binding (scalars, `data-style-width` bar, `data-for` killfeed — m3_demo.rml:23-89) and JS DOM (killfeed via createElement, damage via timers, rAF bar — m4_demo.rml:36-91); serial-deterministic parity discipline (m4_hud_logic.js:9-13) |
| M3b 200-row fixture | C++ side `VaCuusDataForTest.cpp:772-794`; JS churn twin at M3b rates in `VaCuusJsCostTest.cpp:333-382` (200-row FIFO, trim from front :381) |
| `Web/apps/demo-hud/src/hud.tsx` (107 lines) + `Content/DevUI/M5Hud/` | TSX HUD: killfeed capped at 5 (hud.tsx:60-66), model poll for Health (:50-53); mounts as a single `#mount` div + committed bundle (m5_hud.rml:24) |
| `Content/DevUI/M5Hud/vacuus-base.rcss` | The missing-UA-layer base sheet (vacuus-base.rcss:4-8) |

### Composition that reaches ~1,750 honestly (design — arithmetic checkable)

Node counts = elements + their text nodes, per the bench-count method above. [inference] rows marked * are deliberate enrichments beyond any existing file; everything else reuses an existing structure at its cited shape.

| Piece | Structure | Nodes |
|---|---|---|
| Minimap | container + 6 chrome (hud.rml:55-60) + **64** blips (bench hud.html:36) | 71 |
| Buff bar | **18** × (slot + icon + cooldown-sweep + stack span/text + timer span/text)* + container | 127 |
| Scoreboard | **2 team panels* × (panel + header + 24 rows)**, row = div + rank span/text + name span/text + 4 stat cells (span/text) + avatar + class icon + 2-part ping meter* = 19 nodes | 952 |
| Killfeed, live | 12 × (row + 3 spans + 3 texts + HS pill) + container (m3_demo.rml:82-88 row shape) | 97 |
| Killfeed scrollback* | 40 clipped history rows × 8, under `overflow: hidden` (m1_hud.rml:28-29 precedent) | 320 |
| Player plate + bars | m1_hud.rml:9-17 shape | 24 |
| Ability bar | 4 × 6 + container (hud.rml:47-50) | 25 |
| Damage-number pool | 24 live × (div + text)* + layer | 49 |
| Crosshair + hitmarker | hud.rml:36-43 | 9 |
| Compass | m1_hud.rml:21-26 | 20 |
| Ammo + objective | 8 |
| Settings panel | hud.rml:67-76 | 26 |
| Vignette + perf overlay | 3 |
| **Total** | | **≈1,731** |

**The count needs its own observable** (project convention: an invariant with no observable rots). The reference HUD logs a recursive node count at document-ready (`vacuus.RefHud` boot line) and the passport asserts it lands in **[1,650, 1,850]**. That converts "~1,750 elements" from a slogan into a checkable claim.

## 2. Driver split

Decision: **plain JS + C++ data binding; TSX is garnish, not the workload.** Basis: §11's workload definition is the bench port plus the demo's ability bar and damage numbers (arch:361-364) — written before TSX existed; the M4 amendment moves reference-scale numbers to "the milestone that builds that document" with the JS/binding split intact (arch:426-429); M5 measured TSX pump separately and its own row already exists (m5 §6:291). The TSX HUD (`M5Hud/`) ships in the reference project as a second document proving coexistence, excluded from passport rows except the already-owned TSX pump row.

| Surface | Driver | Path + evidence |
|---|---|---|
| Scoreboard (both panels) | **C++ data binding**: `data-for` over `TArray<FScoreRow>` | The M3b path (m3b spec §3); sparse updates (a stat bump every few seconds). This is the row that stresses re-evaluation scaling — M3b's one-changed-row cost was 0.42 ms at 200 rows (m3b §9:424), scaling with bindings×rows (:434-435). 48 rows × ~8 bindings ≈ [inference] ~0.1 ms per changed frame |
| HP/MP bars, ammo, objective | **C++ data binding**: `data-style-width` + `{{}}` text | m3_demo.rml:40, :25-29 |
| 64 minimap blips | **JS rAF** | bench hud.html:44-52 tick; hud.js:166-173 write pattern, but write **`transform`, one property per blip**, not left+top — facade style-set costs 2.4 µs/op (m4 §7:389), so 64 × 1 write ≈ 0.154 ms vs 64 × 2 ≈ 0.31 ms against a 0.50 ms budget, and transform avoids forcing layout |
| Damage numbers | **JS timers** | m4_demo.rml:85-91; serial-deterministic values (m4_hud_logic.js:9-13) so screenshots stay decodable |
| Killfeed live rows | **JS createElement/remove** | m4_demo.rml:75-83. This resolves the M3b-vs-M4 double ownership: JS gets the churn (the allocation shape M4's budgets were taken on), C++ keeps the standing data; one owner per surface per m4_demo.rml:17-18 ("two competing truths") |
| 18 buff icons | **RCSS keyframes only, no driver** | bench hud.html:21-22 `@keyframes pulse` — exercises the animation system with zero script cost |
| Hitmarker, vignette | JS class toggles | hud.js:82-88 restart pattern |

## 3. Passport gap table (§11 row → what's measured where → what's missing)

| §11 row | Budget | Numbers on record | Missing at reference scale | Machinery |
|---|---|---|---|---|
| Game-thread | ≤0.10 ms | M2: 0.004 avg / 0.011 p99-sum, M1+M2 docs (arch:369) | Reference-scale rerun; **prerequisite: the row's own text demands `SVaCuusWidget::OnPaint` gain a scope before anyone tightens/passports this** (arch:369) | PerfLog GameTick/SlateTick/Input scopes exist; add OnPaint scope |
| UI Update+Record | ≤0.50 ms | M2 subsets: 0.052/0.113 and 0.023/0.060 (arch:370); M4 combined with JS, demo scale: sum-of-p99 ≈0.43 PASS; 200-row churn 0.64–0.81 p99 **breach, attributed to document-side re-evaluation** (m4 §7:386) | **The headline gap.** No number at ~1,750 nodes. Highest-risk row: the churn breach already lives in this budget's regime | PerfLog Update/Record + JsPump/JsGC scopes exist (VaCuusStats.h:197-211) |
| Replay | ≤0.50 ms | M1: 0.03/0.07 @97 draws (arch:371); M5 deco A/B settle publishes 0.45-0.47 incl PSO (m5 §6:288) | Reference-scale draws-count run | Replay scope + PerfLog windows exist |
| Composite | ≤0.05 ms | M2: 0.004 avg, true-idle frames (arch:372) | Formality rerun (scale-insensitive by design, but assert it) | exists |
| **RAM ≤32 MB incl JS ≤16** | gate | **NONE** (arch:373 is bare "gate"). JS sub-row only at demo scale: ~617 KB at collection (m4 §7:387-388) | Everything | §4a below — partial machinery exists, plugin-side observable does not |
| **Disk ≤10 MB Win64 shipping** | gate | **NONE** (arch:374) | Everything | §4b below |
| Load-hitch 0 >1 ms | gate | M2: max 0.033 ms + pathological-image stress (arch:375) | Reference-scale + bundle path + font-effect warm-up case | window method exists; see §4c |
| Cooked-Shipping gate (M6 accept, arch:439-440) | §11 green from cooked bundle | Linux Shipping cooked run PASS as a **smoke** gate — demo ran, clean teardown, zero JS errors, but **no §11 numbers were taken in Shipping** (proofs/m5-t9-acceptance/README.md:38-61) | All rows in a Shipping column; Win64/macOS runs | §4d below |

## 4. Measurement machinery for the missing rows

**a) RAM (Exp-RAM-DELTA).** What exists: JS live-bytes atomic counter fed by the FMemory malloc hooks (`VaCuusJsRuntime.h:177`, :322); heap-at-collection via `JS_ComputeMemoryUsage` recorded per GC (`VaCuusStats.cpp:88-89`, :258, printed :360 — deliberately not per-frame, VaCuusStats.h:203-209); the 16 MB cap is genuinely enforced (`JS_SetMemoryLimit`, VaCuusJsRuntime.cpp:107, default 16 at :14-18) — the JS half of the row is fully observable today. What does not exist: any plugin/RmlUi-side memory observable — no allocator hooks in `VaCuusRml` (grep of Public/Private came back empty), no `FPlatformMemory` call anywhere in `Source/` (grep empty), no LLM tags. Design, three layers:
1. **Byte-summing FMalloc proxy**: extend `VaCuusCountingMalloc.h` (already the engine-shaped GMalloc-chaining proxy, MallocPoisonProxy pattern, VaCuusCountingMalloc.h:17-50) from counts to bytes — `+size` on Malloc/Realloc, `-GetAllocationSize` on Free; the size-reporting dependency is the one the JS hooks already document and probe (VaCuusJsRuntime.cpp:89-95). Install before demo boot in the headless run; added-RAM = quiesced-steady-state minus pre-boot baseline, under the M3b quiesced-window protocol (m3b §9:415-416). [inference] RmlUi's plain `operator new` lands in GMalloc via UE's per-module operator-new replacement — verify with one canary allocation before trusting the window.
2. **Coarse cross-check**: `FPlatformMemory::GetStats().UsedPhysical` before/after boot — catches anything bypassing GMalloc; noisy, so it bounds rather than measures.
3. **GPU reported separately**: per-view output RT is PF_B8G8R8A8 at view size (`VaCuusReplayRenderer.cpp:172-178`) = 7.9 MB @1080p, plus UI textures (:214-218) and the glass half-res RT (`VaCuusSlateElement.cpp:397-404`). **§11 does not say whether "Added RAM" includes GPU — that is an owner decision the passport must record; at 1080p the RT alone is a quarter of the budget.**

**b) Disk (Exp-DISK-DELTA).** Nothing measured. The editor .so total (~8.3 MB: VaCuusRml 3.5M + VaCuusJs 1.8M + VaCuus 1.4M + VaCuusRender 1.4M + VaCuusEditor 176K, `Binaries/Linux/` du) is a signpost, **not** the number — shipping is monolithic and dead-stripped. Honest measurement: two `BuildCookRun -platform=Linux -clientconfig=Shipping` packages of VcHost, plugin enabled vs disabled; delta of total staged bytes = added disk. Itemize inside: monolithic-binary delta + pak delta (DevUI loose content today: 636K incl 152K fonts, du) + the `UVaCuusBundle` asset once it exists. **Trap: the M5 packaged run staged loose DevUI via the Build.cs receipt (proofs README:29-31); the passport measurement must run from the cooked bundle (arch:439-440) or it measures the wrong shipping story.** Linux number enters the passport marked *proxy*; the Win64 delta is an owner-hardware handoff line.

**c) Load-hitch.** M2's machinery carries over; two reference-scale additions: (1) the ammo/title glow maps to an RCSS font-effect, the measured 32.5 ms glyph-gen pathology (research :94-95) — the run must exercise the §9 warm-up (arch:337-340) and the passport asserts no hitch *with effects present* (Exp-GLYPH-WARMUP); (2) M3b's grow-0→200-in-one-frame spike (1.71-1.98 ms **UI-thread** Update, m3b §9:426) is the shape of building 1,750 nodes: it lands on the UI thread, which the game-thread hitch gate structurally cannot see — the passport documents the UI-thread load spike as its own number next to the game-thread zero.

**d) Shipping column.** Rerun the reference HUD from the cooked bundle in Linux Shipping with PerfLog live — the preconditions are already recorded: host `bUseLoggingInShipping` (proofs README:44-46), `-ExecCmds` compiled out so ignition is a plugin-parsed launch flag (README:39-43), Saved tree in the user dir (README:53-54). Every §11 row gets a Dev-editor number and a cooked-Shipping-Linux number; Win64 D3D12 + macOS Metal columns are the handoff checklist (arch:404, :441).

## 5. RCSS matrix + gotchas docs

**Supported-RCSS matrix — mechanically generatable.** All built-in registrations live in one function: `StyleSheetSpecification::RegisterDefaultProperties`, `Source/ThirdParty/RmlUi/Source/Core/StyleSheetSpecification.cpp:248-438` — **99 `RegisterProperty` + 20 `RegisterShorthand` call sites** (counted by grep within the function), each carrying name, default, inherited flag, and parser chain in fluent form (e.g. :262-274). A small parser emits the table (property / default / inherited / accepted values); regenerate keyed to `Source/ThirdParty/RmlUi/VENDORED_SHA.txt`. Hand-verified precedent for the format already exists: `docs/research/m2-api-notes/rmlui-input.md:190-207`. [inference] decorators/font-effects/@-rules register elsewhere (Factory instancers) and need a second enumeration pass — not opened here. Annotation column seeded from the notes (e.g. `nav-*` not inherited + needs per-element rules + `#` required, rmlui-input.md:207/:450; pointer-events inherited but no subtree pruning, :198).

**Gotchas page — recorded findings to seed it (all located):**
1. No UA stylesheet; `div` is inline — `M5Hud/vacuus-base.rcss:4-8`; base sheet ships (also in CLI template `Web/packages/cli/template/vacuus-base.rcss`)
2. box-shadow not animatable — verified in-tree: `ElementAnimation.cpp:638-648` refuses `BOXSHADOWLIST`; lint rule exists (`Web/packages/cli/lib/lint.mjs:68-99`)
3. Tween/transition parse strictness — vacuus-base.rcss:13-14; arch §13:398
4. `position: absolute` ancestor rules — vacuus-base.rcss:15-17
5. No default font ("No font face defined" per layout pass) — vacuus-base.rcss:18-19
6. Bare attribute selectors ~2 ms — research :94; lint rule exists (lint.mjs:33-57)
7. Font-effect glyph gen up to 32.5 ms — research :94-95; warm-up mitigation arch:337-340
8. Layout-thrash documents 5-16 ms — research :94-95
9. `data-model` resolved exactly once at `SetParent` → bind before load — m3_demo.rml:7-14
10. `data-for` template is a hidden clone source, `display:none` — m3_demo.rml:71-79
11. `{{ }}` written via JS text nodes stays literal (brace-injection contract) — m5 spec §7:298-301
12. Script/rcss `src` is document-relative; repeating the directory 404s with one named Error — m5_hud.rml:13-18 comment
13. No CSS Grid; flex-first — research :76
14. RmlUi asserts compiled out but its error log lines survive — bd memory `rmlui-asserts-compiled-out-but-logs-are-not`; commit b08bd34
15. StyleSheet/Template caches are process-global and outlive PIE — bd memory `rmlui-caches-outlive-pie-2026-07-30`
16. FName case-collision corrupts case-sensitive identities in cooked builds only — bd memory `fname-cooked-first-registration-wins` (a bundle-path cook gotcha)

**Perf guide seeds:** the §11 table + the scaling law that keeps biting (re-evaluation ∝ bindings×rows, m3b §9:434-435; the M4 churn breach m4 §7:386), the facade op costs (m4 §7:389), the transform-vs-left/top blip arithmetic (§2 above), the idle-gate two-layer contract (m3a §9:428-430).

## Named experiments
- **Exp-REF-COUNT**: boot-time recursive node count logged; assert ∈ [1,650, 1,850].
- **Exp-REF-SCALE**: full PerfLog soak of the composed HUD (the UI row's verdict; if it breaches, the M3b document-side routes apply, not JS).
- **Exp-BLIP-DRIVER**: 64-blip rAF, `transform` single-write vs `left`+`top` — settles the driver idiom with the 2.4 µs/op arithmetic.
- **Exp-RAM-DELTA**, **Exp-DISK-DELTA**, **Exp-GLYPH-WARMUP**: as specified in §4.

## Owner-hardware handoff (documented, not run here)
Win64 D3D12 + macOS Metal §11 columns; Win64 shipping disk delta (the row's literal platform); Win64 IME re-check (arch:417-418); 5.6/5.7 matrix builds.
