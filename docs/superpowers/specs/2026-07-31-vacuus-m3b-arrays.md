# VaCuus M3b — Data Binding: arrays and `data-for`

**Status:** design v2, ready for planning. v1 was reviewed adversarially by three independent
passes (source fidelity, design attack, completeness) and came back **NEEDS REWORK**; §12 records
what v1 got wrong, because — as with the M3 spec — the reasons are the most useful part.

**Scope:** `TArray` UPROPERTYs drive RmlUi `data-for`, with `{{ Arr.size }}`, indexed access and
struct rows — on top of M3a's pipeline. M3a's spec said everything here "depends on a measurement
nobody has taken" (M3 spec §12); this document turns that into a design whose open questions are
exactly the measurements, and nothing else.

**Ground truth:** `docs/research/m3-api-notes/{m3b-impl-map,m3b-ue-arrays,m3b-rmlui-arrays}.md`
(written 2026-07-31 against the code on this disk), plus the M3a notes they extend. Claims not
supported by read source are marked **[unverified]** and carry the experiment that settles them.

---

## 1. Goal

```cpp
USTRUCT(BlueprintType)
struct FKillfeedRow
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadWrite) FString Killer;
    UPROPERTY(BlueprintReadWrite) FString Victim;
    UPROPERTY(BlueprintReadWrite) FString Weapon;
    UPROPERTY(BlueprintReadWrite) bool bHeadshot = false;
};

USTRUCT(BlueprintType)
struct FHudModel
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadWrite) TArray<FKillfeedRow> Killfeed;
};
```
```html
<div data-model="hud">
  <span>{{ Killfeed.size }} recent kills</span>
  <div data-for="kill : Killfeed">
    <span>{{ kill.Killer }}</span> ⟶ <span>{{ kill.Victim }}</span>
    <span data-if="kill.bHeadshot">HS</span>
  </div>
</div>
```

Same one-call contract as M3a: `BindModel` once, `UpdateModel` per frame, the rows track the array.

---

## 2. The findings that decide the architecture

**(a) `CopySingleValue` on an `FArrayProperty` is a correct deep copy — and a wasteful one, whose
waste is structural.** It is the virtual `CopyValuesInternal`: destination **emptied** — every
non-POD element *destroyed*, its heap freed — then resized and per-element
`Inner->CopyCompleteValue` into freshly default-constructed elements
(PropertyArray.cpp:1260-1328; `EmptyAndAddValues` → `EmptyValues` → `DestructItems` →
`DestroyValue` per element, UnrealType.h:4348-4353, :4459-4471, :4595-4611). So a same-size
republish through it can never reuse an element's buffer: for the 200×3-`FString` killfeed that is
~600 frees + ~600 allocations **per copy stage**, and the container block itself is reallocated on
*any* `Num` change in either direction (`Empty(Slack)` reallocates whenever `Slack != ArrayMax` —
ScriptArray.h:137-148, :264-275). Correctness comes free; the steady state does not. Hence §3.3's
`SyncCopy`, the one new copy primitive of the milestone. (`check(Count == 1)` at
PropertyArray.cpp:1262 plus UHT's refusal of static arrays of containers, UhtProperty.cs:2390-2393,
mean Single and Complete coincide for arrays — there is no C-array-of-TArray case to carry.)

**(b) RmlUi communicates both size change and element change the same way** — `DirtyVariable(root)`,
because `DataViewFor::GetVariableNameList` reduces to the container's top-level name
(DataViewDefault.cpp:545-549) and the view map keys on root names only (`GetVariableNameList`
collapses every address to `address[0].name`, DataExpression.cpp:1145-1155; the map keying is
DataView.cpp:82-83). There is no finer signal to send. **One dirty bit per top-level array is
therefore not a simplification but the entire expressible vocabulary**; per-element bits could only
ever reduce *copy* cost, never RmlUi work, and whether the copy needs reducing is §9's measurement
(the refinement sketch is bead `VaCuus-akj.18`).

**(c) Element addresses are computed, never stored — on both sides.** RmlUi re-walks
`root → Child(...)` from the bind-time pointer on every evaluation of every path that can reach an
array: `data-for` update, `{{ Arr.size }}`, static `{{ Arr[3].F }}`, dynamic `{{ Arr[i] }}`
(m3b-rmlui-arrays.md §1). On the UE side `FScriptArrayHelper::GetRawPtr(i)` is
`GetData() + i*ElementSize` computed at call time (UnrealType.h:4332). So the "address valid only
for the current Num()" hazard that made arrays impossible for a flat offset entry disappears when
**no stage stores an element address** — a stated invariant with its own restore-the-bug test (§8),
not a review promise.

