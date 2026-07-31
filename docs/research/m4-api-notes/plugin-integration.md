# TASK C — M4 (VaCuusJs) integration surface map

Code root: `/w/Unreal/VcHost/Plugins/VaCuus/Source/` (branch `m4-js`, HEAD `f0eb3a6` = master post-M3b). All paths below are relative to that root unless absolute. Every cited line was opened and read.

---

## 1. UI thread frame loop, Init/Exit, shutdown sequence

### 1.1 RunFrame phase order (`VaCuus/Private/VaCuusUIThread.cpp:847-906`)

`FVaCuusUIThread::RunFrame()` asserts `check(IsInUIThread())` (`VaCuusUIThread.cpp:851`) and then runs four phases:

| Phase | Lines | Perf scope |
|---|---|---|
| DrainCommands | `VaCuusUIThread.cpp:862-865` | `VACUUS_PERF_SCOPE(DrainCommands)` at :863 |
| DrainInput | `VaCuusUIThread.cpp:866-869` | :867 |
| DataApply (`ApplyModelUpdates()`) | `VaCuusUIThread.cpp:870-879` | :877 |
| Per-view record loop | `VaCuusUIThread.cpp:899-905` | none at loop level, deliberately (:857-861: a wrapping scope "would double-count" Update/Record) |

The scopes are placed at the call site, not inside the functions, "so that the three phases of a UI frame sit next to each other and can be read as a decomposition of it" (`VaCuusUIThread.cpp:857-861`).

**Marker comments, confirmed.** The M3a marker is at `VaCuusUIThread.cpp:871`: `// (data snapshots: M3a)`, and its placement argument (:873-877) is exactly the shape the JS pump reuses: "After both drains, so a model bound or a view loaded by this frame's commands is applied into this frame; and BEFORE Context::Update(), so the re-evaluation a dirtied variable causes is paid inside Update". `VaCuusStats.h:39-42` records that the `DataApply` stat was declared *before* the phase existed, anchored at "RunFrame()'s `(data snapshots: M3)` marker" — that declare-scope-first, land-phase-later playbook is the precedent for the JS pump scope. There is no "Pump JS" marker in code today; the only M4 anticipations in `VaCuusUIThread.cpp` are the stack-size comment (:42-50) and nothing else (grep for QuickJS/M4 across the plugin: `VaCuusUIThread.cpp:44-45`, `VaCuusInputMap.cpp:125`, `VaCuusDataVariable.h:442`, `SVaCuusWidget.cpp:950`, `VaCuusModelLayout.cpp:500`).

**Where "Pump JS" goes.** `Context::Update()` is *not* a RunFrame phase — it runs per view inside `FVaCuusRmlDocumentHost::RecordAndPublishFrame` under the `Update` scope (`VaCuusRender/Private/VaCuusRmlDocumentHost.cpp:397-400`), followed by the snapshot publish (:423) and the `Record` scope wrapping `Context::Render()` + `EndFrameAndPublish` (:429-434). So the spec's step 3 ("Pump JS ... before `Context::Update()`") maps to a new scoped phase in RunFrame between the DataApply block (ends `VaCuusUIThread.cpp:879`) and the record loop (starts :899). One warning from the code: the record loop is gated on `HasView()`, which is a *recordability* test (non-degenerate size), not liveness — the long comment at `VaCuusUIThread.cpp:885-897` explains why DataApply is deliberately *outside* that gate (UMG views fail `HasView()` until their first Slate tick, `VaCuusUMGWidget.cpp:69-76` creates views at `FIntPoint::ZeroValue`). Timers/microtasks for a sizeless-but-alive view have the same shape as data applies: pumping inside the gated loop would stall JS on views awaiting first layout. The pump therefore belongs at thread level (or at least ungated), like `ApplyModelUpdates()` (`VaCuusUIThread.cpp:908-927`, gated on nothing).

