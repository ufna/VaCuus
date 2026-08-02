# VaCuus M6 — Productization: Implementation Plan

> **For agentic workers:** fresh subagent per task, two-stage review after each pair. `- [ ]`
> tracks steps.

**Spec:** `docs/superpowers/specs/2026-08-01-vacuus-m6-productization.md` (v2 after a three-lens
review with 24 blocking findings; §9 lists them — several tasks below are their proofs).
**Ground truth:** `docs/research/m6-api-notes/*.md`.
**Environment:** `/w/Unreal/VcHost/Plugins/VaCuus`, branch `m6-productization`. CLAUDE.md hazards
apply. Baseline: **168 tests**, all green. Implementers do not run `bd`.

**Non-negotiables:** every RmlUi/JS call on the UI thread; M3a-M5 suites green at every boundary;
every refusal named; comments cite source, lines opened; restore-the-bug where marked.

---

### Task 1: The sweep, part 1 — akj.22 helpers, akj.13 BootState, the quick closes

- [ ] **1.1** akj.22: three exported non-inline cast helpers in VaCuusRml (`ElementFormControl`
      cast + the two selection getters), the three live `VaCuusTextInput.cpp` sites + three test
      sites converted, the canary test (helper resolves a real `<input>` non-null) — per
      p2-sweep.md §3's recommended shape; the load-order-accident evidence goes in the helper
      header comment.
- [ ] **1.2** akj.13: `BootState` atomic on `FVaCuusViewStatus` ({Pending, Booted, Failed});
      stamped in the AddView failure branch and after Hosts.Add; `PollStatus` on first `Failed`:
      one Error, `Invalidate()`, `OnLoadCompleted(this,false)`. Stub-host test + restore-the-bug
      (remove the stamp).
- [ ] **1.3** akj.6.39 (BEFORE any passport soak): cap/window the PerfLog TickLog sort.
- [ ] **1.4** Commit: `fix: the sweep lands — casts that survive load order, views that admit failure`

### Task 2: The sweep, part 2 — akj.16 fenced refusal + the small items

- [ ] **2.1** akj.16 per spec §2(j): `FVaCuusStructRecompileGuard : INotifyOnStructChanged` in
      VaCuusEditor forwarding to a runtime static; match on root struct OR any ElementLayout
      struct; game-side shadow destroyed synchronously in PreChange; UI-side drop enqueued +
      `Trigger` + `WaitForFrameCount` (~100 ms; `RunFrameInline` inline); `Abandon()` only on
      timeout with the leaked-bytes Error. Extend the recompile test (bind, rename, assert one
      Error + refused Sample, no crash); restore-the-bug where expressible.
- [ ] **2.2** akj.6.18: move `vacuus.ReloadUI` into the runtime module (move, not copy).
- [ ] **2.3** The closes with evidence (controller executes the bd side; you write the evidence
      lines in the report): akj.6.9 (compiled-subtree grep output), akj.12, akj.6.16, akj.6.17
      (+ the gotchas paragraph lands in Task 7). akj.11's RMLUI_DEBUG decision recorded in a
      comment at the define site; akj.6.35 decided (no stick-press into nav grid by default,
      cvar opt-in — implement the cvar gate if trivial, else record the decision).
- [ ] **2.4** Commit: `fix: recompiles refuse loudly; the sweep's tail`

### Task 3: `UVaCuusBundle` — the asset, the cook, the VFS

- [ ] **3.1** The class per spec §2(a): Runtime module, transient Index/Payload filled in
      PreSave(IsCooking), cook-only manual serialization, FormatVersion, marker-only editor
      saves; the VaCuusEditor factory; `FCookDependency::Function` tree-hash registration.
- [ ] **3.2** The mount table per §2(b): steal-once ownership, lookup-vs-record split, the
      second-steal refusal; the VFS bundle-first branch with bounds-validated span reads; the
      mount predicate (§2(d)); the pack-on-demand editor mount + the live-reload shadow Warning;
      multi-bundle first-hit-wins; the serving counters + the miss Warning naming probed bundles.
- [ ] **3.3** Tests per spec §3.1: round-trip, determinism double-pack, shadowing, Tests/
      exclusion, version/bounds refusals (corrupted fixtures), two-bundle overlap,
      Exp-BUNDLE-UNMOUNT-RACE; the three cook experiments (FILEDEP ZenStore on+off, ADDFILE,
      edit) run and recorded; `vacuus.DumpBundle`.
