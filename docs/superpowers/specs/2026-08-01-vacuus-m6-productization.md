# VaCuus M6 — Productization

**Status:** design v2, ready for planning. v1 was reviewed adversarially by three independent
passes and came back **NEEDS REWORK** with twenty-four blocking findings; §9 records the classes —
the project's habit — because the reasons are the most useful part.

**Scope:** five tracks. **(B)** `UVaCuusBundle`. **(R)** the reference HUD + the perf passport +
the PF_FloatRGBA permutation + the Linux-Vulkan manual matrix. **(F)** the Fab dry-run + the
compat seam. **(D)** the buyer docs + the arch-spec amendments. **(S)** the sweep with a full
bead disposition table. The environment boundary is explicit: Win64/macOS runs and real 5.6/5.7
builds are owner-hardware items; the handoff checklist is a deliverable.

**Ground truth:** `docs/research/m6-api-notes/*.md` (2026-08-01). **[unverified]** claims carry
their experiment.

---

## 1. Goal

A buyer downloads the plugin zip, drops it into a 5.6–5.8 project, reads three doc pages, ships a
game whose UI cooks into one memory-mappable asset — and the plugin's own reference HUD proves
every budget row with numbers the buyer can re-run.

---

## 2. The findings that decide the architecture

**(a) The bundle class lives in the Runtime module; the payload AND index serialize only into
cooks.** `Index` is **not a UPROPERTY** — a packed live object would otherwise leak its index
into ordinary editor saves (nondeterministic, undiffable, source-control churn). `Serialize()`
writes Index + Payload together, manually, only under `Ar.IsCooking()`; `PreSave(IsCooking)`
fills transient members, `PostSave` clears them; an editor save serializes a pure marker
(`SourceNote`). One blob + our index, not `FFormatContainer`'s bulkdata-per-entry (the 16 KiB
alignment penalty applies only to memory-mapped chunks — at ~50 entries ≤~800 KB of Win64-only
padding + ~2 KB TOC; real but modest — the stronger reasons are one region, one steal, page
sharing). The flag recipe is `USoundWave`'s minus its audio-feature check:
`BULKDATA_Force_NOT_InlinePayload | BULKDATA_MemoryMappedPayload` when
`SupportsFeature(MemoryMappedFiles)`, inline otherwise (BulkData.cpp:1728-1744,
SoundWave.cpp:1456-1468). **Format discipline:** an explicit `FormatVersion` field checked at
load/mount — refuse with an Error naming the bundle and both versions on mismatch; every index
entry validated against payload bounds at mount, refusal on violation; corrupted-fixture tests
for both.