**Controlled GC point.** Candidates in RunFrame order: (a) after the record loop, end of RunFrame (`VaCuusUIThread.cpp:905-906`) — post-publish, pause invisible to this frame's output; (b) between DataApply/JS pump and the record loop. Either way `FrameCount` increments only after RunFrame returns (`VaCuusUIThread.cpp:774-775` in `Run()`, :422-423 in `RunFrameInline()`), so the GC pause is inside the frame the counter names, and "counts against the UI budget" needs a new `EScope` + cycle stat (see §9). Named experiment: **GC-point-cost** — add the pause scope at both candidate points behind a cvar, run the M3 demo + `vacuus.M1HUD.PerfLog 1`, compare p99 of `Update`/`Record` windows to see whether pre-record GC shifts layout-cache pressure.

### 1.2 Init (`VaCuusUIThread.cpp:701-760`)

Order is load-bearing: RmlUi claim + boot first (`Engine.IsClaimableOnThisThread() && Engine.Initialize()`, :733), thread ids published only after success (:754-755), `bInitSucceeded` last (:758). The comment at :715-732 documents why publishing the id before the claim was a real bug. A JS runtime that boots in Init must go *after* :733 (RmlUi up) and its failure path must leave nothing behind (:739-742: "Exit() will NOT run when Init() fails"). Note `FVaCuusEngine::Initialize` also installs the system/file interfaces and loads the default font (`VaCuus/Private/VaCuusEngine.cpp:88-148`).

**Stack size is an M4 constraint already written down:** the UI thread runs on a chosen 512 KB stack — `GVaCuusUIThreadStackSize = 512 * 1024` (`VaCuusUIThread.cpp:50`), with the comment (:42-49) saying explicitly "QuickJS lands on this thread in M4, so the platform default is not obviously enough". quickjs-ng's interpreter recurses on the native stack; M4 must either raise this constant or set the JS stack limit well under it. Named experiment: **stack-headroom** — with QuickJS integrated, run a deliberately deep JS recursion + deep RmlUi tree and confirm the JS engine's stack-limit error fires before the thread guard page does.

### 1.3 Exit and the shutdown sequence

Three cooperating pieces:

**(a) The in-band Shutdown command** (`VaCuusUIThread.cpp:944-966`, drained in DrainCommands): closes every document via `Pair.Value->CloseDocument()` (:950-953), sets `bStopRequested` (:954), drops anything queued behind it (:958), publishes `bShutdownDrained` last (:965). This is the path `FVaCuusModule::StopUIThread()` takes via `RequestGracefulShutdown` (`VaCuusUIThread.cpp:426-470`; bounded wait, 0.1 s constant at `VaCuus/Private/VaCuus.cpp:24`, fallback warning `VaCuus.cpp:126-131`), after which `UIThread.Reset()` (destructor → `delete Thread` = stop+join, `VaCuusUIThread.cpp:291-320`) runs `Exit()` on the worker.

**(b) Exit()** (`VaCuusUIThread.cpp:781-845`), numbered in the source:
- Step 0: drain-and-discard leftover commands (:796-801).
- Step 1: every host's `Shutdown()` (:805-808). For the RmlUi host this is `Document->Close()` **and** `Rml::RemoveContext` in one call (`VaCuusRmlDocumentHost.cpp:86-111`), plus the render-side release (:113-124).
- Step 2: `Engine.Shutdown()` → `Rml::Shutdown()` (:816-819; `VaCuusEngine.cpp:211-228`).
- Step 3: `Hosts.Empty(); RetiredHosts.Empty(); Models.Empty()` (:824-827).
- Step 4: `FVaCuusDefinitionRegistry::ReleaseAll()` (:838-841) — pinned-UScriptStruct cache dropped here and only here (`VaCuus/Private/VaCuusDataVariable.h:386-403`), because static destruction would touch a dead UObject system (:829-837).
- Finally the thread-id retraction (:843-844).

