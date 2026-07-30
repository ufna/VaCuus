# VaCuus M3 — Data Binding: UPROPERTY reflection → RmlUi data models

**Status:** design, ready for planning
**Supersedes:** the sketch in the v1 architecture spec §6, which assumed facts that the API
research disproved. Deltas are called out inline as **§6 CORRECTION**.

**Ground truth:** `docs/research/m3-api-notes/{rmlui-data-binding,ue-reflection,vacuus-publish-path}.md`
— ~640 verified `file:line` citations across the three. Every load-bearing claim below points at
one of them. Where this spec asserts something those documents do not, it is marked **[unverified]**
and carries the experiment that would settle it.

---

## 1. Goal

Gameplay data drives the UI without the UI ever touching gameplay memory, and without a game
programmer writing per-type glue.

```cpp
USTRUCT(BlueprintType)
struct FPlayerHud
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadWrite) float Health = 100.f;
    UPROPERTY(BlueprintReadWrite) FString PlayerName;
    UPROPERTY(BlueprintReadWrite) TArray<FKillfeedEntry> Killfeed;
};
```
```html
<div data-model="hud">
  <progress data-attr-value="Health"/>
  <span>{{PlayerName}}</span>
  <li data-for="e : Killfeed">{{e.Killer}} → {{e.Victim}}</li>
</div>
```

One call binds them, and the UI tracks the struct from then on.

---

## 2. The three findings that decide the architecture

**(a) RmlUi's binding API is *not* compile-time only.** `DataModelHandle::BindCustomDataVariable`
is public and non-template, and `DataVariable(VariableDefinition*, void*)` is a public constructor
over a plain polymorphic class whose `Child()` resolves a field **by string at runtime**. So a
runtime adapter over UE reflection needs **no per-type codegen and no template instantiation per
game type** — roughly four hand-written definition classes plus a registry.

**(b) RmlUi stores a raw `void*` at bind time and dereferences it on the UI thread, with no
liveness check.** The game thread owns `UObject`s and GC moves and frees them. Therefore **the
pointer RmlUi holds must point at a UI-thread-owned shadow buffer**, never at live gameplay memory.
This — not the type system — is the real constraint of the milestone.

**(c) `UPROPERTY` metadata does not exist at runtime.** Not empty: *absent*. The accessors live
inside `#if WITH_METADATA`, which is `WITH_EDITORONLY_DATA`, which a Game target compiles as `0`;
UHT does not even emit the strings. Epic says so in its own header. **§6 CORRECTION:** any design
keyed on `meta=(...)` tags is dead. Exposure is driven by `EPropertyFlags`, which *are* cooked.

---

## 3. Architecture

Three stages, each on the thread that is allowed to do it.

```
GAME THREAD                    │ CHANNEL              │ UI THREAD
                               │                      │
 sample live struct            │                      │
 diff vs previous shadow  ─────┼─ TTripleBuffer ─────►│ for each dirty bit:
 write changed fields only     │  latest-wins         │   copy field → SHADOW BUFFER
 OR dirty bits into slot       │  + generation        │   Model.DirtyVariable(topLevelName)
 publish                       │  + dirty bitset      │                    │
                               │                      │ Context::Update() ─┘ evaluates models
```

**Why not simply give RmlUi a pointer to the gameplay struct.** Finding (b). Also the sampler must
run on the game thread while RmlUi runs on another, so a shared pointer is a data race by
construction, not merely a lifetime risk.

**Why a triple buffer and not the command queue.** Per-frame gameplay data is *state*, not an
ordered event stream: only the newest value matters and repeated updates before consumption must
coalesce. The command queue has no coalescing. This mirrors the interactive-region snapshot that
already works, with producer and consumer swapped.

### 3.1 The layout — a flat, pre-resolved command list

