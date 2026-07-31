# VaCuus M3b — Arrays and `data-for`: Implementation Plan

> **For agentic workers:** execute task-by-task with a fresh subagent per task and a two-stage
> review (spec compliance, then code quality) after each. Steps use `- [ ]` for tracking.

**Goal:** `TArray` UPROPERTYs drive RmlUi `data-for` — `{{ Arr.size }}`, indexed access, scalar and
struct rows — with the 200-row measurements that decide the milestone's one open question.

**Spec:** `docs/superpowers/specs/2026-07-31-vacuus-m3b-arrays.md` (v2, after a three-lens
adversarial review returned NEEDS REWORK on v1).
**Ground truth:** `docs/research/m3-api-notes/{m3b-impl-map,m3b-ue-arrays,m3b-rmlui-arrays}.md`
plus the M3a notes.

**Environment:** work in `/w/Unreal/VcHost/Plugins/VaCuus` on branch `m3b-arrays` (already cut from
`master`). Build/test commands and the dev-loop hazards are in `CLAUDE.md` — read that section
before the first build. Baseline: **62 tests**, all green. Implementers do **not** run `bd` (the
build clone has no beads DB); the controller tracks issues.

**Non-negotiables, from the spec:**
- Every RmlUi call on the UI thread; the M3a assertion sites stay as they are.
- **No stage stores an element address** (spec §2(c)/§4) — definitions stateless, helpers
  constructed per call, `GetRawPtr` results live for one expression.
- The array copy is `SyncCopy` (Resize + per-element assign / POD memcpy), never
  `CopySingleValue` on the array property (spec §3.3 — the engine's own copy destroys destination
  elements first).
- One dirty bit per top-level array; diff stops at the first difference.
- Element-context refusals (Text anywhere in the element subtree, nested arrays) live on the
  **array field** at desc-build time; element layouts stay plain and shared (spec §3.1).
- Every refusal and every wrong input logs a named line; nothing is silent.
- Comments cite engine/RmlUi source as `file:line`, and every cited line must have been opened.

---

### Task 1: Layout — the Array kind, the desc, the refusals

**Files:** `Source/VaCuus/Public/VaCuusModelLayout.h`, `Private/VaCuusModelLayout.cpp`,
`Private/Tests/VaCuusModelLayoutTest.cpp`, `Private/Tests/VaCuusModelLayoutTestTypes.h`.

- [ ] **1.1** Failing tests first: a fixture with `TArray<int32>`, `TArray<FString>`,
      `TArray<FKillfeedRow>` (struct row: strings + bool + a nested struct), an array nested inside
      a struct (`Panel.Items`), plus the refusal cases — `TArray<FText>`, a row struct with an
      `FText` member, a row struct with a `TArray` member, `TMap`/`TSet` elements, and the existing
      fixed-array / map / set fields still refused.
- [ ] **1.2** `EVaCuusFieldKind::Array`; `FVaCuusModelField::ArrayDescIndex`;
      `FVaCuusModelArrayDesc { ArrayProperty, ElementKind, Inner, ElementLayout }` (spec §3.1).
      The classifier's `FArrayProperty` refusal becomes acceptance; scalar inners classified with
      the existing classifier; struct inners build a plain `FVaCuusModelLayout` over the element
      type.
- [ ] **1.3** The desc-build scan: any element-layout leaf of kind `Text` or `Array` → the whole
      array field refused, one Warning naming the array property, the offending member and the
      reason (spec §3.1). Test observes each Warning.