**(c) Where JS teardown slots.** The spec order (close documents while JS alive → destroy JS runtime → Rml contexts down) does not fall out of Exit as written, because step 1 fuses document-close and context-removal per host. Two paths to reconcile:
- **Graceful path:** documents are already closed by DrainCommands (:950-953) while the loop is still running — JS is alive there for unload events. JS runtime destruction then goes at the top of Exit, before the host loop (before :805).
- **Hard-stop path** (`Stop()` without the in-band command, `VaCuusUIThread.cpp:477-487`): DrainCommands' close never ran, so documents die inside step 1's fused `Shutdown()`. To honor the ordering on this path, step 1 must be split: loop `CloseDocument()` over hosts (the interface already has it, `VaCuus/Public/VaCuusDocumentHost.h:87`) → destroy JS runtime → loop `Shutdown()`. The existing precedent for exactly this kind of ordering split is `RemoveView` (`VaCuusUIThread.cpp:1144-1180`): models are dropped only *after* `Host->Shutdown()` because "that context's data models still hold raw void*s ... Dropping the references first could destroy a shadow the context is about to read" (:1163-1171). JS-held `Rml::ObserverPtr` handles are dead-safe by construction (spec §7), so the constraint is softer, but the unload-event ordering is not.

Also relevant: `ShutdownModule` runs `StopUIThread()` before the engine dies and *before* `VaCuusRender::ShutdownModule` (reverse load order — VaCuusRender is PostConfigInit; `VaCuus.cpp:47-66`, `VaCuus.uplugin` Modules block), and `UVaCuusSubsystem::Deinitialize` only removes its own views, leaving the thread running across PIE sessions (`VaCuus/Private/VaCuusSubsystem.cpp:34-55`). A per-subsystem JS runtime must therefore die on `RemoveView`-like commands or subsystem teardown, **not** only in Exit — see the keying tension in §2.

Per-view removal detail M4 inherits: `CloseDocument` only *queues* the unload; the elements are freed by the next `Context::Update()` (`VaCuusRmlDocumentHost.cpp:238-247`), and the closed view owes one clearing frame (`:250-283`, `VaCuusDocumentHost.h:76-87`).

---

## 2. Module boundary — registration today, IVaCuusScriptHost

**`IVaCuusScriptHost` does not exist.** Grep over the whole `Source/` tree finds the name only in the spec (`/w/Unreal/VaCuus/docs/superpowers/specs/2026-07-29-vacuus-architecture-design.md:78`). M4 creates the seam.

**There is no startup-registration factory of any kind today.** The spec's "register implementations of core-defined interfaces at module startup (`IVaCuusScriptHost`, RenderInterface factory, input sink)" (spec:77-79) describes a future state. What exists:

- `FVaCuusModule` owns exactly two things: the engine and the UI thread (`VaCuus/Public/VaCuus.h:75-88`); `StartupModule` only builds the engine and primes the content roots (`VaCuus.cpp:29-39`).
- The document host is **instance-passed, not registered**: callers in VaCuusRender construct `FVaCuusRmlDocumentHost` directly and hand it through `UVaCuusSubsystem::CreateView(TUniquePtr<IVaCuusDocumentHost>, …)` — demo toggle `VaCuusRender/Private/VaCuusRender.cpp:647`, UMG widget `VaCuusRender/Private/VaCuusUMGWidget.cpp:75-76`, tests `VaCuusModelViewTest.cpp:119`, `VaCuusCloseDocumentTest.cpp:132`. `CreateView`'s comment states the reason: "The host comes from the caller because building one needs the render-side pieces ... which live in VaCuusRender -- a module that depends on this one" (`VaCuus/Public/VaCuusSubsystem.h:70-79`).
- `FVaCuusRenderModule::StartupModule` registers nothing with core — it maps the shader directory and caches the ImageWrapper module (`VaCuusRender.cpp:1666-1679`); `FVaCuusEditorModule::StartupModule` builds the file watcher (`VaCuusEditor/Private/VaCuusEditor.cpp:15-23`).

