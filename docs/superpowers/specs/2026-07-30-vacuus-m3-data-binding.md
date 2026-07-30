# VaCuus M3 — Data Binding: UPROPERTY reflection → RmlUi data models

**Status:** design v2, ready for planning. v1 was reviewed adversarially and came back
**NEEDS REWORK** with six blocking items; this is the rewrite. What changed is listed in §13, because
the *reasons* three of those items were wrong are the most useful thing in the document.

**Scope:** this spec covers **M3a — the channel and scalar binding**. Arrays and `data-for` are
**M3b**, split out on the review's recommendation; the seam and the reason are in §12.

**Ground truth:** `docs/research/m3-api-notes/{rmlui-data-binding,ue-reflection,vacuus-publish-path}.md`
plus the review's own corrections to those documents. Claims not supported by read source are marked
**[unverified]** and carry the experiment that settles them.

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
    UPROPERTY(BlueprintReadWrite) FVector2D Crosshair;   // nested struct, flattened
};
```
```html
<div data-model="hud">
  <progress data-attr-value="Health"/>
  <span>{{PlayerName}}</span>
</div>
```

One call binds them, and the UI tracks the struct from then on.

---

## 2. The findings that decide the architecture

**(a) RmlUi's binding API is not compile-time only.** `DataModelHandle::BindCustomDataVariable` is
public and non-template; `DataVariable(VariableDefinition*, void*)` is a public constructor over a
plain polymorphic class whose `Child()` resolves a field **by string at runtime**. `Family<T>` and
the type register are bypassed entirely on this path. No per-type codegen.

**(b) RmlUi retains a raw `void*` and dereferences it on the UI thread with no liveness check.**
`DataVariable` owns neither of its two pointers; the only null check anywhere is
`BasePointerDefinition`'s on its own pointer, and `ScalarDefinition<T>::Get` has none. Nothing ever
revalidates. **So the pointer must address a UI-thread-owned shadow buffer, never live gameplay
memory.** This is the organising constraint of the milestone — not the type system.

**(c) `UPROPERTY` metadata does not exist at runtime.** Absent, not empty: the accessors are inside
`#if WITH_METADATA` == `WITH_EDITORONLY_DATA`, a Game target compiles it as `0`, and UHT does not
emit the strings. Exposure runs off `EPropertyFlags`, which survive cooking.

---

## 3. Architecture

```
GAME THREAD                      │ CHANNEL              │ UI THREAD
                                 │                      │
 sample live struct              │                      │
 diff vs game-side shadow        │                      │
 accumulate pending bits ────────┼─ TTripleBuffer ─────►│ for each published bit:
 publish: write CURRENT value    │  latest-wins         │   copy field → UI SHADOW
   of every pending bit          │  + own generation    │   Model.DirtyVariable(name)
 clear bits only on echo         │◄─ applied generation │                    │
                                 │                      │ Context::Update() ─┘
```

### 3.1 The shadow is a real `UScriptStruct` instance — and that is load-bearing

Both shadows (game-side previous-value, UI-side bound) are `Malloc(GetStructureSize())` +
`InitializeStruct`, destroyed with `DestroyStruct`.

**Why this and not a bespoke packed buffer.** The property definition's `DereferencePointer` is
`Property->ContainerPtrToValuePtr<void>(base)`, which uses `FProperty::Offset_Internal` — the offset
in the *real* struct. If the shadow is a real instance, that works directly, and so do
`CopySingleValue` and every property accessor; the adapter stays four classes. If the shadow had its
own packing, every kind would need hand-written marshalling on both sides and bespoke container
types. v1 specified both offsets at once and was therefore incoherent.

**The GC consequence, which is the real reason object properties are excluded.** A `UScriptStruct`
instance owned by the UI thread is **invisible to GC** — nothing calls `AddStructReferencedObjects`
on it. A hard `UObject*` inside it would dangle with no diagnostic. This is independent of, and
harder than, the data-race argument.

### 3.2 The layout — flat, pre-resolved, built once