- [ ] **3.4** The staging gate: RuntimeDependencies globs on `Target.Configuration != Shipping`;
      the cook-inclusion rule wired in the host project (DirectoriesToAlwaysCook) + the
      no-asset-resolved Error.
- [ ] **3.5** Commit: `feat: UVaCuusBundle — the UI tree cooks into one asset`

### Task 4: The reference HUD

- [ ] **4.1** `Content/DevUI/RefHud/` per spec §2(g-h): the composition (two-array scoreboard,
      published per-row enumeration), the drivers (binding/JS/keyframes split, single-property
      transform blips), `vacuus.RefHud` + Shipping ignition; the steady-state warm-up
      (serial-deterministic) and the count observable with its stated method.
- [ ] **4.2** `SVaCuusWidget::OnPaint` scope (akj.6.38 cited on the bead by the controller);
      Exp-BLIP-DRIVER run (transform vs left/top — record); Exp-REF-COUNT green in [1650,1850].
- [ ] **4.3** Screenshots read by eye at two beats; zero JS errors; the M3a-M5 suites green.
- [ ] **4.4** Commit: `feat: the reference HUD — 1,750 nodes, honestly counted`

### Task 5: The passport + PF_FloatRGBA + the manual matrix

- [ ] **5.1** PF_FloatRGBA composite permutation + the PIE check (spec §3.2).
- [ ] **5.2** The passport soaks: every §11 row, Dev + cooked-Shipping-Linux columns; the RAM
      A/B two-run delta (primary) + the fixed-formula proxy cross-check (Dev) + the GMalloc
      canary; the disk A/B staged-bytes delta from the cooked bundle, itemized; load-hitch with
      Exp-GLYPH-WARMUP + the UI-thread build-spike number; Exp-REF-SCALE (the one-panel dirty-
      scope assertion).
- [ ] **5.3** `docs/passport/2026-08-vacuus-perf-passport.md` with the mandatory Method column;
      the GPU-in-or-out owner decision recorded.
- [ ] **5.4** The Linux-Vulkan manual matrix checklist: enumerated, executed by hand once,
      recorded with screenshots; the identical list becomes the Win64/macOS handoff pages.
- [ ] **5.5** Commit: `feat: the perf passport — every row a number and a method`

### Task 6: The Fab dry-run + the compat seam

- [ ] **6.1** FilterPlugin.ini (+/Web/..., −node_modules, −RmlUi/Backends); exec bits stripped;
      the scan script + its planted-violation fixture (run both, record); the OFL.txt + itlib
      license verification; the disclosure list with in-tree pointers.
- [ ] **6.2** `VaCuusEngineCompat.h` + the four hotspot reroutes; SHIM-1 written as the
      owner-hardware experiment page.
- [ ] **6.3** `RunUAT BuildPlugin -StrictIncludes -TargetPlatforms=Linux` to green (three
      legs; run from a clean clone without node_modules); the zip inventory diffed vs
      expectations; findings recorded.
- [ ] **6.4** Commit: `feat: the plugin packages clean — and the scan has seen itself fail`

### Task 7: The docs + amendments + dispositions

- [ ] **7.1** `docs/buyer/`: gotchas.md (16 findings + akj.6.17's matrix note + the bundle
      shadow trap), rcss-matrix.md (generated, keyed to VENDORED_SHA; size the decorator pass
      first), perf-guide.md, setup.md (+ the bundle cook-inclusion rule + the mount predicate
      table); the handoff page (matrix checklists, SHIM-1, disk literal, akj.6.19 IME, the
      c11atomics decision, the Fab upload).
- [ ] **7.2** The arch-spec amendments (§9, §14, §15 inline, M4/M5 style).
- [ ] **7.3** The disposition table applied (the controller runs bd; you write the table).
- [ ] **7.4** Commit: `docs: the buyer reads three pages and ships`

### Task 8: Acceptance + merge

- [ ] **8.1** The packaged gates rerun ON THE BUNDLE: Development + Shipping, M5 demo + RefHud,
      M==0 asserted, the Linux resident path asserted, zero JS errors, clean teardown.
- [ ] **8.2** Full suite green; monolithic build; SIGTERM teardown all tracks live.
- [ ] **8.3** Spec §5's six acceptance lines walked and evidenced; merge `m6-productization` →
      master; beads closed (controller).

---

## Acceptance

Spec §5 verbatim — six lines, each with named evidence.