**Closest existing precedents for "optional module plugs into the core frame loop":**
1. **`IVaCuusDocumentHost`** — interface declared in core, implemented in the dependent module, motivation spelled out: "VaCuus declares the contract and VaCuusRender implements it ... The seam is deliberate" (`VaCuus/Public/VaCuusDocumentHost.h:22-27`). This is the cleanest template for `IVaCuusScriptHost`: core declares it, `VaCuusJs` implements, and the frame loop skips the step when nothing is present (matching spec:79-80 "frame-loop steps with no registered implementation are skipped").
2. **`FVaCuusEngine::SetRenderInterface`** — the one true "register an implementation with core before boot" API (`VaCuus/Public/VaCuusEngine.h:81-87`, enforcement `VaCuusEngine.cpp:230-242`: refused after `Initialize()`). Its only caller today is a test (`VaCuusRecorderTest.cpp:752`). A `FVaCuusModule::SetScriptHostFactory(...)`-style hook called from `FVaCuusJsModule::StartupModule` would follow this shape — with the same "before the thread boots / before the runtime exists" rule.
3. **`FVaCuusUIThread::Models`** — UI-thread state *keyed beside* the hosts rather than pushed into the host interface: "KEYED ON THE VIEW rather than held by the host ... a BindModel(...) method on the host interface would push all of it into whichever module implements the host" (`VaCuus/Public/VaCuusUIThread.h:326-341`). The JS runtime + per-view/per-document JS state can live in `FVaCuusUIThread` the same way, reached through `IVaCuusScriptHost` calls, with `IVaCuusDocumentHost::GetContext()` (`VaCuusDocumentHost.h:110-123`) as the bridge to the DOM.

**Keying tension to resolve (spec vs code):** the spec says "one `JSRuntime`+`JSContext` **per subsystem** on the UI thread" (spec:240-241), but the UI thread has no per-subsystem structure at all — `Hosts` and `Models` are keyed by process-unique `ViewId` (`VaCuusUIThread.h:315, 341`; ids allocated at `VaCuusUIThread.cpp:497-502`), and `FVaCuusUICommand` carries only `ViewId` (`VaCuus/Private/VaCuusUIQueues.h:116`). Multi-PIE is "N subsystems, 1 UI thread, N views" (`VaCuusUIThread.h:26-33`). Making the runtime per-subsystem requires a subsystem/instance id in the command payload (a new field beside `ViewId`) plus a subsystem-scoped map on the UI thread, and a teardown command when a subsystem deinitializes (today `Deinitialize` sends only per-view `EnqueueRemoveView`, `VaCuusSubsystem.cpp:38-49`). This is a design decision M4 must make explicit — no code answers it today.

---

## 3. VFS — what a JS module loader gets

- **The file interface:** `FVaCuusFileInterface` (`VaCuus/Private/VaCuusFileInterface.h:1-31`, `.cpp:59-205`) implements `Rml::FileInterface` over `IPlatformFile::OpenRead` — meaning documents in a **pak** open transparently (`VaCuus/VaCuus.Build.cs:55-60`: "goes through ... the pak layer"). `Open()` resolves relative paths through the ordered roots via `ResolveExistingDocument` (`VaCuusFileInterface.cpp:64-79`), rejects directories (:85-89), and the wrapper `FOpenFile` fixes the Unix seek-to-EOF off-by-one (:14-51). A JS loader can either go through `Rml::GetFileInterface()` or (simpler) call `VaCuusContentPaths::ResolveExistingDocument` + `IPlatformFile` directly; either is UI-thread-legal.
- **Path normalization / roots:** `VaCuusContentPaths::GetDocumentRoots()` — plugin `Content/DevUI` first, project second, absolute + normalised (`VaCuus/Private/VaCuusContentPaths.cpp:18-43`); function-local static primed from the game thread at module startup so the first UI-thread call cannot race plugin mounting (`VaCuusContentPaths.cpp:46-52`; `VaCuus.cpp:33-36`; threading note `VaCuus/Public/VaCuusContentPaths.h:57-61`). `ResolveExistingDocument` handles empty, absolute-passthrough and root-probing (`VaCuusContentPaths.cpp:54-87`).
- **The `vfs://` scheme does not exist in code.** The only pseudo-URLs are memory-source *names*: `"vacuus://memory.rml"` (`VaCuusRmlDocumentHost.cpp:23`), `"vacuus://model_test.rml"` (`VaCuusModelTestHost.h:102`), `"vacuus://data_for.rml"` (`VaCuusDataForTest.cpp:136`). And the scheme will not pass through resolution untouched: `FPaths::IsRelative` treats any string not starting with `/`, `\\` or `<drive>:` as relative (engine `Runtime/Core/Private/Misc/Paths.cpp:1265-1280`), so `ResolveExistingDocument("vfs://app.js")` would probe `<Root>/vfs://app.js` and miss. The M4 module loader must strip/translate the scheme before resolving.
- **Hot reload does not touch VFS state — there is none to touch.** The file interface caches nothing across `Open()` calls; the only caches live reload clears are RmlUi's process-global stylesheet/template caches (`ClearAssetCaches`, `VaCuusUIThread.cpp:1252-1277`; textures deliberately not released, :1272 and `VaCuusEditor/Private/VaCuusLiveReload.h:37-43`). The watcher registers on exactly `GetDocumentRoots()` (existence-checked, `VaCuusLiveReload.cpp:205-247`), the flush **discards the changed-file set** and reloads every view with a file document (`VaCuusLiveReload.cpp` flush: "this set does NOT drive which views reload"; granularity rule `VaCuusLiveReload.h:159-167`). Consequence for M4: a JS module cache inside the runtime is a *new* cache the reload path must learn to drop, and the extension whitelist is **rml/rcss only** (`VaCuusLiveReload.cpp:20-26` `IsWatchedExtension`) — `.js`/`.mjs` must be added or edits to scripts produce no reload at all.

