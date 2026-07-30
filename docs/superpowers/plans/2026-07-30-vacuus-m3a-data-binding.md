# VaCuus M3a — Data Binding Implementation Plan

> **For agentic workers:** execute task-by-task with a fresh subagent per task and a two-stage
> review (spec compliance, then code quality) after each. Steps use `- [ ]` for tracking.

**Goal:** a `USTRUCT`'s reflected scalar properties drive an RmlUi data model, one call to bind,
updates tracked per frame, with no RmlUi call off the UI thread and no gameplay pointer reaching it.

**Spec:** `docs/superpowers/specs/2026-07-30-vacuus-m3-data-binding.md` (v2).
**Ground truth:** `docs/research/m3-api-notes/{rmlui-data-binding,ue-reflection,vacuus-publish-path}.md`.

**Environment:** work in `/w/Unreal/VcHost/Plugins/VaCuus` on a branch `m3a-data-binding` cut from
`master`. Build/test commands and the eight dev-loop hazards are in `CLAUDE.md` — read that section
before the first build. Baseline: **38 tests**.

**Non-negotiables, from the spec:**
- Every RmlUi call on the UI thread. Every cross-thread entry point asserts its thread.
- The shadow buffers are real `UScriptStruct` instances (§3.1) — this is what keeps the adapter small.
- The apply loop is **driven by** the dirty bits (§4).
- Wire names are `GetAuthoredName()`, validated against RmlUi's legality rule (§3.3).
- Arrays are **M3b**. A layout that meets an `FArrayProperty` logs and skips it.

---

### Task 0: Perf scopes on the UI thread frame — prerequisite

**Files:** `Source/VaCuus/Private/VaCuusUIThread.cpp`, `Source/VaCuus/Public/VaCuusStats.h/.cpp`.

`RunFrame()` has **no perf scope at all**, so `DrainCommands` (full document parse + layout) and
`DrainInput` (hit-testing, focus, IME) are outside every measurement — and that is exactly where the
data apply lands. Adding the scopes after the apply would make its cost unattributable.

- [ ] **0.1** Add `VACUUS_PERF_SCOPE` around each phase of `RunFrame`: `DrainCommands`, `DrainInput`,
      and a `DataApply` scope reserved for Task 6. Follow the existing scope pattern exactly.
- [ ] **0.2** Extend the PerfLog line and `stat vacuus` so the new scopes appear alongside
      Update/Record/Replay/Composite.
- [ ] **0.3** Measure the static M1 HUD before and after; confirm the scopes themselves are below the
      noise floor. Record the numbers — they are the baseline every later task is judged against.
- [ ] **0.4** Commit: `perf: measure the UI thread's drain phases (prereq for M3a)`

---

### Task 1: `FVaCuusModelLayout` — the flat, pre-resolved description

**Files:** create `Source/VaCuus/Public/VaCuusModelLayout.h`, `Private/VaCuusModelLayout.cpp`,
`Private/Tests/VaCuusModelLayoutTest.cpp`.

Built once per `UScriptStruct`. One entry per bound field: resolved `FProperty*`, wire name,
top-level name index, `EVaCuusFieldKind`. **Nested structs flattened at build time.** No
`TFieldIterator` after build.

- [ ] **1.1** Write the failing test first: a fixture struct with one field of each supported kind
      (`float`, `int32`, `bool`, a **bitfield** bool, `FString`, `FName`, `FText`, `FUtf8Str`,
      `FAnsiStr`, an enum, a nested struct) produces exactly the expected entries in declaration
      order.