**(d) The adapter's own invariants already name the extension point.** The M3a struct definition is
always handed the base of the type its definition set was built over, and applies a leaf's absolute
offset exactly once (VaCuusDataVariable.h:60-69 — the header currently says "the model base";
M3b restates it as **"the base of the definition set's own type"**, because an element-rooted
struct definition is handed `GetRawPtr(i)`, which *is* that base). Both upstream definition classes
are `final` (VaCuusDataVariable.h:80-81), so the array definition is hand-rolled like the struct
one was. The scalar definition already operates on value pointers (it receives
`ContainerPtrToValuePtr` output — VaCuusDataVariable.cpp:259-265), so **it serves array elements
unmodified**: `GetRawPtr(i)` *is* a value pointer.

**(e) A `TArray<bool>` element is always a native bool.** UHT rejects bool static arrays outright
and bitfields cannot exist inside a container (UhtProperty.cs:2395-2398; m3b-ue-arrays.md §4-5), so
the one comparison that must never run on a raw byte — the mask-aware bitfield read — cannot arise
per element. Element compares run on plain value pointers.

---

## 3. Architecture

Everything M3a built runs unchanged: the channel protocol, the apply loop, the echo, the publish
coalescing, the definition registry. M3b adds one field kind, one definition class, one element
descriptor, one copy primitive, per-element comparison, and three evaluation counters.

### 3.1 The layout: an array is a leaf with a sub-description

`EVaCuusFieldKind::Array` is a new kind. The array's `FVaCuusModelField` entry is a **leaf** — one
entry, one dirty bit, addressed by `ContainerOffset` + `ContainerPtrToValuePtr` like every other
leaf, legal at top level or nested inside a flattened struct (`Panel.Items`). What it cannot be is
*flattened*: leaf count is fixed at build time, element count is per-instance (m3b-impl-map.md §9.6).
So the entry carries `int32 ArrayDescIndex` into a new side table:

```
FVaCuusModelArrayDesc
    const FArrayProperty* ArrayProperty;
    EVaCuusFieldKind      ElementKind;      // scalar kinds, or Struct
    const FProperty*      Inner;            // ArrayProperty->Inner; Offset_Internal == 0, stride == GetElementSize()
    TUniquePtr<FVaCuusModelLayout> ElementLayout;  // struct elements only
```

**The element layout is genuinely plain and shared — element-context policy lives on the array
field, not in the element layout.** A struct element's layout is an ordinary `FVaCuusModelLayout`
over the element `UScriptStruct`: same flattening, same classifier, same per-member diagnostics,
same `TStrongObjectPtr` pinning. This is forced, not chosen: the definition registry keys on the
raw `UScriptStruct*` (VaCuusDataVariable.cpp:29), so a type used both as a model root and as a row
type gets **one** layout and one definition set — two divergent policies per type cannot exist.
The consequences are owned explicitly:

- **Element top-level member names obey the full root rule**, including the reserved set. A row
  member named `Size` is refused with the same Error a root member gets, even though it would
  resolve through `Child`. That is the price of sharing one layout per type; the refusal is loud,
  the workaround is a rename, and an element-mode name pass (requiring a compound registry key) is
  deliberately not built until someone needs it. Nested members inside the element keep the weaker
  `ValidateNested` rule, as in M3a.
- **What a shared layout cannot refuse, the array-desc build refuses.** At array-desc build time
  the element layout's *flat leaf list* is scanned once: any leaf of kind `Text`, or any leaf of
  kind `Array` (a nested container anywhere in the **bindable** element subtree — flattening
  surfaces every bindable leaf within `MaxNestingDepth`, and only those) → **the whole array field
  is refused**, with one Warning naming the array property, the offending element member and the
  reason. The element layout itself stays valid for its other uses. What the scan cannot see rides
  along inert: an unexposed, deprecated, editor-only, illegally named or over-deep member has no
  leaf, so it neither surfaces nor refuses the array — `SyncCopy`'s whole-row `CopyCompleteValue`
  copies it with its row (`CopyScriptStruct`'s property loop applies no exposure filter,
  Class.cpp:3697-3731), and payload with no leaf is never read and never diffed: a hidden `FText`
  copies as an atomic refcount bump and resolves no localization; a hidden hard `UObject*` copies
  as bytes no stage dereferences.