---

## 4. Document lifecycle — where per-document JS hooks

- **Load:** both load kinds funnel into `AdoptDocument` (`VaCuusRmlDocumentHost.cpp:179-217`): new document loaded *first*, old one closed *second* (:193-194 — the ordering that makes "do nothing to the model on reload" safe, mirrored by the probes: `VaCuusModelTestHost.h:109-114`, `VaCuusDataForTest.cpp:143-148`), then `Show(ModalFlag::None, FocusFlag::Document)` (:212), then `ReportLoadResult` (:216). A document-ready JS callback / `<script>` execution belongs in this window — after the document exists and is shown (:212) and before/alongside the load-result report (:216) — plumbed through the script-host seam so the RmlUi host stays JS-free.
- **Live reload (M2) and JS:** a reload is just a fresh `LoadDocumentFile` command re-issued by `UVaCuusView::ReloadDocument` (`VaCuus/Public/VaCuusView.h:154-173`), after one process-wide `ClearAssetCaches` (`VaCuusSubsystem.cpp:195-268`). The data-binding rule on reload is "ON DOCUMENT RELOAD, DO NOTHING — the context, the model ... all survive a load" (`VaCuusView.h:231-239`). For JS: old-document element handles die when `AdoptDocument` closes the old document (ObserverPtr makes them safely dead), and per-document script must **re-run** on the new document via the same ready hook; runtime-level state (timers, module cache — modulo §3's cache-drop question) survives like models do.
- **Close ordering that already exists vs JS teardown:** documents are closed today at — `CloseDocument` command (`VaCuusUIThread.cpp:1046-1047` → `VaCuusRmlDocumentHost.cpp:234-284`), `AdoptDocument`'s replace (:193), `RemoveView` → host `Shutdown` (`VaCuusUIThread.cpp:1160`; `VaCuusRmlDocumentHost.cpp:86-101`), in-band Shutdown drain (`VaCuusUIThread.cpp:950-953`), and Exit step 1 (:805-808). In every one of these the JS runtime is naturally still alive (it dies only in Exit / on its owner's removal), **except** the fused close+RemoveContext inside host `Shutdown()` on the hard-stop path — the split described in §1.3(c) is what makes "close documents while JS alive, then destroy JS runtime, then Rml contexts" true on all paths.
- The unload is deferred: `Close()` queues; the next `Update()` frees (`VaCuusRmlDocumentHost.cpp:238-247`), or `RemoveContext`'s destructor does (:88-91). JS unload-event dispatch must happen at `Close()` time, not at free time.

---

## 5. Command queue — ExecuteScript does not exist yet

`EVaCuusCommandKind` (`VaCuus/Private/VaCuusUIQueues.h:18-90`) contains: `None, AddView, RemoveView, ClearAssetCaches, LoadDocumentFile, LoadDocumentMemory, CloseDocument, Resize, BindModel, DumpModel, SetVisible, Shutdown`. **No ExecuteScript**, though spec §4 lists it in the command-queue vocabulary (spec:105). To add it:

- **Definition:** new enum entry in `VaCuusUIQueues.h`; script source/path rides in `FVaCuusUICommand::Payload`, "the command's general-purpose string" (`VaCuusUIQueues.h:118-119`) — precedent: RML source in Payload for `LoadDocumentMemory` (:50-51) and model name in Payload for `DumpModel` (`VaCuusUIThread.cpp:586-592`).
- **Producer:** an `EnqueueExecuteScript(ViewId, …)` next to the others (`VaCuusUIThread.cpp:524-608`), all funneling through `Enqueue()` with its stop-gate (:627-641). Producers are game-thread-only — the queue is SPSC (`VaCuusUIQueues.h:150-157`).
- **Dispatch:** the routed switch in `DrainCommands` (`VaCuusUIThread.cpp:1036-1065`; unknown-view drop at :998-1026, `checkNoEntry()` default :1062-1064). Note the per-kind ViewSize pre-apply (:1031-1034) is harmless for a script command.
- **Ordering guarantee for free:** FIFO from a single producer means "ExecuteScript enqueued after LoadDocument runs against the loaded document" — the same argument BindModel-before-load rests on (`VaCuusUIQueues.h:63-69`).

Losing a script command silently would repeat the BindModel lesson: the drain logs unknown-view drops at Verbose but hoists BindModel to Error because its loss is invisible downstream (`VaCuusUIThread.cpp:999-1020`) — ExecuteScript has the same failure shape and deserves the same treatment.

---

## 6. Public API shape and the JS enable/disable flag

- **`UVaCuusView`** is the facade: every mutator is a non-blocking enqueue stamped with the view id, gated on `bRegistered` (`VaCuus/Public/VaCuusView.h:88-100, 616-617`). `ExecuteScript(const FString&)` sits naturally beside `LoadDocumentFromMemory` (`VaCuusView.h:176-177`) as a `UFUNCTION(BlueprintCallable, Category="VaCuus")`, with the same "dead view drops it" semantics as `SendInput` (:360-367).
- **`UVaCuusSubsystem`** owns views and pulses the thread (`VaCuus/Public/VaCuusSubsystem.h:25-44`; the pulse `VaCuusSubsystem.cpp:57-108`). If the runtime is per-subsystem (spec), subsystem `Initialize`/`Deinitialize` (`VaCuusSubsystem.cpp:26-55`) are where the create/destroy commands originate.
- **Where config flags live today: nowhere but cvars.** The plugin has no `UDeveloperSettings`, no `GConfig` reads, no `Config=` UCLASS (grep over `Source/` — zero hits). The complete flag inventory is three cvars: `vacuus.M1HUD.PerfLog` (`VaCuus/Private/VaCuusStats.cpp:24-27`), `vacuus.IdleGate` (`VaCuusRender/Private/VaCuusRecordingRenderInterface.cpp:47-52`, read with `GetValueOnAnyThread()` on the UI thread per :42-46), `vacuus.M1HUD.AutoShot` (`SVaCuusWidget.cpp:25`). So the spec's "JS can be disabled at runtime — QuickJS never initialized via config flag" (spec:81-82) has no existing config surface to join; the in-tree precedent is a cvar checked at the moment the runtime would first be created (cheap, any-thread, the `IdleGate` pattern), and an ini-backed `UDeveloperSettings` would be a new mechanism M4 introduces. The build-level "skip when module absent" comes free from the script-host seam (§2): no registered host, no pump.

---

## 7. Test infrastructure reusable for JS tests

