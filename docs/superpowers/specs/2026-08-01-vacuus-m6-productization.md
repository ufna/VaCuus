# VaCuus M6 — Productization

**Status:** design v1, for adversarial review before planning.

**Scope:** five tracks. **(B)** `UVaCuusBundle` — the cook-time archive replacing loose-file
staging in Shipping. **(R)** the reference HUD (~1,750 nodes, honestly counted) + the perf
passport (the arch §11 table filled at reference scale, Dev + cooked-Shipping-Linux columns).
**(F)** the Fab dry-run — `RunUAT BuildPlugin`, the package filter, the no-executables scan, the
compat-header seam for 5.6/5.7. **(D)** the buyer docs — gotchas, the generated RCSS matrix, the
perf guide. **(S)** the sweep — five bugs fixed or closed with evidence. **The environment
boundary is explicit:** Win64/macOS matrix runs and real 5.6/5.7 builds are owner-hardware items;
everything else lands here, and the handoff checklist is a deliverable, not an apology.

**Ground truth:** `docs/research/m6-api-notes/{bundle-cook,buildplugin-fab,refhud-passport,
p2-sweep}.md` (2026-08-01, written against the code and engine on this disk). **[unverified]**
claims carry their experiment.

---

## 1. Goal

A buyer downloads the plugin zip, drops it into a 5.6–5.8 project, reads three doc pages, ships a
game whose UI cooks into one memory-mappable asset — and the plugin's own reference HUD proves
every budget row with numbers the buyer can re-run.

---

## 2. The findings that decide the architecture

**(a) The bundle class lives in the Runtime module — the arch spec's "built by VaCuusEditor" is
corrected.** The cooked game deserializes the class, so it must be Runtime; the packing runs in
its own `WITH_EDITOR` `PreSave` (the cooker runs an editor target, so the code is present);
VaCuusEditor contributes only the asset factory/UI. One `FByteBulkData` blob + a UPROPERTY path
index — deliberately NOT `FFormatContainer`'s bulkdata-per-entry (each bulk datum is its own
IoStore chunk with 16 KiB alignment; ~50 small UI files would waste TOC entries and padding; one
blob gives one region and page sharing). Serialization: `Payload.SerializeWithFlags` with
`BULKDATA_Force_NOT_InlinePayload | BULKDATA_MemoryMappedPayload` when the target supports
mapping, `BULKDATA_ForceInlinePayload` otherwise — the exact `USoundWave`/`FFormatContainer`
recipe (BulkData.cpp:1730-1744, SoundWave.cpp:1456-1468).

**(b) Memory-mapping is Win64-only in 5.8 — Linux/macOS get a resident buffer, and the design
absorbs it.** `SupportsFeature(MemoryMappedFiles)` resolves per target platform; on this machine's
targets it is false. The read path is span-based either way: mapped → `StealFileMapping()` hands
the `FOwnedBulkDataPtr` (handle pair or wrapped allocation — BulkData.h:863-867,
BulkData.cpp:446-462) decoupling the region's lifetime from the UObject; resident → the same span
over the loaded buffer. The arch spec's "memory-mapped bundle entries" is thereby honest on Win64
and degrades gracefully elsewhere — stated, not hidden.

**(c) Cook staleness is solved by a 5.8 mechanism that exists: `FCookDependency::Function`.**
Incremental cook is default-on in 5.8; an unchanged asset whose *source tree* changed would keep
the stale payload. `OnCookEvent(PlatformCookDependencies)` registers a dependency function that
hashes the enumerated tree — a changed `.rml` re-cooks the bundle. Pack determinism: entries
sorted by normalized path; plugin-first duplicate-wins with every shadowed file logged (the D19
rule); `Tests/` excluded — the Build.cs staging caveat finally retires. **[unverified]** the
dependency function's exact re-cook trigger granularity — Exp-BUNDLE-STALE: change one .rml
between two incremental cooks, assert the second recooks the asset.

**(d) The VFS precedence is bundle-first-when-mounted, loose as fallback; Shipping ships
bundle-only.** The runtime mounts a bundle (config-listed soft path loaded by the subsystem at
startup, or game code hands one in); `FVaCuusFileInterface::Open` probes the mounted index before
the loose roots. Dev/editor mounts nothing by default — live reload keeps working on loose files.
The `RuntimeDependencies` staging globs stay for `Configuration != Shipping` (dev packaged builds
keep loose files); Shipping drops them — the bundle is the shipping story.