- **Two refusals run before the scan.** (1) *Container cycles*: a mutual pair
  `FA{TArray<FB>}`/`FB{TArray<FA>}` or a by-value hop `FRow{FSub}`/`FSub{TArray<FRow>}` would
  recurse the layout build to a stack overflow, since every element layout restarts the depth
  count at 0. UHT refuses every *native* writing of the shape — the direct one explicitly
  (UhtArrayProperty.cs:216-222), the indirect ones at the forward reference they need (zero
  code-generation hash, UhtProperty.cs:3066-3071) — but nothing validates the `FProperty` graph
  itself, which a runtime-built `UUserDefinedStruct` assembles freely. A build stack of
  in-progress root types threads through the element-layout constructors; an element type already
  on the stack refuses the array field *before* its layout is constructed, with one Warning naming
  the array property and the cycle. (2) *Zero-bindable
  rows*: a row type whose element layout has no fields would make `Num()` the only observable —
  row counts rendered, row content never — so the whole array field is refused with one Warning
  naming the array property and the row type; the element build suppresses the root-flavored
  "no property could be bound" line, so that Warning is the one diagnostic.

### 3.2 Element type coverage

| Element | M3b | Note |
|---|---|---|
| numeric, native bool | ✅ | bitfields impossible in containers — §2(e) |
| `FString` / `FUtf8String` / `FAnsiString` | ✅ | case-sensitive compare per element, as M3a §5 |
| `FName`, enum | ✅ | same accessors, value-pointer form |
| `FSoftObjectPtr` | ✅ | componentwise compare per element, no allocation |
| struct | ✅ | flattened inside the element via `ElementLayout`; depth limit applies |
| `FText` — as the element type **or anywhere inside a struct element** | ❌ | **the whole array field is refused with a Warning** (§3.1). M3a's Text contract — shadow and compare the *display string* — is a per-field projection at `StoreField` (VaCuusModelSampler.cpp:228-244) that a whole-container copy bypasses; an unprojected `FText` in the UI shadow would make `FVaCuusScalarDefinition::Get` resolve localization on the UI thread (VaCuusDataVariable.cpp:130), the exact race the M3a sampler pins to the game thread (VaCuusModelSampler.h:56-70). Per-element projection is a real design, deferred; the workaround (`FString`, projected by the game) is one line |
| nested `TArray` / `TMap` / `TSet` inside an element struct | ❌ | legal in UHT (m3b-ue-arrays.md §6); maps/sets are refused by the shared classifier as in M3a, nested arrays by the array-desc scan (§3.1): dirtiness is one bit per *top-level* array, so an inner array's cost multiplies invisibly under a single bit. Revisit on demand |
| hard / weak `UObject` refs | ❌ | refused by the shared classifier — same GC and thread arguments as M3a §3.4. Scope, since arrays: an *unexposed* hard reference inside a bound row is still copied into the shadows with its row, as inert never-read payload (§3.1); the enforced invariant is that no `UObject*` is ever **bound or read**, not that the shadow bytes never contain one |

**Fixed-size C arrays (`ArrayDim > 1`) stay refused, and this is the revisit M3a promised.**
Reasons, in order: (1) they cannot be Blueprint-exposed at all (UhtScriptStruct.cs:1147-1149,
UhtClass.cs:2203-2205), so the audience is C++-only code for which `TArray` is strictly more
idiomatic; (2) support would fork the copy contract — `CopySingleValue` copies element 0 only for
`ArrayDim > 1` (UnrealType.h:873-894), so `CopyValue` would need a per-kind `CopyCompleteValue`
branch for one rare shape; (3) nothing in §9's open measurement is informed by them. The refusal
Warning stays, its text updated to name this decision rather than "M3b".

### 3.3 The diff, the store, and `SyncCopy`

**Diff — one bit, first difference wins.** Per array field, `HasFieldChanged` becomes: construct
two `FScriptArrayHelper`s over the live and shadow **value** pointers (the helper takes the value
pointer, not the container — UnrealType.h:4280-4288); `Num()` mismatch → changed; otherwise compare
elements in order and **stop at the first difference** — the bit is per-array, so finding one
difference is finding them all. Scalar elements compare through the same per-kind rules as M3a's
fields, refactored into a value-pointer comparator the field-level switch also calls (bitwise
doubles, case-sensitive strings, display-index names — every M3a §5 rule, value-pointer form).
Struct elements walk `ElementLayout->GetFields()` with the element base standing in for the struct
base. The same refactor gives `DescribeValue` a value-pointer form, which the dump needs for scalar
arrays (§6).