`FVaCuusModelLayout` is built **once** per `UStruct`, and is the milestone's central object. It is
deliberately the shape Unreal's own per-frame differ (`FRepLayout`) uses, and for the same reason:
walking `TFieldIterator` per frame, or calling `FStructProperty::Identical` (which spins up a fresh
iterator per call) or `FMapProperty::Identical` (**O(n²)** — a permutation check), is how a data
binder becomes the frame's dominant cost.

Each entry is flat: resolved `FProperty*`, byte offset from the struct base, a `EVaCuusFieldKind`,
the wire name, the shadow-buffer offset, and the index of the top-level name to dirty.

**The wire name is `FField::GetAuthoredName()`, not `GetName()`.** Blueprint struct properties are
mangled `<Base>_<UniqueId>_<32hexGUID>`; `GetAuthoredName` is the runtime-safe demangler and its
header promises consistency between editor and cooked builds. A designer writing `{{Health}}` in
RCSS must not have to know the GUID.

**Property order is declaration order and is stable within a build but not across builds or
Blueprint recompiles.** Indices are therefore valid only for the lifetime of a layout — never
persisted, never sent anywhere that outlives the process.

### 3.2 Type coverage

| Kind | v1 | Note |
|---|---|---|
| numeric, `FBoolProperty` | ✅ | bitfields need care — see §5 |
| `FStrProperty`, `FNameProperty` | ✅ | |
| `FTextProperty` | ✅ | shadow the **display string**; see §5 |
| `FUtf8StrProperty`, `FAnsiStrProperty` | ✅ | new kinds; `CastField<FStrProperty>` silently misses them |
| `FEnumProperty`, `FByteProperty`-with-enum | ✅ | exposed as both name and value |
| `FStructProperty` (nested) | ✅ | recursive, depth-limited |
| `FArrayProperty` of the above | ✅ | `data-for`; see §5 on shrink cost |
| `FObjectProperty` and friends | ❌ | see below |
| `FMapProperty`, `FSetProperty` | ❌ | v1.x — RmlUi has no map view, and `Identical` is O(n²) |
| `FDelegateProperty` | ❌ | events go the other way, via `data-event-*` |

**Object properties are deliberately excluded from v1, and this is a decision rather than a gap.**
A snapshot that owns its data cannot own a `UObject*` — holding one across the channel either
races GC or requires the UI thread to participate in reference counting, which would put UObject
lifetime on a thread that must never block. The supported pattern is to project what the UI needs
into a plain struct on the game thread. This is stated in the docs, and the builder logs the
property as unsupported rather than skipping it silently.

**Unsupported properties are never silent.** The builder emits one `Warning` per property with the
type name and the reason, once per layout build.

### 3.3 What is exposed

A property is bound when it carries `CPF_BlueprintVisible` **or** `CPF_Edit`. These flags survive
cooking; metadata does not (finding (c)). An explicit opt-out and a descriptor asset for richer
control are v1.x.

### 3.4 The shadow buffer

A single allocation owned by the view's UI-thread state, laid out by the layout: POD fields inline,
strings and arrays as owning containers. RmlUi's `void*` points into it and only into it.

---

## 4. The safety property, and how it is enforced

The research found the failure mode this milestone must be built against:

> Writing the bound storage **without dirtying the variable**. `DataViews::Update` visits only views
> reached from `dirty_variables`, so the DOM never changes, the frame hashes identical, the idle gate
> withholds publication, and the screen shows a stale value **forever, with no log line**.

It is also a heisenbug: `views_to_add` is unconditionally treated as dirty, so it **looks correct
after every live reload** — the developer edits the document, sees the right number, and concludes
it works.

**Enforcement, by shape rather than by discipline: the apply loop is *driven by* the dirty bits.**

```cpp
for (int32 Bit : PublishedDirtyBits)          // iterate the bits …
{
    CopyFieldIntoShadow(Layout[Bit], …);      // … the copy lives inside …
    Model.DirtyVariable(Layout[Bit].TopLevelName);  // … and so does the dirty call.
}
```