**(e) BuildPlugin's reality on Linux: three legs (editor-Dev, game-Dev, game-Shipping), no Win64
attempted, and the packaged tree is filter-driven — with two live hazards.** The default filter
ships `/Content/... /Shaders/... /Source/...` minus root-`/Tests/...` only —
`Source/*/Private/Tests/` rides as source (noise, accepted). **Hazard 1: `Web/` is not in any
default rule** — today's package has no Web at all, violating arch §2's "Web ships source-only";
adding `/Web/...` without `-/Web/node_modules/...` would ship the esbuild **ELF executable**
(verified with `file`). The FilterPlugin.ini gains both rules plus
`-/Source/ThirdParty/RmlUi/Backends/...` (upstream demo backends we never compile, carrying
`compile_shaders.py`). **Hazard 2: LFS pointers** — a checkout without LFS smudge would package
130-byte pointer files silently; the scan greps for the pointer signature. The `gen_relays.sh`
scripts stay (documented re-vendor procedure) with exec bits stripped; `vacuus.mjs`'s node
shebang is whitelisted. The scan (extension blacklist + exec bits + shebangs + node_modules +
LFS pointers) runs over the `-Package` output and is committed as a script the owner re-runs.

**(f) Zero version guards exist; the shim strategy is a seam now, experiments on owner hardware
later.** Writing speculative `#if` guards against unread 5.6/5.7 headers is comment-rot by
construction. M6 creates `VaCuusEngineCompat.h` and routes the four ranked hotspots through it
(`ICustomSlateElement::Draw_RenderThread`'s `FDrawPassInputs` fields; `RegisterInputPreProcessor`'s
registration-key overload; `FMaterialShader::SetParameters`' batched form;
`FSlateDrawElement::MakeCustom`'s header home). **Experiment SHIM-1** (owner hardware): per
engine, `BuildPlugin -StrictIncludes -TargetPlatforms=Win64`; every break lands as a
`UE_VERSION_OLDER_THAN` branch in the compat header; re-run all three engines. The M6 acceptance
line "BuildPlugin passes on 3 versions" is discharged on this machine as "passes on 5.8 Linux,
three legs, `-StrictIncludes`" + the seam + SHIM-1 in the handoff.

**(g) The 1,750 figure was never the HUD's — it is RmlUi's benchmark scale, and M6 makes it
true.** The bench HTML counts ~442 nodes; the number came from RmlUi's own benchmark suite
("1750 total elements") and was attached to the HUD by the research summary, inherited by arch
§11. Building "the bench port" would under-deliver 4×. **Decision: scale the composition to
genuinely hold it** — the designed layout (two 24-row scoreboard panels at 19 nodes/row, 18
enriched buff slots, 12 live + 40 clipped-history killfeed rows, 64 blips, damage pool, plate,
abilities, compass, settings) arithmetics to **≈1,731 nodes**, and the count gets its own
observable: a boot-time recursive node count logged and asserted ∈ **[1,650, 1,850]**
(Exp-REF-COUNT — the slogan becomes a checkable claim).

**(h) The driver split: C++ binding for standing data, plain JS for churn, RCSS keyframes for
free animation; TSX is a coexistence proof, not the workload.** Scoreboard + bars + ammo via
data binding (the M3b path; 48 rows × ~8 bindings ≈ 0.1 ms per changed frame by the M3b scaling
law); 64 blips via rAF writing **`transform` as one property** (64 × 2.4 µs ≈ 0.154 ms vs 0.31
for left+top, and no forced layout — Exp-BLIP-DRIVER settles it); killfeed churn + damage
numbers via JS (the M4 allocation shape); 18 buff icons on pure RCSS keyframes (zero script
cost); the M5 TSX HUD ships as a second document with its own already-owned pump row.

**(i) The passport's two empty rows get real machinery.** **RAM ≤32 MB** (no number exists):
layer 1 — the counting-malloc proxy grows byte-summing (+size on alloc, −GetAllocationSize on
free), installed pre-boot in the headless run; added-RAM = quiesced steady state minus baseline
([unverified] that RmlUi's `operator new` lands in GMalloc via UE's per-module replacement — one
canary allocation verifies before the window is trusted); layer 2 — `FPlatformMemory::GetStats`
before/after as the bypass-catcher bound; layer 3 — GPU reported separately (the 1080p view RT
alone is 7.9 MB), **and whether "Added RAM" includes GPU is an owner decision the passport
records explicitly**. **Disk ≤10 MB Win64 Shipping** (no number): two Linux Shipping
`BuildCookRun` packages, plugin on vs off, delta of staged bytes, itemized (binary + pak +
bundle); entered as *Linux proxy* with the Win64 literal in the handoff. **The measurement runs
from the cooked bundle or it measures the wrong shipping story.** Load-hitch adds the
font-effect warm-up case (the recorded 32.5 ms glyph-gen pathology — Exp-GLYPH-WARMUP) and
documents the UI-thread build spike (the M3b grow-frame shape) beside the game-thread zero the
gate structurally watches.

**(j) The sweep verdicts are evidence-based, and one is the milestone's biggest find.**
**akj.22:** the three `VaCuusTextInput` casts *work today by a load-order accident* — the
identity statics bind per dlopen closure under `RTLD_LOCAL`; VaCuusRender's `PostConfigInit`
load pulls VaCuusRml+VaCuus into one closure, so their ids unify; VaCuusJs (Default-phase, its
own dlopen) was why both M4 incidents were real. Proven with a dlopen probe against the real
binaries. One `.uplugin` edit away from silent death: the fix is three exported non-inline cast
helpers in VaCuusRml (~40 lines, the shape that fixed M4) + a canary test; the global fixes are
rejected with reasons (WEAK-DEFAULT export doesn't help under RTLD_LOCAL;
`linux_global_symbols` risks cross-plugin RmlUi collisions). **akj.13:** true forever —
`Command.Status` never stamped; fix = a `BootState` atomic on the view status, `PollStatus`
invalidates + fires `OnLoadCompleted(false)` on first `Failed`. **akj.16:** the refusal exists
nowhere; the hook is `INotifyOnStructChanged` whose `PreChange` fires **before** the compile
while the old property chain is alive — the one safe teardown moment; game-side shadow destroyed
synchronously, UI-side dropped via an `Abandon()` (free without `DestroyStruct` — a bounded
editor-only leak beats a wrong-offset free). **akj.6.17:** reproduced; stock UE behavior; close
as wontfix-documented ("uncooked → editor `-game`; standalone binary → cooked only" in the
gotchas). **akj.6.9:** zero `.at(` call sites in the entire vendored tree — close as
verified-unreachable + one line in the vendor-update protocol. Quick closes: akj.12 (done in
M3a — verified), akj.6.16 (done differently — verified). Swept-in: akj.6.18 (move
`vacuus.ReloadUI` runtime-side, ~30 min), akj.11's RMLUI_DEBUG decision (keep off everywhere,
record it), akj.17's doc row. Deferred with reasons: akj.6.15 (20 test hosts, zero product
value), akj.17 normalization.

