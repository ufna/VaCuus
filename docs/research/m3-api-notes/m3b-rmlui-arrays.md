# TASK C — RmlUi array/`data-for` verification for M3b (vendored copy)

All citations relative to `/w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/` unless prefixed. Every cited line was opened and read.

## 1. What ptr the Array definition's `Size`/`Child` receive, per path

The mechanism is single: `DataVariable::Size()`/`Child()` forward `ptr` **verbatim** to the definition (`Source/Core/DataVariable.cpp:15-23`), and every path below builds its `DataVariable` through the same walk in `DataModel::GetVariable` (`Source/Core/DataModel.cpp:275-302`): root ptr = the bind-time `void*`, then `variable = variable.Child(address[i])` — so each definition receives exactly the ptr the **previous** definition's `Child` placed into the returned `DataVariable`.

- **(a) data-for update:** `DataViewFor::Update` → `model.GetVariable(container_address)` (`Source/Core/DataViewDefault.cpp:498`), then `variable.Size()` (`:503`). No `Type()` check, no intermediate — `Size(ptr)` gets the walked-to ptr.
- **(b) `{{ items.size }}`:** `Size()` is **never called**. The address is `[{items},{"size",index:-1}]` (`ParseAddress`, `Source/Core/DataModel.cpp:9-48`; name-entry ctor sets `index = -1`, `Include/RmlUi/Core/DataTypes.h:37-42`), and the `"size"` entry arrives at the array's **`Child`** via the same walk (`DataModel.cpp:285-290`), reached from `Instruction::Variable` → `GetValue` → `GetVariableInto` (`Source/Core/DataExpression.cpp:989-996`, `:1168-1183`; `DataModel.cpp:316-323`).
- **(c) static `{{ items[3].field }}`:** resolved at parse time to one fixed address (`DataExpression.cpp:681-693`: `VariableExpression` builds `"items[3].field"`, rewinds, `parser.Variable(full_address)`; address stored by `VariableGetSet`, `:255-266`). At run, same walk: array `Child({index:3})`, then the **element** definition's `Child({field})`.
- **(d) dynamic `{{ items[i] }}`:** compiled to string concatenation + `Instruction::DynamicVariable` (`DataExpression.cpp:617-636`, `:696-699`); at run the concatenated string is re-parsed by `DataExpressionInterface::ParseAddress` → `DataModel::ResolveAddress` → the **same** `GetVariable` walk (`DataExpression.cpp:980-987`, `:1161-1167`).

**Confirmed: on every path the array definition receives the value ptr its parent's `Child` handed over.** For M3b concretely: `FVaCuusPropertyDefinition` derives `BasePointerDefinition` (`/w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuus/Private/VaCuusDataVariable.h:140-150`), and `BasePointerDefinition::Size`/`Child` call `DereferencePointer(ptr)` (= `ContainerPtrToValuePtr`) **before** forwarding (`Source/Core/DataVariable.cpp:152-164`) — so `FVaCuusArrayDefinition::Size/Child` always receive **the address of the TArray** (or the bind-time ptr directly if the array is bound at top level via `BindCustomDataVariable`, since `DataModel::BindVariable` stores the `DataVariable` as-is, `DataModel.cpp:132`).

## 2. Every caller of `VariableDefinition::Size()` in `Source/Core/`

Exhaustive (grep over `Source/Core/` + data headers, each hit opened):

1. `DataVariable::Size()` — forwarder (`Source/Core/DataVariable.cpp:15-18`). Its **only compiled caller** is `DataViewFor::Update` (`Source/Core/DataViewDefault.cpp:503`). The Debugger also calls it (`Source/Debugger/ElementDataModels.cpp:42-43`) but is **not compiled** — zero Debugger relay files exist in `Source/VaCuusRml/Private/Gen/` (verified by listing).
2. `BasePointerDefinition::Size()` — forwards to `underlying_definition->Size(DereferencePointer(ptr))` (`Source/Core/DataVariable.cpp:152-156`).
3. The base fallback `VariableDefinition::Size` — logs `LT_WARNING "Tried to get the size from a non-array data type."` and returns 0 (`Source/Core/DataVariable.cpp:40-44`).

**Is `Size()` called on a variable whose `Type() != Array`?** Yes, trivially reachable: `DataViewFor::Update` calls `variable.Size()` with **no `Type()` check whatsoever** (`DataViewDefault.cpp:498-503`) — `data-for` over a struct/scalar hits the base warning and iterates 0 rows.