`FVaCuusModelLayout` is built once per `UScriptStruct`. Each entry: resolved `FProperty*`, the wire
name, the top-level name index to dirty, the field kind, and **the offset of the containing struct**.
Nested structs are **flattened at build time**; there is no per-frame `TFieldIterator`.

The container offset was missing from v1 and is not optional: a flattened nested leaf's
`Offset_Internal` is relative to *its own* owner, so without it the field cannot be addressed from
the model's base at all — the restore-the-bug proof is that `Origin.X` reads the value of an
unrelated sibling. Storing the **container's** offset rather than the value's keeps every read going
through `ContainerPtrToValuePtr`, so no caller does offset arithmetic. This is not the two-offset
incoherence §13.2 records: there is still exactly one offset per value, valid in both the live struct
and the shadow.

Deliberately the shape Unreal's own per-frame differ (`FRepLayout`) uses, and for the same reason:
`FStructProperty::Identical` spins up a fresh `TFieldIterator` per call and `FMapProperty::Identical`
is **O(n²)**. A binder that calls those per frame becomes the frame's dominant cost.

**Wire names are `FField::GetAuthoredName()`**, not `GetName()` — Blueprint properties are mangled
`<Base>_<UniqueId>_<32hexGUID>` and a designer writing `{{Health}}` must not need the GUID.

**Property order is declaration order, stable within a build only** — not across builds or Blueprint
recompiles. Indices live and die with the layout; never persisted.

### 3.3 Name legality is a policy, not an accident — **new in v2**

RmlUi's `LegalVariableName` is a **strict subset** of what UE permits: first character must be a
letter, the rest `[A-Za-z0-9_]`, and a reserved set `{it, it_index, ev, true, false, size, literal}`
checked **case-insensitively**. UE additionally permits `-`, `+`, `<`, `>`, `?` and leading digits in
an `FName`.

So `UPROPERTY() int32 Size;` — about as ordinary as UE gets — **cannot be bound**. `BindVariable`
logs a warning and returns false, the variable is silently absent, and every `{{Size}}` then fails
address resolution with a second warning.

**CORRECTED after implementation** — the rule is positional, and v1 of this section over-refused.
RmlUi applies `LegalVariableName` in exactly two places, `BindVariable` and `BindEventCallback`,
i.e. only to the name a variable is **registered** under. A dotted address is split by `ParseAddress`
with **no name rule at all**, and each non-first segment is resolved by `DataVariable::Child`. The
reserved words are top-level-only too: `it`/`it_index`/`ev` are alias lookups keyed on the *front* of
the address, `literal` tests `address[0]`, and the `true`/`false` check compares the whole dotted
string. So top-level `Size` is unbindable, but **`Panel.Size` binds and works** — holding nested
segments to the full rule would loudly refuse a field that functions.

**Policy: two rules by position.** A top-level name gets the full `LegalVariableName`. A nested
segment need only be non-empty and `[A-Za-z0-9_]` — what the expression lexer accepts after the first
character, since a `-` there would terminate the lexed address and parse as subtraction. A rejected
top-level name logs one `Error` naming the property, the reason and the reserved word when that is
the cause, and is omitted. Renaming silently would put a name in the document that appears nowhere in
the C++, which is worse. An explicit rename attribute is v1.x.

### 3.4 Type coverage — M3a

| Kind | M3a | Note |
|---|---|---|
| numeric, `FBoolProperty` | ✅ | bitfields need care — §5 |
| `FStrProperty`, `FNameProperty` | ✅ | |
| `FTextProperty` | ✅ | shadow the **display string** — §5 |
| `FUtf8StrProperty`, `FAnsiStrProperty` | ✅ | `CastField<FStrProperty>` silently misses these |
| `FEnumProperty`, `FByteProperty`-with-enum | ✅ | see below |
| `FStructProperty` (nested) | ✅ | flattened, depth-limited |
| `FSoftObjectProperty`, `FWeakObjectProperty` | ✅ | projected to a path string at sample time — value types, no GC ownership |
| `FArrayProperty` | **M3b** | §12 |
| `FObjectProperty` (hard) | ❌ | §3.1's GC argument |
| `FMapProperty`, `FSetProperty` | ❌ | no RmlUi map view; `Identical` is O(n²) |
| `FDelegateProperty` | ❌ | events travel the other way |
| **fixed-size C array** (`ArrayDim > 1`) | ❌ | added after implementation: `int32 Fixed[4]` is **one** `FProperty`, so it is not a *kind* and the type table cannot refuse it — binding element 0 and dropping the rest would be a silent partial bind. Refused with a `Warning`; revisit in M3b alongside `FArrayProperty` |

