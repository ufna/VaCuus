# M6 sweep — five P2/P3 bugs: state, fix shapes, protocol

All paths relative to `/w/Unreal/VcHost/Plugins/VaCuus/` unless rooted. Engine = `/w/Unreal/UnrealEngine/`. Every cited line was opened this session; `[inference]` marks the reasoned-not-read.

---

## 1. akj.13 — CreateView returns a valid-looking handle when UI-thread Initialize() failed · P2

**Premise still true post-M5: YES, unchanged.**

- `CreateView` refuses only three things — null host (`Source/VaCuus/Private/VaCuusSubsystem.cpp:150-154`), no UI thread (`:159-163`), stopping thread (`:165-171`). Past those it enqueues `AddView` (`:178`) and unconditionally constructs + registers the handle (`:180-182`), returning it at `:187`.
- On the UI thread, a failed `Host->Initialize()` logs one Error and returns (`Source/VaCuus/Private/VaCuusUIThread.cpp:1410-1416`). `Command.Status` — the one channel back to the game thread — is never stamped.
- `InitializeView` set `bRegistered = true` (`Source/VaCuus/Private/VaCuusView.cpp:28`); `IsViewValid()` is `GetUIThread() != nullptr` (`:262-265`), which returns non-null while `bRegistered && Subsystem` (`:142-149`). Nothing ever flips it. **True forever.**

**What a caller sees today:** `IsViewValid()==true` permanently. `LoadDocument` stamps `LoadRequestSerial` and enqueues (`VaCuusView.cpp:164-166`); the UI thread drops the command for the unknown view at **Verbose** (`VaCuusUIThread.cpp:1310`). `LoadCompletedSerial` never advances, so `IsLoadPending()` (`VaCuusView.cpp:631-634`) is stuck true forever and `PollStatus` early-outs at `:706-710` — `OnLoadCompleted` (`:730`) never fires. Input is likewise dropped at Verbose (`VaCuusUIThread.cpp:1376`). One UI-thread Error line (`:1414`) is the only diagnostic in the whole process.