**Store and the two other copies go through `SyncCopy`, not `CopySingleValue`.** §2(a): the
engine's whole-array copy destroys every destination element first, so it structurally cannot reuse
element buffers. `FVaCuusModelArrayDesc::SyncCopy(Dest, Src)` is the milestone's one new copy
primitive: `FScriptArrayHelper::Resize(Dest, Src.Num())` — which **preserves surviving elements'
values** (UnrealType.h:4387-4403, `AddValues`/`RemoveValues` touch only the delta; their
*addresses* may still move on a realloc, which §2(c)'s no-stored-addresses invariant already
absorbs) — then per-element `Inner->CopyCompleteValue` for non-POD inners or one `FMemory::Memcpy`
of `Num * ElementSize` for POD inners. The non-POD case is an assignment into a *live* destination
element: for `FStrProperty` it resolves to `FString::operator=` (TProperty::CopyValuesInternal,
UnrealType.h:1626-1632), whose reallocation rule is **grow-only** — `ReallocForCopy` reallocates
iff the quantized reserve of the source length exceeds the destination's capacity, else the buffer
is reused outright (Array.h:1012-1020, :710-751, `NewMax > PrevMax`). Reached through the existing `CopyValue` funnel (the field
carries its desc), so all three copy stages — sampler store, slot write, UI apply — take it without
new call sites, and the cost story becomes: **allocations only where content outgrew capacity or
`Num` grew**, at every stage. §9 measures exactly that.

**`StoreField`'s if-chain becomes an exhaustive switch** while we are in it: today a new kind
silently falls through to `CopyValue` (VaCuusModelSampler.cpp:228-244) — the Array branch must be
*chosen*, and the site gets the same `-Wswitch` protection as the other four kind switches
(m3b-impl-map.md §2 item 4 is the gap).

### 3.4 The channel and the apply: unchanged protocol, honest cost story

One bit per array; a changed array is copied **three times per change**: live→shadow (store),
shadow→slot (publish), slot→UI shadow (apply) — each a `SyncCopy`. Two consequences get stated
where the code makes them true:

- **Republish under a UI stall re-copies the whole array per game frame** (the protocol rewrites
  every outstanding field's current value every publish — VaCuusModelChannel.cpp:145-151). Still
  bounded by distinct changed fields; "a field" is now O(elements). The channel header's
  "allocation-free in the steady state" and the shadow's "paid once per model, not per frame"
  claims (VaCuusModelChannel.h:49-50, VaCuusModelShadow.h:36-38) get scoped: for scalar fields as
  written; for arrays, a republish is element *assignments* — allocation-free only where existing
  capacity absorbs the content, which `SyncCopy` makes the common case and §9 confirms with a
  counting allocator.
- **The slot's container block is reused only while the published `Num` is stable.** `SyncCopy`'s
  `Resize` grows with slack and shrinks only through `RemoveValues` (allocator-default shrink
  policy applies — ScriptArray.h:191-222); the pathological alternation case is measured, not
  assumed away.

The apply is bits→copy→`DirtyVariable`, untouched. It runs before `Context::Update()` on the same
thread, so `data-for` sees the new `Num()` and the new values in the same frame as the dirty — no
partial states are observable.

### 3.5 The RmlUi adapter: `FVaCuusArrayDefinition`

`final : Rml::VariableDefinition`, constructed as `DataVariableType::Array` (nothing in the
compiled plugin keys on it today, but it is one token and the debugger relays may come —
m3b-rmlui-arrays.md §2). Per-type, stateless, owned by `FVaCuusModelDefinitions` like its siblings:
it holds the `FArrayProperty*`, a borrowed element `VariableDefinition*`, and a diagnostic name —
**never an element pointer, never a cached `Num()`**; every `Size`/`Child` computes from the
incoming `void*` at call time (§2(c); the per-type registry sharing demands it —
VaCuusDataVariable.h:281-284 — and §8 tests it).

- `Size(ptr)` → `FScriptArrayHelper(ArrayProperty, ptr).Num()`.
- `Child(ptr, entry)`, in this order — deliberately *not* RmlUi's order (bounds first, `"size"`
  inside the OOB branch, DataVariable.h:143-163); equivalent because a named entry carries
  `index == -1`, and checking the name first keeps the two diagnostics from sharing a message:
  - named `"size"` → `Rml::MakeLiteralIntVariable(Num)`. **This is the case a hand-written
    definition breaks by omission** (M3 spec §12 carried it; RmlUi implements it inside
    `ArrayDefinition::Child`, not in the core — DataVariable.h:148-155), so it gets a
    restore-the-bug test of its own.
  - named non-`"size"` → empty variable + a latched Warning naming the model, the field and the
    child (RmlUi reuses a misleading "index out of bounds" text for this — DataVariable.h:149-155).
  - `0 <= index < Num` → `Rml::DataVariable(ElementDefinition, Helper.GetRawPtr(index))`.
  - `index >= Num` → empty variable + a latched Warning naming the model, the field and both
    numbers.