**Enums** are exposed as **one** variable holding the name string, from
`UEnum::GetAuthoredNameStringByValue`. v1 exposed name *and* value, which needs two legal names per
property and collides with §3.3 for no clear gain; the numeric value is available by binding the
underlying integer property if a document needs it.

**Unsupported properties are never silent** — one `Warning` per property per layout build, with the
type name and the reason.

**Exposure** is `(CPF_BlueprintVisible || CPF_Edit) && !CPF_Deprecated && !IsEditorOnlyProperty()`.

`CPF_Edit` is **load-bearing, not a convenience** — corrected after implementation. `FUtf8String` and
`FAnsiString` cannot be Blueprint-visible **at all**: UHT leaves `IsMemberSupportedByBlueprint`
commented out of their property caps. Without `CPF_Edit`, two of M3a's own supported kinds would be
unreachable by any `UPROPERTY` a user could write.

The two exclusions were added during implementation and both prevent a silent wrong result.
**Deprecated**: nothing maintains the value ("read it from an archive, but don't save it"), so
binding one puts a stale number on screen; logged at `Log`, because the outcome is correct rather
than defective. **Editor-only**: the property does not exist in a Game target, so binding it builds a
layout that works in PIE and shows nothing in the packaged build.

### 3.5 The channel — **corrected in v2**

`TTripleBuffer::SwapWriteBuffers` swaps write with **temp**, so the producer's next write buffer is
one that was *published earlier* and never cleared. v1 said "dirty bits OR-accumulate into the
unpublished slot", inherited from the v1 architecture spec. That is wrong, and it regresses values:

> `Health` 100→90 published; not consumed. 90→80 published into the other slot; consumed. Next frame
> the producer gets the **first** slot back — still carrying `bit(Health)` and the value **90**.
> `Health` did not change this frame, so nothing overwrites it. The UI applies 90 over 80 and holds
> it until `Health` next changes. The applier was faithful to the bits; the value was still stale.

**Corrected protocol.** The pending-dirty set lives on the **game thread**, not in the slot. Every
publish writes the **current** value of every pending field into whichever slot it holds. A bit
clears only when the UI thread echoes back an applied generation — the load-serial protocol the
document loader already uses. The channel carries its own generation counter, because
`FVaCuusViewStatus` has none (the existing `Generation` belongs to the interactive snapshot).

### 3.6 Where the apply runs

At the UI thread's existing `// (data snapshots: M3)` marker, after command and input drain and
**before** `Context::Update()` — over **every** host, not only recordable ones. The per-view record
loop is gated on `HasView()`, which requires a non-degenerate view size; every UMG view fails that
until its first Slate tick. Putting the apply there would silently drop updates and then deliver
them in a burst.

---

## 4. The safety property — **three invariants, not one**

The failure mode this milestone is built against: **the shadow holds a value the UI never learns
about.** The result is a stale number on screen forever, with no log line, because the idle gate
correctly withholds a frame that genuinely did not change. It is also a heisenbug — newly added
views are unconditionally dirty, so it **looks correct after every live reload**.

v1 claimed one structural fix. The review showed the class needs three invariants, and that the
shadow has **more than one writer**.

**I1 — the two shadows are identical at bind.** Otherwise frame 1 sets no bit for an unchanged
field and the UI shows a zero-initialised value until that field happens to change. Enforced by a
**forced full-bit first publish**.

**I2 — the channel never carries a bit whose value is older than the bit.** §3.5.

**I3 — the shadow has exactly one writer.** v1 asserted this; it was false. `data-value`,
`data-checked` and any `data-event-click="Health = 50"` assignment reach `VariableDefinition::Set`
and write the shadow directly, with no VaCuus code involved and no game-thread participation — after
which the game-side diff compares the live struct against its *own* shadow, sees no change, and the
two shadows diverge permanently.