**Is `DataVariableType::Array` load-bearing anywhere?** In the compiled plugin, **no runtime behavior keys on `Type() == Array`**. All `Type()` consumers: `DataVariable::Type()` itself has no compiled caller (`Source/Core/DataVariable.cpp:25-28`); `BasePointerDefinition`'s ctor copies the underlying's type (`:134-136` — this is what makes a property-wrapped array *report* Array); `MemberScalarGetSetFuncDefinition` ctor copies it (`Include/RmlUi/Core/DataVariable.h:235`); `DataStructHandle.h:169` checks `!= Scalar` (template registration path, unused by M3); the Debugger switches on it (`Source/Debugger/ElementDataModels.cpp:26,40,58` — not compiled). Set it correctly anyway; it costs nothing and the debugger relays may be added later.

## 3. `DataAddressEntry` shapes an Array `Child` must handle

`DataAddressEntry` ctors are either-or: name ctor → `index = -1`; index ctor → empty name (`Include/RmlUi/Core/DataTypes.h:37-42`). `ParseAddress` can produce only `{index >= 0}` or `{non-empty name, index == -1}` — empty names and `index < 0` are rejected at parse (`Source/Core/DataModel.cpp:19-24`, `:34-36` — `FromString` yielding `< 0` fails the whole address). What the built-in `ArrayDefinition::Child` (`Include/RmlUi/Core/DataVariable.h:143-163`) does:

| Entry | Behavior | Diagnostic |
|---|---|---|
| named `"size"` (`index == -1`) | falls into the out-of-bounds branch, matches `name == "size"` → `MakeLiteralIntVariable(container_size)` (`:148-152`) | none |
| `0 <= index < Num` | `begin()` + `std::advance`, returns `DataVariable(underlying_definition, &(*it))` (`:158-162`) | none |
| `index >= Num` | empty `DataVariable` (`:155`) | `LT_WARNING "Data array index out of bounds."` (`:154`) |
| negative index | same OOB branch (`:149`) — but **unreachable from RML**: `ParseAddress` rejects it (`DataModel.cpp:35-36`), a runtime-negative dynamic index like `items[-1]` fails address parsing and the interpreter errors with "Variable address not found." (`DataExpression.cpp:984-985`); `data-for` aliases are built only with the loop index `i >= 0` (`DataViewDefault.cpp:516`) | interpreter `LT_WARNING` + program dump, not the array's |
| named non-`"size"` | OOB branch, name mismatch → empty `DataVariable` with the **misleading** "index out of bounds" warning (`:149-155`) | that warning |

[inference] For `FVaCuusArrayDefinition`: implement the same contract — `"size"` → literal int; in-bounds index → element; everything else → empty `DataVariable`; but log a named-child miss as its own message rather than reusing "out of bounds". Downstream, an empty `DataVariable` makes `GetVariable` return empty and `GetVariableInto` log "Could not get value from data variable '%s'." (`DataModel.cpp:316-323`).

## 4. Shrink path: N → K (R = N−K rows removed)

Per removed row `i` in `[K, N)` inside the single `DataViewFor::Update` call (`Source/Core/DataViewDefault.cpp:531-536`): `model.EraseAliases(elements[i])` (map erase, `DataModel.cpp:205-208`), then `RemoveChild(elements[i]).reset()` — destruction is immediate.

`RemoveChild` → `detached_child->SetParent(nullptr)` (`Source/Core/Element.cpp:1492`) → `SetDataModel(nullptr)` (`:2196-2199`) → `data_model->OnElementRemove(this)` (`:2157`) **and recursion into every child** (`:2164-2165`). **So yes — it recurses: every element in the row's subtree fires `OnElementRemove`, which sweeps that element's views too**, not just the row root's. `DataModel::OnElementRemove` (`Source/Core/DataModel.cpp:365-371`) does: `EraseAliases` + `views->OnElementRemove` + `controllers->OnElementRemove` + `attached_elements.erase`. `DataViews::OnElementRemove` is a **full linear scan of all V views** per element (`Source/Core/DataView.cpp:47-60`), moving matches to `views_to_remove`; the controller side is a cheap map erase (`Source/Core/DataController.cpp:36-39`).

Then, at the end of the current `DataViews::Update` iteration, each view in `views_to_remove` triggers a **full pass over `name_view_map`** (M entries) (`Source/Core/DataView.cpp:118-132` — self-flagged `// @performance: Horrible...` at `:117`).