- [ ] **1.4** Element top-level member names obey the full root rule — a row member named `Size`
      is refused with the root Error; test observes it (spec §3.1's stated price).
- [ ] **1.5** Fixed-array refusal text updated to name the decision (spec §3.2), not "M3b".
- [ ] **1.6** `LexToString` + `DescribeValue` extended (`-Wswitch` forces both). `DescribeValue`
      for arrays: `Num()` + first 8 elements + elision marker; add the value-pointer describe form
      the scalar elements need (spec §6). `CopyValue` grows its Array branch → `SyncCopy` stub
      calling site (implemented in Task 2; here it can forward to a TODO-free simple correct form —
      see 2.2 — but the funnel shape lands now: the field reaches its desc without callers passing
      the layout).
- [ ] **1.7** Commit: `feat: layout learns arrays — desc, element layout, refusals`

---

### Task 2: `SyncCopy` and the sampler — value-pointer comparators, array diff

**Files:** `Private/VaCuusModelLayout.cpp` (SyncCopy), `Private/VaCuusModelSampler.cpp`,
`Private/Tests/VaCuusModelSamplerTest.cpp`.

- [ ] **2.1** Failing tests first, sampler: change one element → exactly the array's bit; change
      the **last** element → detected; append / remove / clear → the bit; case-only `FString`
      element change → the bit (restore-the-bug: case-insensitive compare must fail it); NaN double
      element → no republish-every-frame (restore-the-bug: value compare must fail it);
      struct-element leaf change → the bit; unchanged array → no bit.
- [ ] **2.2** `FVaCuusModelArrayDesc::SyncCopy(DestValuePtr, SrcValuePtr)` per spec §3.3:
      `FScriptArrayHelper::Resize` then per-element `Inner->CopyCompleteValue` (non-POD) or one
      memcpy (POD — gate on `CPF_IsPlainOldData` of the inner). Tests: shrink preserves surviving
      values; grow constructs; same-`Num` warm sync into a warm destination performs zero container
      reallocations (allocation counting arrives with Task 6's proxy — here assert values + `Num`;
      leave the allocation assertion to 6.3).
- [ ] **2.3** The comparator refactor: extract per-kind value-pointer compare from
      `HasFieldChanged`; the field switch calls it through `ContainerPtrToValuePtr`, the element
      loop calls it on `GetRawPtr(i)` pairs; struct elements walk `ElementLayout->GetFields()` with
      the element base as the struct base. First difference wins. Bitfield-vs-native-bool: the
      value-pointer `FBoolProperty::GetPropertyValue` is mask-aware for both (m3b-ue-arrays.md §4);
      elements are always native (spec §2(e)).
- [ ] **2.4** `StoreField` becomes an exhaustive switch; the Array branch is chosen, and it is
      `SyncCopy` (spec §3.3). `-Wswitch` now guards the site.
- [ ] **2.5** Commit: `feat: array diff and SyncCopy — first difference wins, capacity survives`

---

### Task 3: Channel and apply with arrays

**Files:** `Private/VaCuusModelChannel.h/.cpp`, `Private/VaCuusBoundModel.cpp`,
`Private/Tests/VaCuusModelChannelTest.cpp`, `Private/Tests/VaCuusModelApplyTest.cpp`.

- [ ] **3.1** Failing tests: I2's no-regression proof with an array field (publish twice
      unconsumed, consume, no element regresses — must fail against a naive bits-OR design, i.e.
      re-run the existing restore-the-bug shape with array content); slot self-sufficiency with
      mixed scalar+array dirty sets; the apply walks bits → `SyncCopy` → `DirtyVariable` once.
- [ ] **3.2** Scope the channel header's "allocation-free in the steady state" and the shadow's
      "paid once per model, not per frame" comments per spec §3.4 — scalar fields as written;
      arrays are assignment-shaped per republish, allocation-free only where capacity absorbs
      content. Cite `ReallocForCopy`'s grow-only rule where the claim is made.
- [ ] **3.3** Commit: `feat: arrays ride the channel — same protocol, scoped cost claims`

---

### Task 4: The RmlUi adapter — `FVaCuusArrayDefinition` and the counters

**Files:** `Private/VaCuusDataVariable.h/.cpp`, `Private/Tests/` (a new adapter test file or the
existing apply-test host), `VaCuusRender/Private/Tests/VaCuusModelViewTest.cpp` fixtures as needed.

- [ ] **4.1** Failing tests first, through a real context (Apply-test DOM-probe style):
      `{{ Arr.size }}` renders and tracks growth; `{{ it }}` / `{{ it_index }}` over a scalar
      array; static `{{ Arr[2].Field }}`; OOB index → named Warning; named non-`size` child →
      named Warning; element member named `size` reachable at `Arr[0].size`; `data-for` over a
      non-array → 0 rows + the named `Size()` diagnostic; `data-event` assignment into an element
      refused, shadow byte-identical.
- [ ] **4.2** `FVaCuusArrayDefinition` per spec §3.5: stateless; `Size` via helper;
      `Child` order size-name → named-miss → bounds-checked index (deliberately not RmlUi's order —
      comment says why); `MakeLiteralIntVariable` for `size`.
      **Restore-the-bug for `.size`:** delete the case, watch the `{{ Arr.size }}` test fail
      through the named-miss branch's own Warning, restore.
- [ ] **4.3** Element definitions: scalar → existing `FVaCuusScalarDefinition(Inner, Kind)`;
      struct → a **root struct definition** on the element type's `FVaCuusModelDefinitions`
      (members = top-level entries, offsets absolute from element base), fetched through the same
      registry. Definition materialization pass 1 branches on Kind::Array.
- [ ] **4.4** Restate the "always handed the model base" invariant comment as "the base of the
      definition set's own type" (spec §2(d)) — in `VaCuusDataVariable.h` where it lives.
- [ ] **4.5** `Size()` overrides on struct and scalar definitions with a latched named Warning
      (the non-array `data-for` diagnostic, spec §3.5); the struct `Child` indexed-address latch
      loses its "arrays are M3b" text.
- [ ] **4.6** The three evaluation counters — `GNumScalarGets`, `GNumArraySizes`,
      `GNumArrayChilds` — `GNumRefusedSets` pattern, relaxed atomics, accessors for tests.