- **Probe hosts.** `VaCuusModelTest::FProbeHost` (`VaCuus/Private/Tests/VaCuusModelTestHost.h:48-223`) is a headless `IVaCuusDocumentHost` with a real `Rml::Context` on the real UI thread: `RecordAndPublishFrame` runs `Context::Update()` then captures DOM values into plain members (:149-164, capture via `GetInnerRML`/`GetAttribute` :194-215). Why a probe and not the production host: module direction — VaCuus-private types are unreachable from VaCuusRender (:21-27). `FDataForProbeHost` (`VaCuusDataForTest.cpp:71-...`) adds the per-frame `FrameLog` with `Reserve`-once (:86-95) and the **settled-count clamp** — `SettledFrames()` reads `Status.FramesRecorded` with acquire because `WaitForFrameCount` alone admits one straggler frame (`VaCuusDataForTest.cpp:301-320`). A JS-executing test host is `FProbeHost` plus "evaluate this source at frame N, capture the DOM/console output into members".
- **Pumping pattern:** `FVaCuusModule::Get().GetOrStartUIThread()` with `ON_SCOPE_EXIT { Module.StopUIThread(); }` (`VaCuusDataForTest.cpp:386-396`), precondition `FVaCuusEngine::Get().IsInitialized()` false (:381), skip under no-multithreading (:375-379); then `EnqueueAddView` / commands / `RunFrames` — one frame per Trigger+`WaitForFrameCount(Before+1)` because the wake event coalesces (`VaCuusModelTestHost.h:239-253`, duplicated `VaCuusDataForTest.cpp:285-299`).
- **Log category precedent:** exactly one category exists — `LogVaCuus`, declared `VACUUS_API DECLARE_LOG_CATEGORY_EXTERN(LogVaCuus, Log, All)` (`VaCuus/Public/VaCuusDefines.h:7`), defined `DEFINE_LOG_CATEGORY(LogVaCuus)` (`VaCuus/Private/VaCuus.cpp:14`). `LogVaCuusJS` (spec:253 names `UE_LOG(LogVaCuusJS)`) follows the same pair in VaCuusJs's public defines header + module cpp, exported with the module's API macro so tests in other modules can match on it.

---

## 8. Build system — vendoring pattern, and C sources under UBT

**How VaCuusRml vendors RmlUi** (`VaCuusRml/VaCuusRml.Build.cs`):
- `PCHUsage = NoPCHs`, `bUseUnity = false` (:10-11); warnings relaxed for third-party code: `UndefinedIdentifierWarningLevel = Off` (:12), `ShadowVariableWarningLevel = Off` (:13), `bWarningsAsErrors = false` (:14); `bEnableExceptions = false` (:15) with the itlib no-throw define (:26); `CppStandard = Cpp20` (:16).
- Include path to the vendored tree at `Source/ThirdParty/RmlUi/Include` (:18-19); `RMLUI_CUSTOM_RTTI` (:20); `RMLUI_STATIC_LIB` for monolithic vs `RMLUI_CORE_EXPORTS` for modular (:28-39); FreeType via `AddEngineThirdPartyPrivateStaticDependencies` (:46); private include dirs into the vendored `Source/*` (:48-52).
- **Relay files:** 190 one-line cpps in `VaCuusRml/Private/Gen/`, each `#include "../../../ThirdParty/RmlUi/Source/Core/X.cpp"` (`Gen/relay_Core_Element.cpp:1-2`), generated by `VaCuusRml/gen_relays.sh` — this is how UBT compiles sources living outside the module directory.