**Complexity of shrinking R rows** (E elements/row, W views/row, V total views, M total map entries): **O(R·E·V)** for the element sweeps + **O(R·W·M)** for map cleanup. Since V and M are themselves proportional to total rows×W, clearing a whole N-row table is **quadratic in N**. Mid-pass safety: removed views stay alive in `views_to_remove` until `:131` and are skipped via `IsValid()` (`:112`, `DataView.cpp:22-25`).

## 5. Grow path: appending R rows

- **All R rows are created in ONE `DataViewFor::Update` call** — the loop runs `i` to `Math::Max(size, num_elements)` and the creation branch handles every missing index (`Source/Core/DataViewDefault.cpp:507-530`): `InstanceElement` → aliases → `InsertBefore` → `SetInnerRML` per row (`:511-527`). Note aliases are inserted **before** `InsertBefore` (`:520-523`), so the row's views parse-resolve `it` correctly.
- New views only **queue**: `DataModel::AddView` → `DataViews::Add` → `views_to_add` (`DataModel.cpp:107-110`, `DataView.cpp:42-45`). The outer loop's next iteration flushes the **entire** `views_to_add` vector and updates every flushed view once, unconditionally (`DataView.cpp:70-88`).
- **Iteration count is independent of R**: appending 200 flat rows costs 2 iterations of the cap-10 loop (iteration i: data-for runs; iteration i+1: all 200 rows' views flushed + run; loop exits when `views_to_add` is empty and the dirty count is stable, `:70`). **200 rows cannot exhaust the cap by count.**
- **Recursion is by nesting depth, not row count**: a `data-for` inside each row is itself a queued view; when it first updates (iteration i+1) it creates *its* rows, whose views run at i+2 — so depth D consumes ~D+1 iterations. Exhausting the cap needs ~9-10 nested `data-for` levels (or a chain of views that keep dirtying variables). If exhausted, nothing is lost: `views_to_add` is persistent member state (`Source/Core/DataView.h:93`) and the next frame's `DataModel::Update` (`Context.cpp:195-197`) flushes it — the tail lands one frame late.

## 6. `{{ it }}` and `{{ it_index }}` for scalar arrays

- `{{ it }}`: `'['`-less, `'('`-less name → `parser.Variable("it")` (`DataExpression.cpp:702-703`) → `ParseAddress` → `ResolveAddress("it", element)`: not in `variables` (reserved, unbindable — `DataModel.cpp:53`, `:70-71`), so the ancestor alias walk replaces entry 0 with the alias's full address (`DataModel.cpp:238-261`) = `container_address + [i]` (`DataViewDefault.cpp:513-516`, `:520`). `GetVariable` then walks `items → Child({index:i})` → the array hands back `(underlying_definition, &element_i)` — **an expression consisting of just `it` resolves to the element scalar and renders** (via `Get` → `Variant` → `DataViewText::Update`, `DataViewDefault.cpp:341-379`).
- `{{ it_index }}`: alias address is `{{"literal"},{"int"},{i}}` (`DataViewDefault.cpp:518`, `:521`); `GetVariable`'s literal special case returns `MakeLiteralIntVariable(address[2].index)` (`DataModel.cpp:295-299`; `MakeLiteralIntVariable` encodes the int in the ptr, `DataVariable.cpp:57-72`). **Renders the fixed creation index.**
- **Never renumbered**: the only `InsertAlias` calls on rows are at creation (`DataViewDefault.cpp:520-521`); the update loop only appends at the tail (new index = current `i`, `:509-516`) and truncates the tail (`:531-540`); `EraseAliases` on removal only deletes. The one other alias-writer in Core, `CopyAliases`, is called solely from `WidgetDropDown` (`Source/Core/Elements/WidgetDropDown.cpp:101`). So a front-removal in your data shifts values under fixed row indices; rows keep their original `i` forever.

## 7. `"size"` collisions

- `items[0].size`: `ParseAddress` splits on `'.'` then extracts brackets — `"items[0]"` → name entry `items` + **index entry** `0`, `".size"` → a **separate named entry** (`Source/Core/DataModel.cpp:22-41`). The walk (`DataModel.cpp:285-290`) consumes `{0}` at the **array's** `Child` and hands `{"size"}` to the **element-struct's** `Child`. **An element field wire-named `size` is therefore reachable** — the array special case never sees it. `StructDefinition::Child` has no `"size"` special case (`Source/Core/DataVariable.cpp:76-95`) and member names are never legality-checked (`AddMember`, `:106-112`; `LegalVariableName` guards only top-level binds, `DataModel.cpp:119`, `:160`).
- Conversely, **an array-level named child that is not `"size"` is unreachable** — built-in behavior is warn + empty (§3).
- `Panel.size` (top-level struct): the expression parser accepts it as one name (`.` is a valid variable character, `DataExpression.cpp:315-331`), `ParseAddress` yields `[{Panel},{size}]`, and the struct's `Child` receives `"size"` like any member — **unaffected by the array special case**, which lives only in `ArrayDefinition::Child` (`Include/RmlUi/Core/DataVariable.h:151-152`). Only the *top-level bind name* `size` itself is forbidden (`DataModel.cpp:53`, `:70-71`).

## 8. Row updates after `DirtyVariable(root)`

- Row views' expressions are alias-resolved **at parse time** (`DataViewCommon::Initialize` → `Parse` → `VariableGetSet` → `ParseAddress` → `ResolveAddress`, `DataViewDefault.cpp:25-36`, `DataExpression.cpp:255-266`), so `GetVariableNameList` reduces every row expression to `address[0].name` = the container root (`DataExpression.cpp:1145-1155`), and `name_view_map` is keyed on that root (`DataView.cpp:82-83`). `DirtyVariable(root)` → `equal_range` collects **every view in every row** (`DataView.cpp:90-95`) → each re-evaluates.
- DOM writes are compare-before-write in every default view: attribute `:79`, attr-if `:102`, checked `:141`, style `:168`, class `:191`, rml `:212`, if `:235`, visible `:260`, text `:354` (all `Source/Core/DataViewDefault.cpp`).
- The cost lands **inside `Context::Update`**: `for (auto& data_model : data_models) data_model.second->Update(true);` (`Source/Core/Context.cpp:195-197`) → `DataModel::Update` → `views->Update` (`Source/Core/DataModel.cpp:373-381`). Nothing evaluates eagerly at `DirtyVariable` time (`DataModel.cpp:325-331` just inserts into a set).

## 9. `data-for` element attributes

Two cooperating mechanisms, both verified:

1. **Copy to rows**: all attributes except exactly `"data-for"` and `"rmlui-inner-rml"` are copied into each generated row element (`Source/Core/DataViewDefault.cpp:485-491`, the two skips at `:488`), passed to `Factory::InstanceElement` (`:511`). When the row attaches (`InsertBefore` → `SetParent` → `SetDataModel` → `ApplyDataViewsControllers`, `Element.cpp:1404`, `:2206`, `:2162`), its copied `data-if`/`data-class`/etc. instantiate as normal per-row views.
2. **Structural cancellation on the template**: `ApplyDataViewsControllers` clears every other pending view/controller when it meets a structural view and initializes **only** the `data-for` (`Source/Core/ElementUtilities.cpp:439-448`, comment at `:441-444`). So `data-if` on the template element creates **no view on the template itself** — which also means nothing can un-hide the template's `display: none` set at `DataViewDefault.cpp:474`. Per-row `data-if`/`data-class` work exactly as intended.

## 10. Deltas from `rmlui-data-binding.md` §5

Re-opened `Source/Core/DataViewDefault.cpp:429-572` and `Include/RmlUi/Core/DataVariable.h:133-167` in full. **No substantive difference found.** All §5 cites still match: `variable.Size()` at `:503`; `GetVariableNameList` at `:545-549`; siblings-insert at `:523`; `display:none` at `:474`; attribute copy at `:486-491`; iterator defaults at `:462-466`; `Release` `delete this` at `:551-554`; `result` never assigned (`:502`, `:542`); `ArrayDefinition` at `DataVariable.h:133-167` with the `"size"` case at `:151-152` and the OOB warning at `:154`. Two trivia only: (1) the doc's snippet range "DataVariable.h:147-155" starts one line early — the code sits at `:148-155` (`:147` is blank); (2) the doc does not mention the structural-view cancellation (`ElementUtilities.cpp:439-448`, §9 above) — additive, not a contradiction. Nothing in this task's questions required an experiment; all were settled from source.

---

**Correction (2026-07-31, spec review round 1):** §6's exhaustiveness sentence — "the one other
alias-writer in Core is `CopyAliases`" — is incomplete. `DataViewAlias::Initialize` also calls
`InsertAlias` (`Source/Core/DataViewDefault.cpp:593`). It writes an alias on its own `data-alias`
element, never on `data-for` rows, so the frozen-row-identity conclusion stands unchanged; only the
enumeration was short by one.