- `Get`/`Set` are the base-class failures (`Set` on a leaf under the array funnels to the scalar
  definition's refusal — I3 holds; a `data-event` assignment through `kill.Killer` or `Items[0].X`
  is refused and logged like any other).

The element definition: for scalar kinds, an `FVaCuusScalarDefinition(Inner, ElementKind)` — the
existing class, unmodified (§2(d)). For struct elements, a **root struct definition** over the
element type: an `FVaCuusStructDefinition` whose members are the element type's top-level entries
with offsets absolute from the *element* base — the shape pass 2 already builds per top-level name,
expressed once for the whole element type. Element definitions live in the element type's own
`FVaCuusModelDefinitions`, fetched through the same registry (keyed on the element
`UScriptStruct`), so two models sharing a row type share its definitions — which is exactly why
statelessness is load-bearing and tested (§8). The M3a invariant comment is restated per §2(d).

**`data-for` over a non-array gets a named diagnostic.** RmlUi's base `Size()` warns "Tried to get
the size from a non-array data type" and yields 0 rows — reaching our log but naming nothing
(m3b-rmlui-arrays.md §2). The struct and scalar definitions override `Size()` with a latched
Warning naming the model and the variable, exactly the shape of the existing indexed-address latch
(VaCuusDataVariable.cpp:316-334) — which itself keeps refusing indexed access *on structs* and
loses the "arrays are M3b" milestone reference in its text.

**Three evaluation counters** — `GNumScalarGets`, `GNumArraySizes`, `GNumArrayChilds`, the
`GNumRefusedSets` pattern (VaCuusDataVariable.cpp:30) — because §8's idle gate asserts "0
evaluations" and an invariant with no observable cannot be tested. Every evaluation of a bound leaf
terminates in `FVaCuusScalarDefinition::Get`; every array-path evaluation passes `Size` or `Child`.
One relaxed atomic increment each; the idle assertion is an exact counter-delta of zero.

### 3.6 Semantics a document author must know (documented, not fixed)

These are RmlUi's semantics, verified in the vendored copy; VaCuus documents them rather than
papering over them:

- **Row identity is positional and frozen.** `it` aliases `Arr[i]` with the creation-time `i`,
  forever; `it_index` is a frozen literal (DataViewDefault.cpp:513-521; no renumbering exists —
  m3b-rmlui-arrays.md §6 incl. its correction note). Removing from the front shifts every value
  under fixed indices: all rows re-render, tail rows disappear. A killfeed that appends at the tail
  and trims from the front pays full re-render on trim — that is the shape of §9's measurement, not
  a VaCuus bug.
- **Shrink is the expensive direction.** Every removed row's every element sweeps all views, then
  each removed view sweeps `name_view_map` — RmlUi flags its own cleanup `@performance: Horrible`
  (DataView.cpp:117-132); clearing an N-row table is quadratic in N (m3b-rmlui-arrays.md §4).
  Growth appends at the tail and creates all missing rows in one update; 200 appended rows resolve
  in two convergence iterations, nowhere near the cap of 10 — which only nesting depth or a
  keep-dirtying view chain can exhaust, and the latter is closed here by one-way binding
  (m3b-rmlui-arrays.md §5).