**Fix shape** (the bead's own suggestion holds up against the code): `FVaCuusViewStatus` (`Source/VaCuus/Public/VaCuusViewStatus.h:56-157`) has load serials, frame counters, snapshot — no boot field. Add `std::atomic<uint8> BootState {Pending, Booted, Failed}`; store `Failed` (release) in the `AddView` failure branch (`VaCuusUIThread.cpp:1414`), `Booted` after `Hosts.Add` (`:1423`). In `UVaCuusView::PollStatus` (`VaCuusView.cpp:680`), on first observing `Failed`: one game-thread Error, `Invalidate()` (`:36-53`, already flips `bRegistered` and detaches IME/router), and broadcast `OnLoadCompleted(this, false)` so load-waiters unblock. Test: a stub host whose `Initialize` returns false (pattern exists — `FStubHost`, `Source/VaCuus/Private/Tests/VaCuusReloadTest.cpp:32`), pump one drain + one poll, assert the handle went invalid; restore-the-bug by removing the stamp.

**Effort:** ~half day incl. test. **Verdict: SWEEP** — a reference-HUD buyer (M6's audience) hitting a boot failure currently gets an eternally "valid" dead view.

---

## 2. akj.16 — BP struct recompile dangles every FProperty* in a live layout · P2

**Premise still true: YES — the refusal exists nowhere; only the fact-recording test exists.**

- `Source/VaCuusEditor/Private/Tests/VaCuusModelRecompileTest.cpp:63-163` asserts exactly the two facts (type object survives `:126-128`, every resolved `FProperty*` dangles `:136-146`) and deliberately does not refuse anything (`:58-61`). Grep for `FStructEditorManager`/`INotifyOnStructChanged` in plugin source: zero hits.
- M3b did double the surface as the bead's note says: an array-of-struct field pins a second layout via `FVaCuusModelArrayDesc::ElementLayout` (`Source/VaCuus/Public/VaCuusModelLayout.h:163-165`), and the UI-side definitions pin types too (`Source/VaCuus/Private/VaCuusDataVariable.h:351`).

**What breaks today:** editor-only heap corruption exactly as the bead records — `FVaCuusModelShadow` owns a real struct instance destroyed via `DestroyStruct` (`Source/VaCuus/Private/VaCuusModelShadow.h:11-12, :124-127`); after a recompile the new `DestructorLink` walks the old buffer.

**The hook, found and read:**
- Listener interface: `FStructureEditorUtils::INotifyOnStructChanged` = `FStructEditorManager::ListenerType` (`Engine/Source/Editor/UnrealEd/Public/Kismet2/StructureEditorUtils.h:28-42`); virtuals `PreChange`/`PostChange` (`.../Kismet2/ListenerManager.h:11-12`); registration is automatic in the listener's ctor/dtor (`ListenerManager.h:25-32`).
- **Timing is the load-bearing fact:** `BroadcastPreChange` fires at `Engine/Source/Editor/KismetCompiler/Private/UserDefinedStructureCompilerUtils.cpp:599`, *before* the compile at `:622` (which reaches `CleanAndSanitizeStruct` at `:512`); `BroadcastPostChange` at `:661`. So during `PreChange` the OLD property chain is still alive — the one moment a synchronous game-side teardown is safe.

**Minimal refusal design:**
1. `VaCuusEditor`: `FVaCuusStructRecompileGuard : INotifyOnStructChanged` (UnrealEd lives only there — runtime module cannot see it). `PreChange(Struct)` forwards to a new runtime-module static (e.g. on `UVaCuusSubsystem`, next to the existing statics pattern `Source/VaCuus/Public/VaCuusSubsystem.h:109`).
2. Runtime: walk live bound models; match if the model's root struct **or any `ElementLayout`'s struct** is the changing type. For each match: log **Error** naming model + struct; set a dead flag that makes `Sample`/`PublishPending` no-ops (`Source/VaCuus/Private/VaCuusBoundModel.h:73` — both are game-thread entry points); destroy the *game-side* shadow synchronously (safe at PreChange time, old properties alive).
3. UI side cannot be reached synchronously (the compile won't wait for a drain): enqueue a model-drop whose shadow teardown uses an `Abandon()` on `FVaCuusModelShadow` — free the buffer *without* `DestroyStruct`. A bounded, editor-only, once-per-incident leak beats a wrong-offset free. RmlUi's raw `void*` into the UI shadow (`VaCuusModelShadow.h:107-111`) is why the drop must remove the whole data model, not rebind.
4. Test: extend the recompile test — bind a model, fire `RenameVariable`, assert one Error + refused `Sample`, never a crash.

**Effort:** 1–1.5 days incl. test. **Verdict: SWEEP** — designers editing a BP struct during PIE is a normal Fab-customer action; today it is silent heap corruption.

---

## 3. akj.22 — rmlui_dynamic_cast across .so · P2

**Premise: TRUE mechanism, but the three suspect sites provably WORK today — by a load-order accident, now fully characterized.** This is the sweep's biggest finding.

**Complete audit** (grep, all modules): live non-test sites are exactly `Source/VaCuus/Private/VaCuusTextInput.cpp:238, :243, :334`; test sites `Source/VaCuus/Private/Tests/VaCuusInputRoutingTest.cpp:197`, `Source/VaCuusRender/Private/Tests/VaCuusTextEntryTest.cpp:174`, `Source/VaCuusRender/Private/Tests/VaCuusSlateRoutingTest.cpp:173`. VaCuusJs has none (comments only — the M4 fixes hold). Zero plugin-side classes use `RMLUI_RTTI_Define*`; 24 vendored classes do.

**Mechanism, verified end-to-end and settled by experiment:**
- Identity = address of a function-local static in an inline member (`Source/ThirdParty/RmlUi/Include/RmlUi/Core/Traits.h:67-91`); the cast compares it through virtual `IsClass` (`:93-105`). Modular flags: `-fvisibility-ms-compat` (`LinuxToolChain.cs:439`), `-fvisibility-inlines-hidden` (`:450`).
- The statics land in each `.so`'s **dynamic symbol table as WEAK DEFAULT** (readelf on `Binaries/Linux/`: VaCuus.so syms 786/1081/1147; also present in VaCuusRml.so, VaCuusRender.so) with `R_X86_64_GLOB_DAT` relocations — so binding is decided by the **dynamic linker's scope**, and UE dlopens every module **RTLD_LOCAL** (`Engine/.../Unix/UnixPlatformProcess.cpp:109`, with UE modules deliberately kept local `:131-141`).
- **Experiment (gotprobe3, scratchpad, run against the real binaries):** a module dlopen'd as its own root binds the identity to **its own copy**; modules pulled in as **DT_NEEDED of one dlopen root all bind to the root's copy**. Ids unify inside one dlopen closure and diverge across separate dlopens.
- **Why today's editor works:** `VaCuus.uplugin` gives VaCuusRender `PostConfigInit` — it loads *first*, and its DT_NEEDED closure includes VaCuusRml **and** VaCuus (readelf -d). So {Render, VaCuus, Rml} unify on Render's copies; the Default-phase dlopens of the already-loaded libraries change nothing. Proof at the behavior level: today's run (`/w/Unreal/VcHost/Saved/Logs/VcHost.log`, 2026.08.01-18.01) shows `Routing`, `SlateRouting`, `TextEntry` all `Success`, and TextEntry's `FieldValue=="Va"` assertion (`VaCuusTextEntryTest.cpp:480`) is reachable only through a successful cross-module cast (`:174`).
- **Why the two M4 incidents were real:** VaCuusJs is dlopen'd separately at Default phase → its own copies → every id it compared against the Render-rooted closure differed. Both confirmed failures involved VaCuusJs; the fix that worked (class + instancer into VaCuusRml, `Source/VaCuusRml/Public/VaCuusScriptDocument.h:14-38`) is sound precisely because construction and comparison then share one module's GOT.

**What breaks when a cast dies (silent, both):** `:238/:243` failing → `GetSelectionCharacterRange` returns false (`:247-250`) → caret parked at end-of-text (`:359-364`) — IME selection silently lost. `:334` failing → `FillTextFieldState` returns false (`:335-340`) → "no field" published → IME never activates for a focused field, indistinguishable from the legitimate author-custom-element case the fallback exists for (`:337-339`).

**Fix shapes, judged:**
- **Recommended (per-site):** three exported non-inline helpers in VaCuusRml (`VACUUSRML_API Rml::ElementFormControl* …Cast(Rml::Element&)`, plus the two selection getters), same pattern that fixed M4. Both sides of the id compare then resolve through VaCuusRml's relocations — sound under *every* load order, ~40 lines, no vendored diff. Convert the 3 live sites + 3 test sites.
- **Global fix judged against the probe:** "export the statics with default visibility" does **not** work — they already *are* WEAK DEFAULT and still diverge under RTLD_LOCAL scoping. A true single home means rewriting the Traits.h macros to out-of-line definitions inside VaCuusRml — a diff across 24 vendored classes with permanent upstream-merge burden. `linux_global_symbols` (`UnixPlatformProcess.cpp:114-127`) would unify everything but exports vendored Rml symbols RTLD_GLOBAL — [inference] a collision hazard if any other Fab plugin in the buyer's project vendors RmlUi. Reject both.

**Effort:** ~half day incl. a canary test (assert the helper resolves a real `<input>` non-null — fails loudly if the mechanism regresses). **Verdict: SWEEP** — "works by loading phase" is exactly the kind of accident a licensee's project rearranges; the current safety is one `.uplugin` edit from silently dying, and monolithic games mask it completely.

---

## 4. akj.6.17 — uncooked standalone missing global shaders · P2

**Premise still true post-M5: YES — reproduced this session.** `./Binaries/Linux/VcHost VcHost.uproject -RenderOffscreen -unattended -nullrhi` → **exit 1 within seconds, no project log written**, stdout showing only the UnrealTraceServer fork (scratchpad `standalone.out`) — the failure is even quieter than the bead recorded. The cooked path is fine: the M5 staged run's log (`~/.config/Epic/VcHost/Saved/Logs/VcHost.log`, 17:59, `LogPakFile` teardown, clean exit) confirms M5's cooked gate covers the shader library.

**Fix shape:** this is stock UE behavior — a non-editor target has no compiled global shader library and cannot build one from uncooked data; [inference] `-odsc` is a cook-server workflow, not a fix for "run the standalone binary on raw content". The supported matrix is: **uncooked → `UnrealEditor -game` (what every recipe in CLAUDE.md already uses); standalone binary → cooked/staged only.**

**Effort:** one paragraph in the M6 gotchas doc (a contracted deliverable) + close. **Verdict: SWEEP as wontfix-documented.**

---

## 5. akj.6.9 — itlib flat_map::at() UB in Shipping · P3

**Premise: the hazard is real but the path is dead code.**

- The UB exists as described: `ITLIB_FLAT_MAP_NO_THROW` → `assert(false)` (`Source/ThirdParty/RmlUi/Include/RmlUi/Core/Containers/itlib/flat_map.hpp:99-104`), and `at()` returns `i->second` on a miss once asserts compile out (`:335-355`). The define is public (`Source/VaCuusRml/VaCuusRml.Build.cs:26`); `SmallUnorderedMap`/`SmallOrderedMap` are `itlib::flat_map` (`Include/RmlUi/Config/Config.h:85-87`).
- **Audit result: zero `.at(` call sites in the entire vendored RmlUi tree and zero in all five plugin modules** (greps over `Source/ThirdParty/RmlUi` and `Source/VaCuus*`). Only `flat_map.hpp` even contains the throw macro; `flat_set.hpp` has none.

**Fix shape:** close as **verified-unreachable**, with one line in the vendor-update protocol: re-run `grep -rn '\.at(' Source/ThirdParty/RmlUi` after any RmlUi bump. **Effort:** minutes. **Verdict: SWEEP (close now).**

---

## P3 quick verdicts

| Bead | State (verified) | Verdict |
|---|---|---|
| **akj.11** RmlUi asserts compiled out | Premise already corrected in the bead; the surviving in-code comments state the *correct* version (`Source/VaCuus/Private/VaCuusSystemInterface.cpp:79-87`). Remaining acceptance: (a) `RMLUI_DEBUG` decision — **not recorded**, no `.cs` defines it (grep); (b) the assert-only-API call-site enumeration — not done. | **SWEEP the decision** (recommend: keep it off in all shipped configs, note the local-debug recipe — 15 min), fold (b) into the M6 gotchas audit pass; then close. |
| **akj.12** RunFrame perf scopes | **DONE.** `RunFrame` (`VaCuusUIThread.cpp:1046`) carries `VACUUS_PERF_SCOPE` for DrainCommands `:1062`, DrainInput `:1066`, DataApply `:1076`, JsPump `:1096`, JsGC `:1133`; stats declared (`VaCuusStats.h:51-53`, `VaCuusStats.cpp:10-12`) and the PerfLog prints every phase (`VaCuusStats.cpp:31, :40-42`). | **CLOSE.** |
| **akj.17** data-style units defeat idle gate | Still true: compare at `Source/ThirdParty/RmlUi/Source/Core/DataViewDefault.cpp:168`, `SetProperty` `:170`; `Property::Get<T>` returns the unit-less variant (`Include/RmlUi/Core/Property.h:41-45`); vendored file untouched since the vendor commit (`git log`: eff3176 only). | **SWEEP the doc half** — it is a named row for the M6 perf guide ("a unit-bearing data-style binding never goes idle"). Normalization/upstream: **DEFER**. |
| **akj.6.15** probe-host dedup | Grown from 5 to **8 identical `FProbeHost` copies + 12 specialized variants = 20 test hosts** across four modules (grep list). Half-day+ refactor, test-breakage risk, zero product value. | **DEFER.** |
| **akj.6.16** capture-release dedup | **Effectively done, differently:** logic single-homed as `SVaCuusWidget::ReleaseOwnPointerCapture` (`SVaCuusWidget.h:129`, `.cpp:158`), called from both teardown owners (`VaCuusUMGWidget.cpp:149`, `VaCuusRender.cpp:385`), per-user rationale documented in-code citing this very bead (`VaCuusUMGWidget.cpp:141-146`). | **CLOSE as done** (residual DetachView-vs-caller placement is taste, and the code argues its side). |
| **akj.6.18** vacuus.ReloadUI editor-only | Still true: command registered only in `Source/VaCuusEditor/Private/VaCuusLiveReload.cpp:526-530`, but the dispatch is already a runtime static (`VaCuusSubsystem.h:109`) and the editor body just forwards (`VaCuusLiveReload.cpp:499`). Fix = move the `FAutoConsoleCommand` into the runtime module (pattern in place: `GDumpModelCommand`, `VaCuusSubsystem.cpp:435`); move, don't copy — same-name double registration. | **SWEEP** (~30 min; manual reload in `-game`/packaged is product-facing and touches the same VFS surface as the M6 bundle work). |

## Sweep order recommendation

1. **akj.22** (half day) — smallest fix neutralizing the largest latent risk; the load-order accident is now documented evidence, don't let it rot.
2. **akj.13** (half day) — API honesty for the reference HUD.
3. **akj.6.9 + akj.6.17 + akj.6.16 + akj.12 closes** (~1 hour total, two of them doc-only).
4. **akj.16** (1–1.5 days) — the only multi-day item; editor-only but corruption-class.
5. **akj.6.18 + akj.11 decision + akj.17 doc row** — fold into the M6 docs/bundle passes where they naturally land.
6. **Defer:** akj.6.15; akj.17 normalization/upstream.

Experiment artifacts: `/tmp/claude-1000/-w-Unreal-VaCuus/68bf753f-68ae-4444-9939-886e328b9fd3/scratchpad/gotprobe3.c` (RTLD_LOCAL identity-binding probe, both load orders) and `standalone.out` (akj.6.17 repro).