**C sources: UBT compiles `.c` natively.** There is no `.c` anywhere in the plugin today, but the toolchain support is first-class:
- `.c` is a known input extension and lands in `InputFiles.CFiles` (engine `UnrealBuildTool/Configuration/UEBuildModuleCPP.cs:3072, 3123-3127`).
- `ModuleRules` has `CStandardVersion? CStandard` (`UnrealBuildTool/Configuration/Rules/ModuleRules.cs:1506`), flowing into the compile environment (`UEBuildModuleCPP.cs:2742`), and clang compiles C files with `-x c` + the chosen `-std=c99/c11/c17/c2x` (`UnrealBuildTool/Platform/Clang/ClangToolChain.cs:594-599, 546-569`).
- **Engine precedent for a vendored C amalgamation inside a normal Runtime module: SQLiteCore.** `Private/SQLiteEmbedded.c` is a relay `.c` that `#include`s the vendored `sqlite/sqlite3.inl` wrapped in `UE_COMPILER_THIRD_PARTY_INCLUDES_START/END` (engine `Plugins/Runtime/Database/SQLiteCore/Source/SQLiteCore/Private/SQLiteEmbedded.c:17-24`), with the API exported across the module boundary by a definitions-level macro `SQLITE_API=SQLITECORE_API` (`SQLiteCore.Build.cs:23`) and behavior configured entirely through `PrivateDefinitions` (:26-43). (`SymsLib` also ships `.c` files but is `ModuleType.External` linking prebuilt libs — `SymsLib.Build.cs:10` — so SQLiteCore is the compile-it-yourself precedent.)
- **VaCuusJs.Build.cs shape, therefore:** clone the VaCuusRml preamble (NoPCHs, no unity, warnings off, no exceptions — quickjs-ng is C, PCHs don't apply to C anyway), vendor quickjs-ng under `Source/ThirdParty/quickjs-ng/`, set `CStandard = CStandardVersion.C11` (or C17), and either list relay `.c` files in `Private/Gen` (the VaCuusRml pattern, via a `gen_relays.sh` sibling) or place an SQLiteCore-style single relay `.c` including the sources. Export macros for any C symbols crossing module boundaries follow the `SQLITE_API=SQLITECORE_API` trick; the module boundary itself follows VaCuusRml's `RMLUI_STATIC_LIB`/exports split (:28-39).

---

## 9. Stats — where JS heap and GC-pause counters join

Two parallel systems, both in core so every module can sample (`VaCuus/Public/VaCuusStats.h:10-25` explains why they are `VACUUS_API` in VaCuus):

1. **`stat vacuus` cycle stats:** `DECLARE_STATS_GROUP(TEXT("VaCuus"), STATGROUP_VaCuus, ...)` (`VaCuusStats.h:8`); UI/RT/GT cycle stats (:43-59); per-frame DWORD counters `Draw Calls`/`Commands` (:62-63); all `DEFINE_STAT`'d in `VaCuusStats.cpp:10-22`. A GC-pause cycle stat (`STAT_VaCuusJsGC (UI)`) and a JS-pump stat join the `(UI)` block. Heap bytes have **no in-plugin precedent** — the existing counters are per-frame-reset DWORDs (:61-63); heap size wants a set-each-frame stat, which is new ground here.
2. **`FVaCuusPerfLog` (headless soaks):** `EScope` is a **positional** enum in RunFrame order (`VaCuusStats.h:84-129`) indexed into `GScopeNames` (`VaCuusStats.cpp:35-47`), guarded by `static_assert(UE_ARRAY_COUNT(GScopeNames) == FVaCuusPerfLog::Num, ...)` (:48-49) — and the header warns that only reading both lists together catches a mis-*ordered* insertion (`VaCuusStats.h:79-83`). Adding `JsPump` (between `DataApply` and `Update`) and `JsGC` means: enum entry + name at the same position + `DECLARE/DEFINE_CYCLE_STAT` + the padded name string. The `VACUUS_PERF_SCOPE` macro emits both systems in one line (`VaCuusStats.h:180-182`).
3. **Non-scope counters precedent:** `AddUIFrame(bPublished)` — a dedicated counter with its own window/total accounting and its own printed line (`VaCuusStats.h:140-149`; `VaCuusStats.cpp:160-181`, window print :247-269) — is the template for "GC runs this window / bytes reclaimed", printed beside the idle line.
4. **The playbook for adding a scope before its phase exists** is written down for DataApply: declare the stat first so the cost never folds into a neighbor (`VaCuusStats.h:39-45`). Do the same for `JsPump`/`JsGC` before the pump lands.

---

## Open questions → named experiments

1. **pump-granularity** — thread-level pump (one `JSContext` pumped once per RunFrame) vs per-view rAF dispatch inside the record loop: measure with the PerfLog whether rAF callbacks mutating view B's DOM after view A's `Update()` in the same frame produce one-frame-late publishes for A; decides whether rAF dispatch must be ordered before the whole record loop (current mapping, §1.1) or interleaved per view.
2. **stack-headroom** — §1.2: deep JS recursion + deep document on the 512 KB stack (`VaCuusUIThread.cpp:50`); pass = QuickJS's own stack-limit error, fail = guard-page crash → raise the constant or set `JS_SetMaxStackSize` accordingly.
3. **runtime-keying** — §2: per-subsystem (spec) vs per-process runtime on the one thread; prototype the subsystem-id-in-command plumbing and check whether anything in Tier 1 actually needs isolation between PIE clients beyond what per-view document scoping already gives.
4. **GC-point-cost** — §1.1: GC before vs after the record loop, compared via PerfLog p99 on the M3 demo.