- **`{{ Arr.size }}` is the array's element count, always.** The routing is real — `Arr[0].size`
  hands the name to the element struct's `Child`, not the array's (ParseAddress yields the index
  and the name as separate entries — m3b-rmlui-arrays.md §7; the Task 4 test observes the element
  struct's own missing-member Warning there) — **but nothing bindable can sit at that address**:
  an element *top-level* member named `Size` is refused by the root rule §3.1 imposes on shared
  layouts (reserved word). The reachable spelling is one level deeper — `Rows[0].Panel.Size`
  renders fine, because `ValidateNested` applies no reserved-word rule. One doc line; no refusal.
  *(Corrected after implementation: v2 claimed `Arr[0].size` itself was reachable, which the
  shared-layout name rule forecloses.)*
- **`data-for` re-evaluates every view in every row on any dirty of the root.** DOM writes still
  gate on compare-before-write per view, so O(all bindings) evaluations, O(changed) writes — the
  M3a idle discrimination extends to a third layer here, now observable via §3.5's counters: an
  unchanged 200-row model must publish nothing, apply nothing, *and* evaluate nothing (no dirty →
  `DataViews::Update` runs once over an empty vector).

## 4. Threading

Nothing new. Layout and element-layout build on any thread over a linked type; sample + diff game
thread only; apply + every RmlUi call UI thread only, asserted where they were already asserted.
The one new rule is §2(c) stated as an invariant: **no stage stores an element address** — not the
definitions (stateless, per-type), not the sampler (helpers constructed per call), not the applier
(walks bits, syncs whole values). `GetRawPtr` results live for exactly one expression. §8 carries
the two tests that would catch a violation; "review checks it" is not the enforcement.

The Blueprint-recompile hazard now has **two** type objects per array-of-struct field: the model
root and the element type, both pinned, both dangling their `FProperty*`s on recompile
(VaCuus-akj.16; facts in VaCuusModelRecompileTest.cpp:19-60). The tracked refusal fix must cover
element types too — noted on the bead.

## 5. Public API

Unchanged. `BindModel`/`UpdateModel` signatures, ordering rules, wrong-call behaviour, reload
behaviour ("do nothing") — all M3a §7 verbatim; arrays ride the same calls. Blueprint arrays ride
the same CustomThunk (a `BlueprintType` struct with a `TArray` member is the already-supported
case).

## 6. Diagnostics

`vacuus.DumpModel` learns to print arrays: `Num()` plus the first 8 elements — struct elements
through the element layout's `DescribeValue`, scalar elements through §3.3's value-pointer describe
form — with an elision marker; a 200-row dump that printed every row would bury the scalar fields
it exists to show. Every new refusal (Text in an element subtree, nested container, the fixed-array
decision, the non-array `data-for` target, the named non-`size` child, index out of bounds) logs
one named line per §8's observed-diagnostic tests; per M3a's rule, never silent. One consequence
worth its own line: a model whose only field is a refused array binds as an *empty* model — M3a
already specifies and logs this ("no bindable field", VaCuusView.cpp:327-335), but arrays make it
likelier, since one refused element member empties the whole field.

## 7. The demo — killfeed end to end

`m3_demo.rml` grows a killfeed panel (`data-for` over `TArray<FKillfeedRow>` on the existing `hud`
model); `PumpDemoModel` appends a row every ~1.5 s with animated content, trims from the front
above 6 rows, and `vacuus.M3Demo.Freeze` keeps proving the idle row on a real screen. AutoShot
screenshots at a grown and a trimmed state.

## 8. Testing

Per the project standard: every correctness claim gets a test that has been seen to fail.

- **Layout:** array classified with desc + element layout; array nested in a struct (`Panel.Items`)
  addressable; every §3.2 refusal fires its Warning and is observed: Text as element type, Text
  *inside* a struct element, nested array in an element, map/set element, fixed array; element
  top-level member named `Size` refused with the root-rule Error (§3.1's stated price).
- **Sampler, restore-the-bug where marked:** change one element → exactly the array's bit;
  change the **last** element → detected (early-out cannot clip the scan); append / remove /
  clear → the bit; case-only `FString` element change → the bit **(restore: flip to
  case-insensitive compare, watch it fail)**; NaN double element → no republish-every-frame
  **(restore: value compare)**; struct-element leaf change → the bit; unchanged 200-row array →
  no bit. `SyncCopy`: shrink preserves surviving elements' values; grow constructs; a same-`Num`
  sync into a warm destination performs zero container reallocation (allocation counter, §9's
  harness).
- **Channel:** I2's no-regression proof re-run with an array field (publish twice unconsumed,
  consume, no element regresses); slot self-sufficiency with mixed scalar+array dirty sets.
- **Adapter, through a real context:** `{{ Arr.size }}` renders and tracks growth **(restore:
  delete the `"size"` case from `Child`; the address then falls to the named-child branch, VaCuus's
  own Warning fires and the rendered text breaks — the test asserts both, which also proves the
  named-miss branch)**; `{{ it }}` and `{{ it_index }}` for a scalar array; static `{{ Arr[2].F }}`;
  OOB index warns with names; a named non-`size` child warns with names; element member named
  `size` reachable at `Arr[0].size`; `data-for` over a non-array variable → 0 rows + the named
  `Size()` diagnostic; `data-event` assignment into an element refused, shadow byte-identical (I3).
- **Statelessness (the §4 invariant, made falsifiable):** (a) two views over models sharing the
  same element `UScriptStruct` with different data, updated interleaved — each renders its own rows
  **(restore: cache `Num()` in the array definition, watch cross-contamination fail exactly
  this)**; (b) growth 0→200 in steps sized to cross container reallocation boundaries, values
  asserted through the DOM probe after each step.
- **`data-for` end to end (DOM probe, Apply-test style):** rows appear on growth in the same
  UI frame as the apply; rows disappear on shrink; after a one-element change, exactly that row's
  captured text changes and every other row's captured text is byte-identical; after a front-trim,
  every row's captured text equals the shifted expectation (value-level assertions — a write-side
  no-spurious-`SetText` claim is not observable without patching vendored RmlUi, and is not
  claimed); reload with live rows → model survives, rows rebuild, updates continue.