---

## 3. Architecture

### 3.1 Track B — `UVaCuusBundle`

Per §2(a-d): the Runtime-module class (`Index` UPROPERTY + manual-serialized `Payload`),
`PreSave(IsCooking)` packing with deterministic order and D19 duplicate rules, the cook
dependency function, per-platform flag selection, `StealFileMapping` at mount, the span-based
VFS branch (bundle-first), the Shipping-only staging retirement (globs gated on configuration),
`vacuus.DumpBundle` (index listing + provenance). The editor factory in VaCuusEditor. Tests:
pack/read round-trip (editor-built bundle read through the VFS on the UI thread); duplicate
shadowing logged; Tests/ excluded; the staleness experiment; the M5 demo booting from a mounted
bundle in the packaged gate rerun.

### 3.2 Track R — the reference HUD + passport

Per §2(g-i): `Content/DevUI/RefHud/` (document + RCSS + JS driver + the C++ feed on the demo
driver pattern), `vacuus.RefHud` (+ the Shipping ignition flag), the node-count observable, the
driver split, the passport soak (every §11 row, Dev + cooked-Shipping-Linux columns, the PerfLog
machinery that already exists + the new RAM/disk experiments), `SVaCuusWidget::OnPaint` gains
its scope first (the row's own prerequisite). The passport lands as
`docs/passport/2026-08-vacuus-perf-passport.md` with every number's method named.

### 3.3 Track F — the Fab dry-run

Per §2(e-f): FilterPlugin.ini rules (+Web, −node_modules, −RmlUi/Backends), exec-bit hygiene,
the scan script (committed, run, output recorded), `RunUAT BuildPlugin -StrictIncludes
-TargetPlatforms=Linux` run to green (three legs), the packaged-zip inventory diffed against
expectations, `VaCuusEngineCompat.h` + the four hotspot reroutes, SHIM-1 written as the
owner-hardware experiment. `CanContainContent` confirmed required (the bundle asset). The
third-party disclosure list drafted (RmlUi MIT, quickjs-ng MIT, preact MIT, itlib MIT, LatoLatin
OFL — inventory verified against the trees).

### 3.4 Track D — the docs

`docs/buyer/`: **gotchas.md** (the 16 recorded findings, seeded verbatim from the research
inventory), **rcss-matrix.md** (generated from `StyleSheetSpecification.cpp`'s 99+20
registrations keyed to `VENDORED_SHA.txt`, + the second enumeration pass for
decorators/font-effects, + annotation column), **perf-guide.md** (the §11 table, the
bindings×rows scaling law, the facade op costs, the transform-vs-left idiom, the idle-gate
contract, the data-style-units row from akj.17), **setup.md** (the ten scattered buyer notes:
style-set cooking, Shipping ignition + logging, Web npm-install, DevUI roots, live-reload
editor-only, the stale-receipt rule until the bundle, quickjs re-vendor protocol). The
owner-hardware handoff checklist as its own page.

### 3.5 Track S — the sweep

Per §2(j), in the research's recommended order: akj.22 (helpers + canary), akj.13 (BootState),
closes with evidence (akj.6.9, akj.12, akj.6.16, akj.6.17-documented), akj.16 (the recompile
refusal), akj.6.18 (ReloadUI move), akj.11 (decision recorded), akj.17 (doc row). Each fix
restore-the-bug where expressible.

## 4. Threading

Nothing new in kind: the bundle mount publishes an immutable index (the style-set snapshot
pattern); the mapped region's lifetime outlives every document reading it (owned by the mount,
released after views close — the ReleasedTextures discipline); the VFS read path stays UI-thread.