**(b) Memory-mapping is Win64-only in 5.8 — Linux/macOS get a resident buffer.** Per-platform
`SupportsFeature` resolution verified (Windows true; Linux/Mac inherit Generic's false); the read
path is span-based either way via `StealFileMapping` (BulkData.h:863-867, BulkData.cpp:446-462).
**The steal is destructive and unrepeatable** (it nulls the source allocation) — so the mount
table owns the stolen region for the asset's whole in-memory lifetime: `UnmountBundle` removes
the LOOKUP entry but the subsystem retains the mount record for reuse; a genuine second-steal
attempt (null allocation, nonzero size) refuses with an Error. The packaged-gate rerun asserts
the resident path on Linux explicitly (the `!IsMapped` branch exercised, logged).

**(c) Cook staleness via `FCookDependency::Function` (verified to exist: CookDependency.h:131,
`UE_COOK_DEPENDENCY_FUNCTION` :375-377), hashing the enumerated tree.** The experiments the
research demanded all run: **Exp-COOK-FILEDEP with ZenStore ON and OFF** (the legacy-dependency
path is unprovable by reading — CookOnTheFlyServer.cpp:10590-10598), **Exp-COOK-ADDFILE** (an
added file must recook — the reason a tree-hash beats per-file deps), and the incremental-cook
edit case. Fallback if granularity disappoints: hash-in-PreSave + always-recook (correct,
slower). **Pack determinism gets its observable:** pack the identical tree twice from
differently-ordered sources, assert byte-identical payload + index hash; the content hash
surfaces in `vacuus.DumpBundle` so the observable persists into the field.

**(d) VFS precedence: bundle-first when mounted, loose fallback — with the mount predicate
stated and the editor story made real.** The mount decision: cooked builds
(`FPlatformProperties::RequiresCookedData()`) mount the config-listed bundle; the editor and
uncooked `-game` mount nothing by default. **The PIE parity workflow needs a payload the editor
asset does not have** (editor saves carry none) — so the editor mount path packs on demand:
`vacuus.Bundle.Enable 1` in PIE packs from the loose tree into a transient buffer
(`WITH_EDITOR`), and a live-reload watcher event over a mounted bundle logs a Warning naming the
shadowed path (the trap made loud, not silent). **Bundle-serving observability (the silent-miss
killer):** a Log-level teardown line per view — "N opens served by bundle 'X', M by loose
roots" — and the bundle-path acceptance gates assert **M == 0**; the miss Warning names every
probed bundle before the loose fallback. Multi-bundle: mounts stack in order, first hit wins —
stated, with a two-bundle overlap test. **Cook inclusion is a buyer-facing rule, not a
footnote:** a config-soft-path-only bundle is invisible to the cooker — the project must
hard-reference it or list it in `DirectoriesToAlwaysCook`; setup.md carries the rule, the
reference project demonstrates it, the Track B tests assert the cook log cooked the package, and
a cooked build whose configured soft path resolves to no asset logs one Error naming the path.
Shipping staging: the `RuntimeDependencies` globs gate on `Target.Configuration != Shipping`
(verified readable in ModuleRules); packaged Development stages both loose files and the bundle —
the M==0 assertion is what proves the bundle actually served.

**(e) BuildPlugin + the scan (unchanged from v1's verified mechanics) — plus the scan's own
fixture.** Three legs on Linux, the filter hazards (Web/ absent by default; the esbuild ELF; the
LFS pointer scan; `-/Source/ThirdParty/RmlUi/Backends/...`), exec-bit hygiene, the shebang
whitelist. **The scan is itself a test that must be seen to fail:** a committed fixture tree
plants one instance of each violation class (exe, exec-bit, shebang, node_modules, LFS pointer);
the wrapper runs against the fixture first and must report exactly those hits before its clean
verdict counts.

**(f) The compat seam + SHIM-1 (unchanged) — and the re-scope is RECORDED, not silent.** Track D
writes the arch-spec amendments in the established inline style: §14's acceptance discharged as
"5.8 Linux three legs + the seam + SHIM-1 handoff" with the Win64/macOS/5.6/5.7 items named;
§9's bundle wording corrected (Runtime-module class, Win64-only mapping); §15's
`CanContainContent` confirmed and the disclosure list referenced.

**(g) The reference HUD's count is a steady-state observable, not a boot one.** The ~1,731
composition stands (the 952-node scoreboard reading = 2 × (panel + a full 19-node header row +
24 × 19-node rows); the per-row enumeration is published in the document's comment) — but most of
it materializes at runtime: data-for clones instantiate on the first Update, killfeed
history/damage pools fill from the sim. **Exp-REF-COUNT is therefore defined at declared steady
state**: a serial-deterministic warm-up (N sim seconds saturating every pool — the M4 discipline)
then a recursive count with a stated method (elements + ElementText nodes; hidden data-for
templates excluded; scrollbars included if RmlUi generates them) asserted ∈ [1,650, 1,850].
**Gate precedence stated:** if Exp-REF-SCALE's breach routes ever shrink the document, REF-COUNT
yields — the budgets are gates, the count is a marketing claim made checkable; both cannot be
load-bearing at once.

**(h) The scoreboard arithmetic is corrected and the topology is the design.** M3b's law:
0.42257 ms for one changed row at 200 rows × 4 bindings = 800 bindings ⇒ ~0.53 µs/binding. One
48-row × 8-binding array ⇒ ~0.20 ms per changed frame. **The design: two separate top-level
arrays, one per team panel, with independent dirty scopes** ⇒ ~0.10 ms per one-panel change,
~0.20 worst-case both-panel churn — and Exp-REF-SCALE asserts a one-panel change re-evaluates
only that panel's bindings (the µs-per-binding shown in the passport). The rest of the driver
split stands (blips via single-property `transform` rAF; JS churn; RCSS keyframes; TSX as
coexistence proof).

**(i) The RAM row's primary form is an A/B two-run delta; the proxy is a Dev-only cross-check
with a fixed formula.** Three v1 defects: the proxy's requested-size-add vs quantized-size-
subtract accumulates negative drift ∝ churn; the proxy is `WITH_DEV_AUTOMATION_TESTS`-gated and
compiled out of Shipping (the row's own venue); and a single-run window attributes engine-side
load allocations to the plugin. **The design: primary = two identical cooked-Shipping-Linux
boots, plugin (or HUD) enabled vs disabled, `FPlatformMemory::GetStats().UsedPhysical` at
matched quiesced checkpoints — the honest form, valid in Shipping.** The proxy (Dev cross-check)
gets symmetric quantized accounting: add `Inner->GetAllocationSize(result)` after alloc,
subtract it before free, realloc = subtract-old + add-new — the sum equals live quantized bytes
exactly; the GMalloc canary stays. GPU reported separately; whether "Added RAM" includes GPU is
the owner decision the passport records. Disk row unchanged (A/B staged-bytes delta, itemized,
from the cooked bundle, Linux proxy + Win64 handoff).

**(j) akj.16's teardown is fenced-synchronous first; `Abandon()` is only the timeout fallback.**
v1's leak-vs-corruption dichotomy was false: the plugin already owns a fence
(`Trigger` + `WaitForFrameCount`, documented any-thread-safe). `PreChange` enqueues the UI-side
model drop, triggers, waits (~100 ms timeout; `RunFrameInline` in inline mode) — the old
property chain is alive for the whole window, so the normal `DestroyStruct` teardown runs.
Only on timeout does `Abandon()` fire, each occurrence logging an Error with an estimated
leaked-bytes figure — the residual leak is loud, rare, and measured, not a design principle.

**(k) The sweep's evidence is scoped precisely.** akj.22 unchanged (the load-order accident,
helpers + canary). akj.13 unchanged (BootState). **akj.6.9's closure evidence is scoped to the
compiled subtree**: zero `.at(` sites in `Source/ThirdParty/RmlUi/{Source,Include}` and all five
plugin modules; the 30 hits in `Backends/RmlUi_Renderer_DX12.cpp` are never-compiled upstream
demo code (and Backends is excluded from the Fab package by (e)); the vendor-update protocol
line greps the compiled subtree. akj.6.17 wontfix-documented; akj.12 close (the scopes exist —
verified; provenance uncited, dropped); akj.6.16 close-as-done. **akj.6.39 is promoted into the
sweep and lands BEFORE any passport soak** — the PerfLog TickLog's unbounded game-thread sort
under the shared lock is machinery the passport leans on; fix = cap/window the sort. akj.6.38
is cited by the OnPaint-scope task (the bead updates, not orphans).

---

## 3. Architecture

### 3.1 Track B — `UVaCuusBundle`

Per §2(a-d). Tests: pack/read round-trip through the VFS on the UI thread; the determinism
double-pack; duplicate shadowing logged; `Tests/` excluded; format-version + bounds-violation
refusals (corrupted fixtures); the two-bundle overlap; **Exp-BUNDLE-UNMOUNT-RACE** (unmount
mid-read, two threads — the region-lifetime claim's observable); the three cook experiments
(§2(c)); the M==0 bundle-serving assertion in the packaged rerun incl. the Linux resident-path
assert. `vacuus.DumpBundle` (index, provenance, content hash).

### 3.2 Track R — the reference HUD + passport + matrix

Per §2(g-i): the document + drivers + the steady-state count; `SVaCuusWidget::OnPaint` gains its
scope first (bead akj.6.38 cited); **akj.6.39's sort fix precedes the soaks**; the passport
(`docs/passport/`) with a **mandatory Method column** per row (an empty cell is visible); every
§11 row gets Dev + cooked-Shipping-Linux numbers or a named handoff line. **The PF_FloatRGBA
composite permutation** (M5's explicit assignment to M6): implement the editor/PIE float-target
permutation the arch spec requires for correct editor rendering, with a PIE composite check as
its observable. **The Linux-Vulkan manual matrix pass is defined and executed**: an enumerated
interactive checklist (mouse/keyboard/gamepad input on screen-space and world-space views, IME
text entry expectations per platform, live reload, glass/decorators visually verified, the demo
suite toggles) — run once by hand on this machine, recorded with screenshots; the identical
checklist ships as the Win64 D3D12 / macOS Metal handoff pages.

### 3.3 Track F — the Fab dry-run

Per §2(e-f): FilterPlugin rules, the scan + its fixture, `BuildPlugin -StrictIncludes` three
legs green, the zip inventory diffed, `VaCuusEngineCompat.h` + the four reroutes, SHIM-1
written. The third-party disclosure list with **every entry pointing at a real in-tree license
file the package includes**: RmlUi MIT (present), quickjs-ng MIT (present), preact MIT
(present), itlib MIT (verify in-tree; add if the vendored copy lacks it), **LatoLatin OFL —
currently MISSING: add `Content/DevUI/fonts/OFL.txt` with the Lato copyright notice** (a real
compliance gap v1 claimed as verified).

### 3.4 Track D — the docs + amendments

The four buyer pages as v1, **plus**: setup.md gains the bundle cook-inclusion rule and the
mount-predicate table (editor vs uncooked-game vs packaged); gotchas.md gains the
bundle-vs-live-reload shadowing trap; **the arch-spec amendment task** (§2(f)) writes the §9,
§14, §15 inline amendments. The handoff page carries: Win64/macOS matrix checklists (§3.2's
enumerated list), SHIM-1 per engine, the Win64 disk-row literal, the Win64 IME re-check
(bead akj.6.19 named), the quickjs c11atomics decision, the Fab upload itself.

### 3.5 Track S — the sweep + the disposition table

Fixes: akj.22, akj.13, akj.16 (per §2(j)), akj.6.39 (before soaks), akj.6.18, akj.11's decision
recorded, akj.17's doc row. Closes with evidence: akj.6.9 (scoped per §2(k)), akj.12, akj.6.16,
akj.6.17 (documented). **The disposition table covers every open bead** — the ones v1 silently
skipped, dispositioned here: akj.6.19 (handoff, named command); akj.6.35 (the stick-press
decision — decide it in this milestone: recommend "no digitized stick presses enter the nav
grid by default, cvar opt-in", record on the bead); akj.6.42 (fold its named coverage gaps into
the sweep's test work where they overlap the touched code, defer the rest with the list); 
akj.6.24/6.37/6.40/6.41 (defer with one-line reasons); akj.6.25/6.26/6.27 (defer — texture
pipeline v1.x); akj.6.15 (defer — 20 hosts, zero product value); akj.6.22 (close — the
two-tree workflow is this session's own working practice, documented in CLAUDE.md); akj.18/akj.3
(parked as recorded). Each disposition lands on its bead.

## 4. Threading

The mount publishes an immutable index (the snapshot pattern); **the region's release rule is
the mount table's** (§2(b)): the record outlives lookup removal and is destroyed only at
subsystem/module teardown after views close — the unmount-race test is the observable. The VFS
read path stays UI-thread; reads are clamped memcpys/span views validated at mount time.

## 5. Acceptance

1. The M5 demo + the reference HUD boot from a **cooked bundle** in packaged Development AND
   Shipping (Linux), zero JS errors, clean teardown, **bundle-served M==0 asserted**, the Linux
   resident path asserted.
2. The passport filled: every §11 row Dev + cooked-Shipping-Linux (or named handoff), Method
   column complete; PF_FloatRGBA's PIE check green.
3. `BuildPlugin -StrictIncludes` green on 5.8 Linux, three legs; the scan clean **after its
   fixture run reports exactly the planted violations**; the zip inventory as designed; every
   disclosure entry points at an in-tree license file.
4. The docs exist; every gotcha cites its source; the arch amendments landed.
5. The sweep per §3.5; the disposition table complete, every bead updated.
6. **The Linux-Vulkan manual matrix checklist executed and recorded**; the handoff enumerates
   every owner-hardware item with its exact command.

## 6. Risks

| Risk | Mitigation |
|---|---|
| Cook-dependency granularity | the three cook experiments before the design freezes; fallback always-recook |
| RmlUi allocations bypass GMalloc | the canary before the Dev cross-check window; the A/B primary form does not care |
| The HUD breaches the UI row at ~1,731 nodes | the corrected two-array topology (~0.10/0.20 ms); breach routes are document-side; REF-COUNT yields to the gates per §2(g) |
| BuildPlugin foreign-env break | -StrictIncludes here first; the seam absorbs drift |
| Fab's unrecorded rules | the dry-run confirms; new findings land in the disclosure draft |
| The steady-state count drifts with RmlUi internals | the method statement pins what is counted; the window is ±100 |

## 7. Out of scope

Win64/macOS execution, real 5.6/5.7 builds, the Fab upload (handoff); pricing/licensing owner
items; akj.6.15; akj.17 normalization; akj.18; akj.3; the texture-pipeline trio (akj.6.25-27).

## 8. (reserved)

## 9. What v1 got wrong, and why it matters

1. **The RAM proxy's formula accumulated bias** (requested-add vs quantized-subtract) **and its
   venue was compiled out of the config the row is for** — two independent breaks in one
   measurement design; the A/B two-run delta is the honest primary.
2. **A UPROPERTY index on a packed live object leaks cook state into editor saves** — the
   pack-once-serialize-thrice flow only works when cook data serializes exclusively into cooks.
3. **The PIE parity story was structurally vacuous** — the editor asset has no payload; mounting
   it tests the fallback path while reporting success. Pack-on-demand makes it real.
4. **`StealFileMapping` is once-only** and v1's remount story would have returned empty spans.
5. **The boot-time node count was unpassable** — most of the composition materializes at
   runtime; the observable moved to declared steady state with a stated method.
6. **The scoreboard arithmetic contradicted its own cited law** (the fixture has 4 bindings/row,
   not 8 per row of cost) — corrected to ~0.53 µs/binding and the two-array topology became the
   design, not an accident.
7. **`Abandon()` was a false dichotomy** — the plugin owns a fence; synchronous teardown is the
   primary and the leak is the measured timeout fallback.
8. **"Zero `.at(` in the entire vendored tree" was false as written** (30 hits in never-compiled
   backend code) — evidence scoped to what is compiled, the protocol line corrected.
9. **Silence, the recurring class**: PF_FloatRGBA (M5's explicit assignment) dropped; the
   Linux-Vulkan manual matrix undefined; five open beads undispositioned; the arch re-scopes
   unrecorded; the bundle cook-inclusion rule flagged by the research and dropped by the spec;
   the OFL license claimed verified while absent from the tree; the scan never seen to fail;
   determinism asserted without an observable; the dropped research experiments. Each is now a
   named deliverable with an observable — the M2 lesson, applied to a spec about applying it.