- **Idle, three layers, all exact counters:** bound unchanging 200-row model over N settled frames
  → 0 published frames (`GetNumPublishes`), 0 fields applied (`GetNumFieldsApplied`), 0 evaluations
  (§3.5's counters, delta zero).
- **Cost:** §9's table, measured by extending the two existing cost harnesses with a 200×4 killfeed
  fixture.

## 9. Budgets — the measurement M3b exists to take

Targets are tripwires in the M3a sense (loose, 10×, catching regressions not micro-drift), except
the idle row, which is exact. 200 rows × 4 fields (3 `FString` + 1 bool), the killfeed shape.
Timing via the existing `FPlatformTime` brackets; **allocation counts via a counting `FMalloc`
proxy chained over `GMalloc` around the measured window** — imitate `FMallocPoisonProxy`
(MallocPoisonProxy.h:24-186, the Public-tree proxy shape; the engine installs proxies by exactly
this chaining, UnrealMemory.cpp:385-412). The count is process-wide, so the window runs on a
quiesced frame and the assertion is a small bound, not a literal zero of the whole process. Named
here because without it two rows below are unmeasurable.

| | Target | **Measured 2026-07-31** (7950X3D, Development editor, commit 2895ae5) |
|---|---|---|
| GT diff, idle, 200 rows | ≤0.02 ms | **0.00501 ms**/frame (2000 iters) — 4× under |
| GT one-element change: store + publish (two `SyncCopy`s) | ≤0.10 ms | **0.01021 ms**/frame (row 100, same-length string) — 10× under. All-200-changed is *cheaper*, 0.00720 ms: first-difference-wins stops that diff at row 0, while the row-100 change scans 100 clean rows first; the two `SyncCopy`s dominate both |
| UI apply (third `SyncCopy`) + `DirtyVariable` | ≤0.10 ms | **0.00428 ms**/frame — 23× under (still-context control: 0.00007 ms) |
| Re-evaluation + DOM for one changed row | ≤0.50 ms | **0.42257 ms**/frame (CHANGING−STILL; a second run gave 0.45456) — **PASS but tight, 85–91% of target: the row to watch on other machines.** It is ~100% re-evaluation: a bool-toggle row (one 1-char DOM write) measures 0.42419, indistinguishable from the string change |
| Warm same-`Num` republish, unchanged strings | **0 container reallocations; ~0 element allocations** | **Exactly 0 mallocs + 0 reallocs in all 16 windows** (8 direct `SyncCopy`, 8 channel republish); container block and all 600 element string buffers at identical addresses across 8 warm copies. Restore-the-bug: routing the funnel through the engine's `CopySingleValue` produced **600 reallocs in every window** — the counting observable is what distinguishes the copies |
| Grow 0→200 in one frame | measure, no target | Update **1.714 / 1.867 / 1.975 ms** min/med/max over 5 fresh contexts (apply itself 0.039 ms median) — the 200 × `SetInnerRML` load spike, by design |
| Shrink 200→0 in one frame | measure, no target | Update **0.202 / 0.209 / 0.456 ms** (apply 0.004 ms median) — the quadratic cleanup is not biting at N=200 |
| Idle, 200 rows bound | **0 published, 0 applied, 0 evaluated** | **PASS, exact**: the counter layer in `VaCuus.Model.DataForIdle`; the cost run's still-context adds 1 publish *ever* (the born-dirty first frame), then 0/0/0 over 200 frames |

**The bit-granularity decision, taken on these numbers: one bit per top-level array ships.**
The changed-row cost is 0.423 ms of which the three copies contribute ~0.014 ms — the rest is
RmlUi re-evaluation, which §2(b) proved no VaCuus granularity can reduce. Per-element dirty
ranges (`VaCuus-akj.18`) could shrink only the 0.014, and stay parked with these numbers attached.
The tight row is re-evaluation at 85–91% of its 0.50 ms tripwire — a document-side scaling
property (bindings × rows), not a copy-pipeline one.

## 10. Risks

| Risk | Mitigation |
|---|---|
| Copy cost at scale | §9 measures at 200 rows before acceptance; `SyncCopy` makes the steady state assignment-shaped; each copy is separately visible in the perf scopes |
| A stored element address goes stale | §4's invariant, enforced by §8's two-context and realloc-crossing tests — not by review |
| `.size` omission (the classic hand-rolled-array bug) | implemented + restore-the-bug test |
| Shrink cost surprises a user | documented in §3.6, measured in §9, demo trims from the front deliberately |
| Element kind rules drift from field kind rules | one comparator, two call sites (§3.3) — divergence requires editing shared code |
| `StoreField` silently mis-stores future kinds | the if-chain becomes an exhaustive switch (§3.3) |
| An FText smuggled in via a struct element | refused at array-desc build by the flat-leaf scan (§3.1), with its own observed test |
| BP recompile dangles element-type properties | inherited hazard, doubled surface; noted on VaCuus-akj.16 |

## 11. Out of scope

Per-element dirty granularity (`VaCuus-akj.18`, opened only if §9 fails); `FText` elements
(refused, §3.2); nested containers in elements (refused, §3.2); fixed-size C arrays (decided
refusal, §3.2); element-mode wire-name policy (compound registry key — not until someone needs
it, §3.1); maps/sets (M3a's refusal stands); JS access to array models (M4, same objects); two-way
binding (M4).

## 12. What v1 got wrong, and why it matters

1. **v1 claimed warm republishes reuse `FString` buffers, citing the very function that makes
   reuse impossible.** `CopyValuesInternal` *destroys* every destination element before assigning
   (PropertyArray.cpp:1267-1269 → `DestructItems`), so the claim was not optimistic but inverted —
   and it propagated into two §9 rows as "allocations ~0". All three review passes caught it
   independently. The fix is a design element, not a wording change: §3.3's `SyncCopy`. A cost
   story inherited from a container's *interface* still needs its *implementation* read.
2. **v1 refused `TArray<FText>` and forgot `FText` one level down.** A struct element with an
   `FText` member sailed through the plain classifier into the exact UI-thread localization race
   the refusal exists to prevent — the milestone's signature failure class, shipped by the spec's
   own test list. The fix (§3.1) also dissolved a second latent bug v1 hadn't seen: the registry
   cannot carry two policies for one type, so element layouts must be *identical* to root layouts,
   and every element-specific refusal must live on the array field.
3. **v1's "0 evaluations" gate had no observable** — verbatim the M2 lesson the project's
   conventions record. Three counters (§3.5) make it exact; the cost-subtraction pattern v1 named
   measures *time*, and can assert nothing exactly.
4. **v1's statelessness invariant was "asserted by review"** — the enforcement the project
   explicitly rejects. §8 now carries the two tests that fail if it breaks.
5. **v1 asserted a DOM-write property no observable could see** (spurious identical-text writes).
   Reworded to value-level assertions that can fail.
6. Smaller but the same species: the allocation rows were unmeasurable (no allocator hook named),
   the backlog bead v1 cited did not exist (it does now: `VaCuus-akj.18`), and the element-name
   rule contradicted the layout code it claimed to reuse.