A field that is not dirty is never copied, so "written but not dirtied" cannot be expressed. There
is no other writer of the shadow buffer; that is a class invariant, asserted, and the reason the
buffer is private to the applier.

The residual risk moves to the **game-thread diff**: a change the diff fails to detect never sets a
bit. That is why §5's per-kind diff rules are correctness requirements, not optimisations — and why
each gets a test that changes only that kind and asserts a publish.

**Conversely, the publish itself is already proven.** A dirtied variable forces a publish through
two independent legs — the text change re-lays out in the same `Context::Update()` and regenerates
geometry, so both `NewGeometry` and `ReleasedGeometry` are non-empty and the resource-traffic
condition fails the idle gate; and independently the content hash moves, because the recorder never
recycles handles. Same argument the codebase already makes for `:hover`, and the existing
`VaCuus.Render.IdleGate.HoverRecolour` test is its end-to-end assertion.

---

## 5. The per-kind diff rules (correctness, not optimisation)

- **Bitfield bools.** Storage is the whole containing integer plus a field mask, and adjacent
  bitfields share an offset and element size; `CopyValuesInternal` read-modify-writes the
  destination byte. **A `memcmp` of a scratch buffer gives false positives.** Compare through
  `GetPropertyValue_InContainer`.
- **`FText`.** `FTextProperty::Identical` compares *identity*, not display string, when
  `GIsEditor == false` — i.e. exactly in the builds that ship. Shadow and compare the **display
  string**.
- **`FString` / `FName`.** Direct comparison; `FName` compares as index.
- **Nested structs.** Flattened into the layout at build time; never `FStructProperty::Identical`.
- **Arrays.** Length first, then element-wise through the flattened element layout. `data-for`
  reuses elements and appends/truncates at the tail, so growth is cheap; **shrink is the expensive
  direction** and RmlUi's own code self-flags it. Large arrays get a documented budget, not a
  silent cliff.
- **Never `ExportTextItem`.** Removed in 5.8 → `ExportTextItem_Direct`. And the near-identically
  named `ExportText_Direct` **silently writes nothing** when the value equals its delta, so passing
  `nullptr` yields an empty string for every default-valued field.

---

## 6. Threading contract

| Stage | Thread | Why |
|---|---|---|
| Build layout | any | Type descriptors are immutable once linked; native `UClass`/`UScriptStruct` are GC-root-flagged. **But** `UUserDefinedStruct`/`UBlueprintGeneratedClass` are *not* native and are collectable — a layout over a Blueprint type must hold a strong reference or be rebuilt on recompile. |
| Sample + diff | **game thread only** | Instance data has *zero* engine synchronisation; reading a live `UObject` while the game thread writes is a plain data race. |
| Ship | any | The snapshot is a value copy owning its strings, retaining no `UObject*`. |
| Apply + dirty | **UI thread only** | Everything RmlUi. Asserted. |

---

## 7. Public API

```cpp
// C++ and Blueprint
UVaCuusView::BindModel(FName ModelName, UScriptStruct* Type);   // once
UVaCuusView::UpdateModel(FName ModelName, const void* Data);    // per frame; diffs internally
UVaCuusView::MarkModelDirty(FName ModelName, FName FieldPath);  // fine-grained escape hatch
```

Blueprint gets `CreateModelFromStruct` / `UpdateWholeModel` with a wildcard struct pin, matching
the ergonomics of the category leader.

**Model lifetime: created once, never removed.** `RemoveDataModel` is a one-way door — `data-model`
is read only in `Element::SetParent` and `Element::SetDataModel` is private, so a still-loaded
document is permanently detached afterwards. There is **no unbind API at all**; re-binding a name
warns and keeps the stale pointer. The API therefore offers no unbind, and says why.

**On document reload the correct action is nothing.** The context, the model and its values all
survive; only element-attached `DataView`s are destroyed and rebuilt, and RmlUi re-initialises
those itself. Tearing the model down races the load and can leave the document unbound with only an
RmlUi error to show for it — and that error is invisible (§8).