- [ ] **1.2** Walk with `PropertyLinkNext` (it is the cheap hot form and is built *from*
      `TFieldIterator`'s default order). Note `EFieldIterationFlags::Default` includes **deprecated**
      properties — decide and state whether to skip them.
- [ ] **1.3** Exposure: bind on `CPF_BlueprintVisible || CPF_Edit`. Both survive cooking; metadata
      does not.
- [ ] **1.4** Wire names from `FField::GetAuthoredName()`. Test asserts
      `GetAuthoredName() == chop(GetName())` for a Blueprint-style mangled name rather than comparing
      to a literal — the editor and cooked implementations differ for a never-renamed member.
- [ ] **1.5** **Name legality (spec §3.3).** Validate each wire name against RmlUi's rule: first char
      a letter, rest `[A-Za-z0-9_]`, and reject the reserved set `{it, it_index, ev, true, false,
      size, literal}` **case-insensitively**. A rejected property logs one `Error` naming the property
      and the reason, and is omitted. Test: a property called `Size` is refused with a diagnostic.
- [ ] **1.6** Unsupported kinds (`FArrayProperty` — M3b, `FMapProperty`, `FSetProperty`, hard
      `FObjectProperty`, `FDelegateProperty`) log one `Warning` each with the type name. Never silent.
- [ ] **1.7** Soft/weak object refs resolve to a path string — value types, no GC ownership.
- [ ] **1.8** Blueprint types are collectable: hold a strong reference to the `UScriptStruct`.
- [ ] **1.9** Commit: `feat: FVaCuusModelLayout — flat pre-resolved reflection walk`

---

### Task 2: The shadow buffers

**Files:** create `Source/VaCuus/Private/VaCuusModelShadow.h/.cpp`; test in the Task 1 test file.

A `FVaCuusModelShadow` owns `Malloc(GetStructureSize())` + `InitializeStruct`, and
`DestroyStruct` + `Free` in its destructor. Two exist per model: game-side previous-value and
UI-side bound.

- [ ] **2.1** Failing test: construct, write a field, read it back, destroy — no leak under the
      automation memory check.
- [ ] **2.2** Implement. Non-copyable, movable.
- [ ] **2.3** Document in the header **why** it is a real struct instance and not packed bytes
      (§3.1), and that it is therefore **invisible to GC** — which is why hard object properties can
      never enter it.
- [ ] **2.4** Commit: `feat: model shadow buffers as real USCRIPTSTRUCT instances`

---

### Task 3: The RmlUi adapter

**Files:** create `Source/VaCuusRml/...` or `Source/VaCuus/Private/VaCuusDataVariable.h/.cpp`
(whichever keeps RmlUi includes private — check how the existing document host does it).

Four definition classes over RmlUi's runtime-dispatched path, plus a **process-wide, UI-thread-only**
registry keyed on `FProperty*`.

- [ ] **3.1** `FVaCuusStructDefinition` — `Child()` resolves a member **by string** through the layout.
- [ ] **3.2** `FVaCuusPropertyDefinition` — derives from RmlUi's `BasePointerDefinition`;
      `DereferencePointer(base)` is `Property->ContainerPtrToValuePtr<void>(base)`. Deriving gets
      `Get/Set/Size/Child/ReflectMemberNames` and the null guard for free, and forwards the correct
      variable type.
- [ ] **3.3** Scalar definitions per `Rml::Variant` mapping, one per `EVaCuusFieldKind`.
- [ ] **3.4** **`Set()` refuses** — returns `false` and logs once per address (spec §4/I3). Test: a
      `data-event` assignment leaves the shadow unchanged and logs.
- [ ] **3.5** The registry is asserted UI-thread-only.
- [ ] **3.6** Commit: `feat: runtime RmlUi variable definitions over UE reflection`

---

### Task 4: The channel

**Files:** `Source/VaCuus/Public/VaCuusViewStatus.h`, `Private/VaCuusModelChannel.h/.cpp`.

- [ ] **4.1** Failing test **first**, and it is the spec's I2 restore-the-bug: publish twice without
      consuming, then consume, and assert **no field regresses**. This must fail against the naive
      "OR bits into the slot" design.
- [ ] **4.2** Implement: `TTripleBuffer` built in place via `GetWriteBuffer()` (never `Write()`,
      which takes by value); **its own generation counter** — `FVaCuusViewStatus` has none, the
      existing `Generation` belongs to the interactive snapshot; the pending-dirty set on the **game
      thread**; every publish writes the **current** value of every pending field; bits clear only on
      an echoed applied-generation, release/acquire, mirroring the document loader's load-serial.
- [ ] **4.3** Enqueue, never `Trigger()` — the input queue's rule.
- [ ] **4.4** Commit: `feat: model update channel (latest-wins, echo-cleared dirty set)`

---

### Task 5: Game-thread sample and diff

**Files:** `Source/VaCuus/Private/VaCuusModelSampler.h/.cpp` and tests.

- [ ] **5.1** A test per kind: change exactly one field, assert exactly its bit sets, nothing else.
      Write all of them failing first.
- [ ] **5.2** Per-kind rules from spec §5, each a correctness requirement:
      **bitfield bools** compared through `GetPropertyValue_InContainer`, never `memcmp` of a scratch
      buffer (adjacent bitfields share an offset and element size); **`FText`** compared by **display
      string**, because `Identical` selects identity comparison when `!GIsEditor`; `FName` as index;
      soft/weak refs as path strings; never `ExportTextItem` (removed in 5.8) and never
      `ExportText_Direct` with a null delta (it writes nothing for default-valued fields).
- [ ] **5.3** `check(IsInGameThread())`. Must be driven from `UVaCuusSubsystem::Tick` to land inside
      the existing `GameTick` perf scope.
- [ ] **5.4** **I1: the forced full-bit first publish** — otherwise frame 1 sets no bit for a field
      whose value equals the shadow's zero-init and the UI shows `0` until it happens to change.
      Restore-the-bug test: a struct whose defaults differ from zero shows its defaults on frame 1.
- [ ] **5.5** Commit: `feat: per-frame sample and diff with per-kind comparison rules`

---

### Task 6: UI-thread apply

**Files:** `Source/VaCuus/Private/VaCuusUIThread.cpp`, the document host.

- [ ] **6.1** Apply at the existing `// (data snapshots: M3)` marker — after command and input drain,
      **before** `Context::Update()` — over **every** host, not only recordable ones. The record loop
      is gated on `HasView()`, which every UMG view fails until its first Slate tick; the apply must
      not be inside it. Comment must say why.
- [ ] **6.2** The loop is **driven by the bits**: `for each set bit → copy field into the UI shadow →
      DirtyVariable(top-level name)`. The copy lives inside the dirtying loop; there is no other
      writer of the shadow, and that is a class invariant with an assert.
- [ ] **6.3** Echo the applied generation back for Task 4's bit clearing.
- [ ] **6.4** Use the `DataApply` scope from Task 0.
- [ ] **6.5** Test: a bound, unchanging model publishes **nothing** over 100 frames — the spec's
      correctness gate wearing a performance costume.
- [ ] **6.6** Commit: `feat: dirty-driven model apply on the UI thread`

---

### Task 7: Public API

**Files:** `Source/VaCuus/Public/VaCuusView.h/.cpp`, `VaCuusSubsystem`.

- [ ] **7.1** `BindModel(FName, const UScriptStruct*)` and
      `UpdateModel(FName, const UScriptStruct*, const void*)`. **The type is a parameter** — untyped
      made "wrong type" and "wrong size" undiagnosable, and the first `FString` field turns that into
      a crash.
- [ ] **7.2** Specified wrong-call behaviour: type mismatch → `Error`, no write; null → `Warning`;
      before bind → `Warning`; wrong thread → `check(IsInGameThread())`; dead view → existing gate.
      A test per case.
- [ ] **7.3** Blueprint: `CreateModelFromStruct` / `UpdateWholeModel` via CustomThunk with a wildcard
      struct pin, which supplies the type for free.
- [ ] **7.4** Header states the **bind-before-`LoadDocument`** ordering requirement and why
      (`data-model` resolves once, in `SetParent`), that there is **no unbind** and why, and that the
      correct action on reload is **nothing**.
- [ ] **7.5** Reload test: a model survives a document reload and keeps updating with no rebind.
      **Do not reload between a write and its assertion** — reload masks a missing dirty flag.
- [ ] **7.6** Commit: `feat: BindModel/UpdateModel on UVaCuusView, C++ and Blueprint`

---

### Task 8: Diagnostics

**Files:** `Source/VaCuusRender/Private/VaCuusRender.cpp` (console commands live there).

The library's own diagnostics are compiled out in every configuration we build, so ours are all
there is.

- [ ] **8.1** `vacuus.DumpModel <view> <model>` prints the layout, both shadows, the pending and
      published dirty sets, and the last applied generation.
- [ ] **8.2** Verify every diagnostic from §8 fires: duplicate model name, duplicate member,
      unsupported kind, illegal wire name, layout/type mismatch, refused `Set`.
- [ ] **8.3** Commit: `feat: vacuus.DumpModel and the model diagnostics`

---

### Task 9: Acceptance

- [ ] **9.1** Extend `Content/DevUI/m2_demo.rml` (or a new `m3_demo`) with a `data-model` panel
      bound to a live struct; a `vacuus.M3Demo` command drives it.
- [ ] **9.2** **End-to-end through a real `Rml::Context`:** `{{Field}}` shows the new value after an
      update. Six of the seven existing idle-gate tests drive the recorder directly and would not
      catch RmlUi changing behaviour.
- [ ] **9.3** Measure against spec §9: game-thread sample+diff for 64 scalar fields; UI-thread copy +
      `DirtyVariable`; **the re-evaluation cost inside `Context::Update()`**, which is the dominant
      new cost and was budgeted nowhere in v1; and idle → **0 published frames**.
- [ ] **9.4** Settle the two **[unverified]** items: the "~0.7 µs per dirtied variable" figure (which
      cites nothing anywhere), and what happens when a Blueprint struct is recompiled under a live
      layout.
- [ ] **9.5** Update spec §9 with measured numbers. Report them; the controller edits the spec.
- [ ] **9.6** Commit, then merge to master.

---

## Acceptance

1. A `USTRUCT` drives a document with one `BindModel` call and per-frame `UpdateModel`.
2. All three §4 invariants hold, each with its own restore-the-bug proof.
3. Idle costs **0 published frames**.
4. Game-thread cost stays inside the ≤0.10 ms gate; the UI-thread apply and the re-evaluation it
   causes are measured and inside §9.
5. Every diagnostic in §8 fires and is visible.
