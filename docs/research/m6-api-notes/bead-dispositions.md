# M6 bead disposition table (Task 7.3)

Every open VaCuus bead referenced by the M6 spec/plan, the sweep research
(`p2-sweep.md`), the passport docs and the milestone docs — one row each, per spec
§3.5's contract ("the disposition table covers every open bead"). The controller
executes the `bd` side from this table; commits are on branch `m6-productization`.
Historical bead ids cited only inside code comments as provenance (akj.23, akj.6.29,
akj.6.34, akj.6.36 et al.) are already closed and are not rows here.

| Bead | Subject | Disposition | Evidence / reason |
|---|---|---|---|
| akj.22 | `rmlui_dynamic_cast` across `.so` works by load-order accident | **closed-in-M6** — 0eb0033 | Three exported non-inline cast helpers in VaCuusRml; 3 live + 3 test sites converted; canary test resolves a real `<input>` non-null; the load-order evidence lives in the helper header comment (p2-sweep.md §3) |
| akj.13 | CreateView returns a valid-looking handle on failed UI-thread init | **closed-in-M6** — 0eb0033 | `BootState` atomic on FVaCuusViewStatus; PollStatus on first Failed: one Error, Invalidate, OnLoadCompleted(false); stub-host test + restore-the-bug |
| akj.6.39 | PerfLog TickLog unbounded game-thread sort under the shared lock | **closed-in-M6** — 0eb0033 | Sort capped/windowed BEFORE any passport soak (plan 1.3 ordering — the passport leans on this machinery) |
| akj.16 | BP struct recompile dangles every FProperty* in a live layout | **closed-in-M6** — a96ecb4 | Fenced-synchronous teardown per spec §2(j); `Abandon()` only on timeout with leaked-bytes Error; recompile test extended; the residual-window trade-off recorded as a comment on the bead |
| akj.6.18 | `vacuus.ReloadUI` registered editor-only | **closed-in-M6** — a96ecb4 | Command moved (not copied) into the runtime module |
| akj.11 | RmlUi asserts compiled out / RMLUI_DEBUG decision unrecorded | **closed-in-M6** — a96ecb4 | Decision recorded in a comment at the define site: off in all shipped configs, local-debug recipe noted; log lines survive asserts (gotchas.md #14) |
| akj.6.35 | Digitized gamepad stick-presses entering the nav grid | **closed-in-M6** — a96ecb4 | Decided per spec §3.5: no stick-press navigation by default, `vacuus.NavStickPress` cvar opt-in (decision comment at SVaCuusWidget.cpp:32, gate at :794); default-off asserted in VaCuusSlateRoutingTest.cpp:1239 |
| akj.6.9 | itlib `flat_map::at()` UB in Shipping | **closed-in-M6** (close-with-evidence) | Verified unreachable, evidence scoped to the COMPILED subtree: zero `.at(` sites in RmlUi Source/Include and all five modules; the 30 Backends hits are never-compiled demo code, excluded from the package (8a345fc); vendor-update protocol line re-greps after any bump (spec §2(k)) |
| akj.12 | RunFrame perf scopes missing | **closed-in-M6** (close-with-evidence) | Already done: DrainCommands/DrainInput/DataApply/JsPump/JsGC scopes verified present with lines opened (p2-sweep.md P3 table); provenance claim uncited → dropped per spec §2(k) |
| akj.6.16 | Pointer-capture release duplicated across teardown owners | **closed-in-M6** (close-as-done) | Single-homed as `SVaCuusWidget::ReleaseOwnPointerCapture`, called from both owners, rationale documented in-code citing the bead |
| akj.6.17 | Uncooked standalone binary: missing global shaders, silent exit | **wontfix-documented** | Stock UE behavior (non-editor targets cannot build shader libraries from uncooked data); the contracted gotchas paragraph is docs/buyer/gotchas.md #17 (Task 7 docs commit); supported matrix stated: uncooked → `-game`, standalone → cooked only |
| akj.6.22 | Two-working-trees dev hazard (plugin edited where it does not build) | **closed-in-M6** | The workflow is documented in CLAUDE.md (build/edit tree split + why-a-clone); the second-working-tree detector ships in live reload (`VaCuusLiveReload.cpp`); the `AdditionalPluginDirectories` recipe + trap recorded on the bead |
| akj.6.38 | `SVaCuusWidget::OnPaint` carries no perf scope | **closed-in-M6** — 7d54c2f | Scope added before the passport soaks, per §11 row 1's own demand; passport row 1 sums it |
| akj.6.19 | Win64 IME re-check (M2 accept line's deferred leg) | **owner-hardware** | docs/buyer/owner-handoff.md §4 (matrix row 5, windowed, exact steps); the quickjs `/experimental:c11atomics` decision rides as its named sibling item (§5) |
| akj.17 | Unit-bearing data-style binding defeats the idle gate | **carried** (doc half landed) | The contracted perf-guide row is docs/buyer/perf-guide.md ("a unit-bearing data-style binding never goes idle", with the DataViewDefault.cpp:168-170 mechanism); vendored-tree normalization / upstream fix deferred to v1.x per p2-sweep.md |
| akj.6.42 | Test coverage gaps the M3 final review named | **carried** | Gaps overlapping M6-touched code landed in the M6 suites (bundle/RefHud/world tests — the suite grew 168 → 186 this milestone); the remainder deferred with the bead's own list, per spec §3.5 |
| akj.6.24 | (subject on the bead — not restated by any in-tree M6 doc) | **carried** | Deferred per spec §3.5's disposition line; controller confirms title at bd time |
| akj.6.37 | file:line citation drift the correction pass did not reach | **carried** | Deferred; drift is policed per-milestone by review (cf. 85bccda, this Step 0's :194-195 fix); the bead holds the known stale clusters |
| akj.6.40 | Inline mode makes `IsInUIThread()` true on the game thread | **carried** | Deferred; affects commandlet/`-nothreading` venues only — every M6 suite ran threaded; the assert's limits are documented where it lives |
| akj.6.41 | (subject on the bead — not restated by any in-tree M6 doc) | **carried** | Deferred per spec §3.5's disposition line; controller confirms title at bd time |
| akj.6.25 | Synchronous texture upload (measurement-gated: ~1 ms RT per 4 MB) | **carried** | Texture pipeline v1.x trio, per spec §7 out-of-scope list; the gate measurement stands in §11 row 7's history |
| akj.6.26 | (texture pipeline trio member; subject on the bead) | **carried** | Texture pipeline v1.x, per spec §3.5/§7 |
| akj.6.27 | Decodes launched while a view is unsized sit undrained | **carried** | Texture pipeline v1.x, per spec §3.5/§7 |
| akj.6.15 | Probe-host dedup across test files | **carried** | Deferred: grown to 20 hosts, half-day+ refactor, test-breakage risk, zero product value (p2-sweep.md) |
| akj.18 | Snapshot dirty-bit granularity | **carried** (parked as recorded) | M3b decision stands: current granularity fits, bead stays parked |
| akj.3 | Servo browser-grade tier | **carried** (parked as recorded) | v2+ option; gets its own view class if ever built (arch spec §10) — never constrains v1 |