---

## 8. Diagnostics, because the library's own are gone

**Every RmlUi assert and error log is compiled out in every build we produce.** `NDEBUG` leaves
`RMLUI_DEBUG` undefined, and `RMLUI_LOG_TYPE_ERROR` is defined as the assert macro. The worst
instance sits in the machinery this milestone uses: double-registering a struct returns a null
handle **silently**, and the next member registration null-derefs.

Therefore VaCuus checks its own preconditions and logs through `LogVaCuus`:

- duplicate model name, duplicate member, unsupported property kind, layout/shadow size mismatch,
  a bind whose type does not match the layout;
- each model gets its **own `DataTypeRegister`** via `CreateDataModel(name, register)`, so a reload
  cannot trip the silent double-registration path;
- `vacuus.DumpModel <view> <model>` prints the layout, the shadow values and the dirty state — the
  observable the stale-value bug needs, because a milestone whose failure mode is *no output at all*
  must ship the tool that shows the pipeline is alive.

Tracked separately as a plugin-wide question: whether Development builds should define `RMLUI_DEBUG`.

---

## 9. Budgets

The game-thread gate is **≤0.10 ms/frame total**, currently met with ~25× headroom — but the
existing measurement is an inference from margin, not complete coverage, and the sample+diff lands
squarely inside it.

| | Budget | Note |
|---|---|---|
| Game-thread sample + diff, 64-field struct | ≤0.02 ms | inside the existing gate, not additional to it |
| UI-thread apply + dirty | ≤0.05 ms | RmlUi's own measured cost is ~0.7 µs per dirtied variable |
| Idle (no field changed) | **0 published frames** | the gate must still hold; a bound model must not by itself defeat it |

The last row is a correctness gate wearing a performance costume: if merely *having* a model
publishes every frame, the milestone has silently undone M2's central result.

`RunFrame()` currently has **no perf scope at all**, so the apply would be unmeasurable where it
lands. Adding those scopes is a prerequisite task, not a follow-up.

---

## 10. Testing

- **Unit, per property kind:** change exactly one field of one kind, assert exactly its bit sets.
  Bitfield bools, `FText` in a cooked-like configuration, and `FName` each get their own case —
  these are §5's correctness rules and each has a plausible wrong implementation that passes a naive
  test.
- **The stale-value regression, restore-the-bug:** write the shadow without dirtying and assert the
  frame is withheld and the value stale — then assert the shipped design cannot express it.
- **Idle:** a bound, unchanging model publishes nothing over N frames.
- **Reload:** a model survives a document reload and keeps updating, with no rebind.
- **End-to-end through a real `Rml::Context`:** a `{{Field}}` expression shows the new value after
  an update. The idle-gate tests learned this lesson already — five of them drove the recorder
  directly and would not have caught RmlUi changing behaviour.
- **Blueprint-authored struct:** wire names are authored names, not mangled ones.

---

## 11. Risks

| Risk | Mitigation |
|---|---|
| Stale-value bug (§4) | Structurally unexpressible; plus a restore-the-bug test |
| A diff rule misses a change | Per-kind tests; the three known-sharp kinds each get their own |
| Blueprint type recompiled under a live layout | Layout holds a strong ref; rebuild on recompile. **[unverified]** — needs an experiment recompiling a BP struct with a bound model live |
| Array shrink cost on a large `data-for` | Documented budget; measure at 200 rows before committing to a v1 number |
| Game-thread budget eaten by the diff | Flat command list from the start; measured per task, not at the end |
| RmlUi's silent failures | §8 |

---

## 12. Out of scope for M3

JS access to models (M4 — the models are the same objects, so no rework), maps/sets, object
properties, a descriptor asset for per-property control, and two-way binding from UI back into
gameplay (`data-event-*` handlers exist; routing them to Unreal delegates is M4's surface).