**Decision: `Set()` refuses.** It returns `false` and logs once per address. This is clean because
both RmlUi call sites skip their `DirtyVariable` when `Set` returns false. One-way binding is the
contract for M3; writing back to gameplay is M4's surface, where it can be routed to a delegate on
the game thread instead of scribbling on a buffer.

Given all three, the apply loop is still driven by the dirty bits — a field that is not dirty is
never copied, so "written but not dirtied" cannot be expressed *inside the applier*. The other two
doors are now closed too.

**The publish itself is proven.** A dirtied variable forces a publish by two independent legs: the
DOM change regenerates geometry, and `RenderInterface` has **no mutate-in-place operation** for any
resource, so a change necessarily appears as release + fresh compile — both resource-delta arrays
non-empty, failing the idle gate — and independently the content hash moves, because handles are
strictly increasing and never recycled. This is the general mechanism, not a text-specific one, so
it holds for `data-attr`, `data-class`, `data-style` and `data-if` alike.

---

## 5. Per-kind diff rules (correctness, not optimisation)

- **Bitfield bools.** Storage is the containing integer plus a field mask; adjacent bitfields share
  an offset and element size, and `CopyValuesInternal` read-modify-writes. **A `memcmp` of a scratch
  buffer gives false positives.** Compare through `GetPropertyValue_InContainer`.
- **`FText`.** `FTextProperty::Identical` selects an identity comparison when `!GIsEditor` — i.e.
  exactly in the builds that ship. It still returns equal for two empty texts, a shared string-table
  id+key, a shared identity, or two culture-invariant/transient texts (that last falls through to a
  display-string compare). Shadow and compare the **display string**.
- **`FString` / `FName` / `FUtf8Str` / `FAnsiStr`.** Direct comparison; `FName` compares as index.
- **Nested structs.** Flattened at build time; never `FStructProperty::Identical`.
- **Soft/weak object refs.** Project to a path string at sample time and diff the string.
- **Never `ExportTextItem`** — removed from `FProperty` in 5.8 (the name survives as a
  `TStructOpsTypeTraits` customization point) → `ExportTextItem_Direct`. And the near-identically
  named `ExportText_Direct` **silently writes nothing** when the value equals its delta, so a
  `nullptr` delta yields an empty string for every default-valued field.

---

## 6. Threading contract

| Stage | Thread | Why |
|---|---|---|
| Build layout | any | Type descriptors are immutable once linked. **But** `UUserDefinedStruct` / `UBlueprintGeneratedClass` are not native and are collectable — a layout over a Blueprint type holds a strong reference or is rebuilt on recompile. **[unverified]**: recompiling a BP struct under a live layout. Experiment: bind, edit the struct in the editor, recompile, observe. |
| Sample + diff | **game thread only** | Instance data has zero engine synchronisation. Must be driven from `UVaCuusSubsystem::Tick` to land inside the existing `GameTick` perf scope; from an actor tick or a Blueprint node it is outside every scope. |
| Ship | any | Value copy, owns its strings, retains no `UObject*`. |
| Apply + dirty | **UI thread only** | Everything RmlUi. Asserted. |

The definition registry is **process-wide and UI-thread-only** — `VariableDefinition`s carry only
`FProperty*`/`UStruct*` and no per-instance state, so one registry is correct and cheaper than one
per model. Asserted as UI-thread-only state.

**PIE multi-instance is a non-issue, and the reason is worth stating:** there is one `Rml::Context`
per **view**, not per game instance; models live on the context; view ids are process-unique. The
same model name in two views cannot collide. The one process-wide non-atomic global on this path,
`Family<T>::Id()`, is never touched by `BindCustomDataVariable` — which is precisely why this design
is PIE-safe.

---

## 7. Public API

```cpp
UVaCuusView::BindModel(FName ModelName, const UScriptStruct* Type);              // once, before LoadDocument
UVaCuusView::UpdateModel(FName ModelName, const UScriptStruct* Type, const void* Data);
```