## 5. Acceptance

1. The M5 demo + the reference HUD boot from a **cooked bundle** in packaged Development AND
   Shipping (Linux), zero JS errors, clean teardown — the M5 gate rerun on the bundle path.
2. The passport is filled: every §11 row has a Dev number and a cooked-Shipping-Linux number
   (or a named owner-hardware line), with methods.
3. `BuildPlugin -StrictIncludes` green on 5.8 Linux, three legs; the scan clean; the zip
   inventory as designed.
4. The docs exist and every gotcha cites its source.
5. The sweep: five beads closed with evidence, three quick-closes verified, the deferred two
   reasoned.
6. The handoff checklist enumerates every owner-hardware item with its exact command.

## 6. Risks

| Risk | Mitigation |
|---|---|
| The cook dependency function's granularity surprises | Exp-BUNDLE-STALE before the design freezes; fallback = hash-in-PreSave + always-recook (correct, slower) |
| RmlUi allocations bypass GMalloc | the canary check before the RAM window is trusted; the FPlatformMemory bound catches the rest |
| The reference HUD breaches the UI row at 1,731 nodes | the M3b document-side scaling law predicts ~0.1 ms/changed frame for the scoreboard; if the soak breaches, the routes are document-side (fewer bindings per row, coarser rows) — the budget commentary already frames the headroom |
| BuildPlugin finds a foreign-env compile break | -StrictIncludes on this machine first; the compat seam absorbs version drift |
| Fab's unrecorded rules | the dry-run confirms what §2 recorded; anything new lands in the disclosure draft |

## 7. Out of scope

Win64/macOS execution (handoff); real 5.6/5.7 builds (SHIM-1 handoff); the Fab upload itself;
pricing/licensing decisions (arch §15 owner items); akj.6.15's host dedup; akj.17 normalization;
per-element granularity (akj.18 stays parked); the Servo tier (akj.3).