- [ ] **4.7** **Statelessness, made falsifiable:** two views over models sharing the same element
      `UScriptStruct`, different data, interleaved updates — each renders its own rows
      (restore-the-bug: cache a `Num()` member in the array definition, watch cross-contamination
      fail exactly this, restore); growth 0→200 in steps crossing container reallocation
      boundaries, values DOM-asserted after each step.
- [ ] **4.8** Commit: `feat: FVaCuusArrayDefinition — data-for over UPROPERTY arrays`

---

### Task 5: End-to-end `data-for`, idle three layers, reload

**Files:** `Private/Tests/VaCuusModelApplyTest.cpp` / `VaCuusModelTestHost.h`,
`VaCuusRender/Private/Tests/VaCuusModelViewTest.cpp` + `VaCuusModelViewTestTypes.h`.

- [ ] **5.1** DOM end-to-end: rows appear on growth in the same UI frame as the apply; rows
      disappear on shrink; one changed element → exactly that row's captured text changes, every
      other row byte-identical; front-trim → every row equals the shifted expectation (value-level
      assertions only, per spec §8).
- [ ] **5.2** Reload with live rows: model survives, rows rebuild, updates continue — no reload
      between a write and its assertion.
- [ ] **5.3** Idle, three layers, exact: bound unchanging 200-row model over the settled window →
      `GetNumPublishes` delta 0, fields-applied delta 0, evaluation-counter delta 0.
- [ ] **5.4** Commit: `test: data-for end to end — growth, shrink, one-row change, triple idle`

---

### Task 6: The measurements

**Files:** `Private/Tests/VaCuusModelSamplerTest.cpp`, `Private/Tests/VaCuusModelCostTest.cpp`,
a small counting-`FMalloc` proxy in the test tree (imitate `FMallocPoisonProxy`, chain over
`GMalloc` for the window — spec §9).

- [ ] **6.1** 200×4 killfeed fixture (3 `FString` + bool), asserted by count.
- [ ] **6.2** Game side (Sampler.Cost pattern): idle diff; one-element change (store + publish);
      all-changed. Warm-up first; timers bracket sample+publish only.
- [ ] **6.3** Allocation counts via the proxy: warm same-`Num` republish with unchanged strings →
      0 container reallocations, ~0 element allocations (small bound, quiesced window); the 2.2
      SyncCopy assertion lands here too.
- [ ] **6.4** UI side (Cost.UIApply pattern): apply cost for the changed array; re-evaluation + DOM
      for one changed row (the decision number); grow 0→200 single-frame protocol; shrink 200→0
      single-frame protocol — `SetMeasuring` around exactly the one frame, fresh contexts for a
      distribution.
- [ ] **6.5** Report all numbers via `AddInfo` **and** `UE_LOG` (headless reads the log), with the
      spec's tripwire targets; the controller writes them into spec §9 and takes the
      bit-granularity decision (spec §9: fits → ship, `VaCuus-akj.18` stays parked).
- [ ] **6.6** Commit: `test: the 200-row numbers M3b exists to take`

---

### Task 7: Dump, demo, docs

**Files:** `Private/VaCuusBoundModel.cpp` (dump halves), `VaCuusRender/Private/VaCuusRender.cpp`,
`VaCuusRender/Private/VaCuusDemoModel.h`, `Content/DevUI/m3_demo.rml`.

- [ ] **7.1** `vacuus.DumpModel` prints arrays: `Num()` + first 8 + elision, both halves,
      struct elements via element-layout describe, scalar elements via the value-pointer form.
- [ ] **7.2** Demo killfeed per spec §7: `TArray<FKillfeedRow>` on `FVaCuusDemoModel`, pump
      appends ~1.5 s, trims above 6, `Freeze` still proves idle. AutoShot a grown and a trimmed
      state; check both against the RML by eye.
- [ ] **7.3** Commit: `feat: killfeed demo, array dump`

---

### Task 8: Acceptance

- [ ] **8.1** Full automation suite green (baseline 62 + all new); count read from
      `Saved/Logs/VcHost.log`.
- [ ] **8.2** Monolithic game target builds (`VcHost Linux Development`) — different code path,
      required before a milestone lands.
- [ ] **8.3** Headless visual run of the demo with screenshots; a normal-quit editor run for
      module teardown (ShutdownModule never runs under the automation harness).
- [ ] **8.4** Spec §9 updated with measured numbers; the granularity decision recorded.
- [ ] **8.5** Merge `m3b-arrays` → `master` (canonical repo pulls the branch), close beads.

---

## Acceptance

1. The killfeed drives end to end: one `BindModel`, per-frame `UpdateModel`, `data-for` rows track
   growth, shrink and content, on screen.
2. Every §8 spec test exists and has been seen to fail (restore-the-bug where marked).
3. Idle is exactly zero at all three layers, by counters.
4. The §9 table is filled with measured numbers and the bit-granularity decision is taken and
   written down.
5. Every new diagnostic fires visibly; no refusal is silent.