**`UpdateModel` takes the type**, because v1's `(FName, const void*)` made "wrong type" and "wrong
size" undiagnosable — both would read arbitrary memory at the layout's offsets, and the first
`FString` field turns that into a crash. The Blueprint CustomThunk gets the type for free; C++ pays
the same. Mismatch → `Error`, no write.

Wrong-call behaviour is specified, not left to chance: null data → `Warning`, return; called before
bind → `Warning`, return; wrong thread → `check(IsInGameThread())`, as every other view mutator
already does; dead view → existing registration gate.

**`MarkModelDirty` is dropped.** `DirtyVariable` accepts top-level names only — a dotted path fails
the same legality rule and is *silently* inserted where it matches nothing, because the guard is a
compiled-out assert. And dirtying without copying makes RmlUi re-read a shadow that still holds the
old value: a footgun whose two failure modes are both silent.

**Ordering requirement: bind the model and every variable before `LoadDocument`.** `data-model` is
resolved once, in `Element::SetParent`; a `data-for` container address is resolved at view
initialisation and the view is discarded if it does not resolve. The failure is an RmlUi log line —
which is invisible (§8) — plus an inert document.

**Model lifetime: created once, never removed.** `RemoveDataModel` is a one-way door and there is
**no unbind API**; re-binding a name warns and keeps the stale pointer. The API therefore offers no
unbind, and the header says why.

**On document reload, do nothing.** The context, model and values all survive; only element-attached
views are rebuilt, and RmlUi re-initialises those itself. VaCuus loads the new document *before*
closing the old one, so the new document attaches while the model is fully live. Tearing the model
down races the load.

---

## 8. Diagnostics, because the library's are gone

**Every RmlUi assert and error log is compiled out in every configuration we build** — `NDEBUG=1`
is keyed on `bUseDebugCRT`, which defaults **false even in Debug**, so `RMLUI_DEBUG` is never
defined; and `RMLUI_LOG_TYPE_ERROR` is the assert macro. The library's entire self-diagnosis is
absent, including for M1/M2 code already shipped. Tracked plugin-wide.

So VaCuus checks its own preconditions and logs through `LogVaCuus`: duplicate model name, duplicate
member, unsupported property kind, illegal wire name (§3.3), layout/type mismatch (§7), and a
refused `Set` (§4).

**`vacuus.DumpModel <view> <model>`** prints the layout, both shadows, the pending and published
dirty sets, and the last applied generation. A milestone whose failure mode is *no output at all*
must ship the tool that shows the pipeline is alive.

---

## 9. Budgets

| | Budget | Note |
|---|---|---|
| Game-thread sample + diff, 64 scalar fields | ≤0.02 ms | inside the existing ≤0.10 ms gate, not additional to it — and that gate is itself met by inference from margin, not complete coverage |
| UI-thread copy + `DirtyVariable` | ≤0.02 ms | **[unverified]**: the "~0.7 µs per variable" in the v1 spec cites nothing and no research document supports it. Experiment: dirty N variables in a real context and measure. |
| **UI-thread re-evaluation caused by dirtying** | ≤0.05 ms | the dominant new cost, and v1 budgeted it nowhere. It runs inside `Context::Update()`, is `O(views under every dirtied name)`, and each expression run constructs a fresh variant stack |
| Idle (no field changed) | **0 published frames** | |

The last row is a correctness gate wearing a performance costume: if merely *having* a model
publishes every frame, the milestone has silently undone M2's central result. It is achievable —
with no dirty variables the view update runs once over an empty vector and exits, nothing writes the
DOM, the hash is unchanged and there is no resource traffic.

`RunFrame()` currently has **no perf scope at all**, so the apply would land where nothing is
measured. Adding those scopes is a **prerequisite task**, not a follow-up.

---

## 10. Testing

- **Per property kind:** change exactly one field, assert exactly its bit sets. Bitfield bools,
  `FText`, `FName`, and the new UTF8/ANSI string kinds each get a case — each has a plausible wrong
  implementation that a naive test passes.
- **The three invariants, each restore-the-bug:** (I1) bind a struct whose defaults differ from
  zero and assert frame 1 shows the defaults; (I2) publish twice without consuming, then consume,
  and assert no field regresses; (I3) drive a `data-event` assignment and assert the shadow is
  unchanged and a warning is logged.
- **Idle:** a bound, unchanging model publishes nothing over N frames.
- **Reload:** a model survives a reload and keeps updating, with no rebind. **Do not reload between
  a write and its assertion** — reload masks a missing dirty flag.
- **End-to-end through a real `Rml::Context`:** `{{Field}}` shows the new value. Six of the seven
  existing idle-gate tests drive the recorder directly and would not catch RmlUi changing behaviour.
- **Illegal names:** a property called `Size` is refused with a diagnostic, not silently absent.
- **Blueprint wire names:** assert `GetAuthoredName() == chop(GetName())` rather than comparing to a
  literal — `UUserDefinedStruct::GetAuthoredNameForField` has two *separate* implementations, editor
  and cooked, which agree for a renamed member but **not** for a never-renamed one (editor yields
  `MemberVar_2`, cooked yields `MemberVar`). An editor automation test only exercises the first.

---

## 11. Risks

| Risk | Mitigation |
|---|---|
| Stale value | §4's three invariants, one restore-the-bug test each |
| A diff rule misses a change | Per-kind tests; the three sharp kinds each get their own |
| BP struct recompiled under a live layout | Strong ref + rebuild. **[unverified]** — experiment in §6 |
| Re-evaluation cost dominates | Budgeted in §9; measured per task |
| Illegal property name | §3.3, with a test |
| RmlUi's silent failures | §8 |

---

## 12. M3b — arrays and `data-for`

Split out because **everything in it depends on a measurement nobody has taken**, while everything in
M3a is settled by source already read.

The seam is real, not administrative. A flat entry keyed on a byte offset cannot address an array
element — its address is `FScriptArrayHelper::GetRawPtr(i)`, valid only for the current `Num()`. And
RmlUi dirties **top-level names only**, so the only granularity that maps onto `DirtyVariable` is
**one bit per top-level array**, which means the apply deep-copies the whole array whenever any
element changes. Whether that is acceptable at 200 rows is a measurement.

M3b carries: the array definition including the `.size` child (handled inline by RmlUi's own array
definition — a hand-written one that forgets it breaks `{{ Killfeed.size }}` with only a warning);
the bit-granularity decision; the shrink cost (RmlUi's own view cleanup is self-flagged quadratic and
shrink is the expensive direction, while growth appends at the tail); a 200-row budget; and the
killfeed end to end.

## 12.1 Also out of scope

JS access to the same models (M4 — same objects, no rework), two-way binding (M4, via delegates
rather than shadow writes — see §4/I3), maps and sets, hard object properties, and a descriptor
asset for per-property control.

---

## 13. What v1 got wrong, and why it matters

Recorded because each was a *plausible* error, and the pattern is worth remembering.

1. **The channel protocol regressed values** (§3.5). v1 inherited "OR dirty bits into the unpublished
   slot" from the v1 architecture spec without checking what `TTripleBuffer` actually recycles. A
   design inherited from an approved document still needs its mechanism read.
2. **Two incompatible shadow layouts** (§3.1). v1 gave each entry a struct offset *and* a shadow
   offset, which are the same number only if the shadow is a real struct instance — which v1 did not
   say. Two half-specified designs read as one complete one.
3. **"The shadow has exactly one writer" was false** (§4/I3). RmlUi's own `data-value` /
   `data-checked` / `data-event` assignment path writes it. A safety argument that names its writers
   must enumerate them from the library's code, not from ours.
4. **Arrays broke the central data structure** (§12) and v1 described them in one line as though they
   fit.
5. **An untyped `UpdateModel`** (§7) made two of the four wrong-call cases undiagnosable.
6. **Name legality was never considered** (§3.3) — and `Size` is both an extremely common UE property
   name and an RmlUi reserved word.

Two things v1 got right and that survived review intact: **finding (b)** as the organising constraint
of the whole milestone, and **§9's idle row** as a correctness gate wearing a performance costume.
