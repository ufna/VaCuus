# VaCuus as it exists today — the publish path a data channel must plug into

Research note for **M3 (data binding)**, written 2026-07-30 against the tree at
`/w/Unreal/VcHost/Plugins/VaCuus/Source` (byte-identical to `/w/Unreal/VaCuus/Source`; `diff -rq`
is clean, so every `file:line` below resolves in either copy).

**Status of the claims.** Every line cited here was opened and read. Where I am reasoning rather
than quoting, the paragraph says **INFERENCE**. Where the existing source says something I believe
is wrong or drifted, it is in §9.

**Vendored RmlUi** is at `Source/ThirdParty/RmlUi`; citations of the form
`Source/Core/Context.cpp:181` are relative to that root. The RmlUi *data-model API* itself is the
subject of a sibling note — this one covers only as much of it as is needed to answer §4 and §6.

---

## 0. Orientation: what exists, and where

Four modules (`VaCuus.uplugin`; `VaCuus.Build.cs:11-33` for the runtime core's dependencies):

| Module | Type | What it owns |
|---|---|---|
| `VaCuusRml` | Runtime | Vendored RmlUi 6.x compiled into UE via one relay TU per vendored `.cpp` (`VaCuusRml/gen_relays.sh`, `VaCuusRml/Private/Gen/relay_*.cpp`) |
| `VaCuus` | Runtime | The UI thread, the queues, the view/subsystem handles, input translation, the snapshot builder, the file/system interfaces, stats |
| `VaCuusRender` | Runtime | The recording render interface, the command buffer, the RHI replayer, the Slate custom element, `SVaCuusWidget`, `UVaCuusWidget`, and the concrete `FVaCuusRmlDocumentHost` |
| `VaCuusEditor` | Editor | Directory watcher + live-reload debounce, `vacuus.ReloadUI`, proof harnesses |

`VaCuus` takes `VaCuusRml` as a **private** dependency (`VaCuus.Build.cs:27-33`), so `VaCuus`'s
*.cpp files may include RmlUi headers (`VaCuusUIThread.cpp:16-27` does) but its **public headers may
not name Rml types**. That constraint is the reason `IVaCuusDocumentHost::GetContext()` exists and
is documented as a seam (`VaCuusDocumentHost.h:109-123`) — and it decides where a model builder can
live (§3.4).

**There is no data-binding code in the tree at all today.** A grep for `DataModel|DirtyVariable|
data-model` across the four modules returns three hits, all of them comments:
`VaCuusSubsystem.h:100`, `VaCuusUIThread.cpp:809`, `Tests/VaCuusReloadTest.cpp:135`.
`VaCuusUIThread.cpp:809` is the reserved slot:

```cpp
	DrainCommands();
	DrainInput();
	// (data snapshots: M3)
```
— `VaCuusUIThread.cpp:807-809`

---

## 1. Every cross-thread channel that exists today

Three threads participate: **GT** (game), **UI** (the one process-wide `FVaCuusUIThread`), **RT**
(render). Plus task-graph workers for image decode.

### 1.1 Summary table

| # | Channel | Type | Writer | Reader | Coalescing | Drained at |
|---|---|---|---|---|---|---|
| 1 | Command queue | `TSpscQueue<FVaCuusUICommand>` | GT | UI | none in the queue; per-view *effects* coalesce | `FVaCuusUIThread::DrainCommands()`, `VaCuusUIThread.cpp:823-925` |
| 2 | Input queue | `TSpscQueue<FVaCuusInputEvent>` | GT | UI | none — order is the contract | `FVaCuusUIThread::DrainInput()`, `VaCuusUIThread.cpp:927-963` |
| 3 | Wake latch | `FEventRef{EEventMode::AutoReset}` | any | UI | **binary latch** — N triggers → 1 frame | `Run()` waits, `VaCuusUIThread.cpp:735` |
| 4 | Interactive-region snapshot | `TTripleBuffer<FVaCuusInteractiveSnapshot>` | UI | GT | **latest-wins**, intermediates dropped | `UVaCuusView::RefreshSnapshot()`, `VaCuusView.cpp:305-321` |
| 5 | View status: load result | `std::atomic<uint64> LoadCompletedSerial` + `std::atomic<uint8> LoadResult` | UI | GT | **one slot**, latest completion wins | `UVaCuusView::PollStatus()`, `VaCuusView.cpp:375-399` |
| 6 | View status: frame counters | `std::atomic<uint64> FramesRecorded` / `FramesPublished` | UI | GT | monotonic counters | polled ad hoc (`VaCuusView.cpp:280-288`) |
| 7 | Command buffer | `TUniquePtr<FVaCuusCommandBuffer>` via `ENQUEUE_RENDER_COMMAND` | UI | RT | **gated** by the idle short-circuit (§4); RT queues up to 4 | `FVaCuusSlateElement::Draw_RenderThread` |
| 8 | Widget dest rect | `ENQUEUE_RENDER_COMMAND(VaCuusSetDestRect)` | GT | RT | last write wins | `SVaCuusWidget::OnPaint`, `SVaCuusWidget.cpp:271-287` |
| 9 | Texture decode results | `TMpscQueue<FVaCuusTextureDecode>` + `std::atomic<bool> bAbandoned` | worker tasks | UI | none | `DrainCompletedDecodes()`, `VaCuusRecordingRenderInterface.cpp:768-863` |
| 10 | UI-thread identity | file-scope `std::atomic<uint32> GVaCuusUIThreadId` | UI (in `Init`/`Exit`) | any | n/a | `FVaCuusUIThread::IsInUIThread()`, `VaCuusUIThread.cpp:664-668` |
| 11 | Thread lifecycle flags | `std::atomic<bool>` ×4, `std::atomic<uint32/uint64/int32>` ×5 | mixed | any | n/a | `VaCuusUIThread.h:280-314` |
| 12 | PerfLog accumulators | `FCriticalSection` + arrays | GT, UI, RT | GT | per-5s window | `FVaCuusPerfLog::TickLog()`, `VaCuusStats.cpp:173-280` |

Two things that **look** cross-thread and are not: the mouse-cursor latch and the caret latch in
`VaCuusSystemInterface.cpp:28-46` are plain non-atomic statics, explicitly because they are
"written and read only from the one thread allowed to call into RmlUi at all"
(`VaCuusSystemInterface.cpp:44-46`), and `GetVaCuusLatchedMouseCursor` asserts it
(`VaCuusSystemInterface.cpp:49-56`).

### 1.2 The command queue — channel 1

Payload (`VaCuusUIQueues.h:84-108`): a `Kind` enum, a `ViewId`, an `FString Payload`, an
`FIntPoint ViewSize`, a `bool bVisible`, a `uint64 LoadSerial`, and — for `AddView` only — a
`TUniquePtr<IVaCuusDocumentHost>` and a `TSharedPtr<FVaCuusViewStatus>`. It is move-only.

Container choice is documented, and the reason matters for a new channel:

```
 * Command transport. TSpscQueue (not TQueue/TCircularQueue: both carry
 * "planned for deprecation in favor of TSpscQueue" warnings in 5.8, and
 * TCircularQueue memzeroes its storage so it cannot hold an FString).
 *
 * SPSC means exactly ONE producer thread. That producer is the game thread --
 * the subsystem, the views, the widget's Tick and the console command all run
 * there. Anything else pushing commands needs TMpscQueue instead.
```
— `VaCuusUIQueues.h:110-119`

**Routing.** Every per-view command carries `ViewId`; `DrainCommands` looks it up in the UI
thread's `TMap<uint32, TUniquePtr<IVaCuusDocumentHost>> Hosts` (`VaCuusUIThread.h:263`) at
`VaCuusUIThread.cpp:881-888`, logging Verbose and dropping when the view is gone. Two kinds are
**thread-level** and are handled *before* the lookup so they still work with an empty view map:
`Shutdown` (`:838-861`) and `ClearAssetCaches` (`:875-879`). The `ClearAssetCaches` rationale is
the single most transferable design lesson in the file (`VaCuusUIThread.h:146-168`): a
process-global effect must not ride on a per-view command, because the case that needs it most is
the one with no live view.

**Coalescing.** The queue itself does none. `ViewSize` is applied on *every* routed kind before the
kind's own handler (`VaCuusUIThread.cpp:890-896`), and `FVaCuusRmlDocumentHost::SetViewSize` early-
outs on an unchanged size (`VaCuusRmlDocumentHost.cpp:131-134`), so a burst of resizes costs one
relayout. `UVaCuusView::Resize` also refuses to enqueue an unchanged size at all
(`VaCuusView.cpp:231-234`).

**Closing.** `Enqueue` drops (Verbose) once `bStopRequested` is set (`VaCuusUIThread.cpp:601-615`),
and every enqueue triggers the wake latch (`:614`).

### 1.3 The input queue — channel 2

Same container, same single-producer rule. Three properties worth copying:

- **One shared queue, not one per view**, with the `ViewId` on the payload. The reasons are spelled
  out at `VaCuusUIQueues.h:121-147`: global ordering across views, a trivial producer that never
  looks anything up in UI-thread-owned structures, and one drop rule with one log line.
- **A separate queue from commands**, so the ordering *between* them is deliberate:
  `RunFrame` drains all commands and then all input (`VaCuusUIThread.cpp:807-808`), which is what
  guarantees an event never reaches a context that is about to be resized or reloaded in the same
  frame (`VaCuusUIQueues.h:142-147`, `VaCuusUIThread.h:230-235`).
- **It deliberately does not wake the UI thread.** `EnqueueInput` has no `Trigger()`:

```cpp
	Event.ViewId = ViewId;
	Queues->Input.Enqueue(MoveTemp(Event));

	// No Trigger() here on purpose -- see the header. The frame that consumes this is
	// the one UVaCuusSubsystem::Tick asks for later in this same game frame.
```
— `VaCuusUIThread.cpp:594-598`; rationale at `VaCuusUIThread.h:176-179` ("waking here would turn a
mouse drag into one UI frame per motion event").

**A per-frame data channel should follow this rule exactly**: enqueue, do not trigger. The
subsystem's tick pulse is the only wake.

### 1.4 The wake latch — channel 3

`FEventRef WakeEvent{EEventMode::AutoReset}` (`VaCuusUIThread.h:277-278`). `Trigger()` is safe from
any thread and coalescing (`VaCuusUIThread.cpp:486-492`); `Run()` waits on it and runs exactly one
frame per wakeup (`VaCuusUIThread.cpp:731-748`). `Stop()` sets the flag *and* triggers, which is
load-bearing for the join (`VaCuusUIThread.cpp:474-484`).

### 1.5 The interactive-region snapshot — channel 4, and the template for a reverse channel

This is the closest existing analogue in *shape* to what M3 needs, run backwards.

Storage: `TTripleBuffer<FVaCuusInteractiveSnapshot> Snapshots`, private, with a three-method
facade on `FVaCuusViewStatus`:

```cpp
	FVaCuusInteractiveSnapshot& GetSnapshotWriteBuffer() { return Snapshots.GetWriteBuffer(); }
	void PublishSnapshot() { Snapshots.SwapWriteBuffers(); }
	const FVaCuusInteractiveSnapshot& AcquireSnapshot() { return Snapshots.SwapAndRead(); }
```
— `VaCuusViewStatus.h:125`, `:128`, `:147`

The design points, all of which transfer:

- **Build in place, never publish by value.** `GetSnapshotWriteBuffer()` hands out the write slot
  so the three buffers keep their array capacity between publishes, which is what makes a
  steady-state frame allocation-free; the comment also notes `TTripleBuffer::Write()` takes
  `const BufferType` **by value** (`TripleBuffer.h:199`) and so could not be used
  (`VaCuusViewStatus.h:116-124`).
- **Latest-wins is the desired coalescing**, not a compromise: "the game thread wants the newest
  geometry, never a backlog of old geometry" (`VaCuusViewStatus.h:150-156`).
- **A generation counter is mandatory**, because the read swap is a no-op when nothing was
  published and hands the same buffer back: "a caller must compare Generation rather than assume
  freshness" (`VaCuusViewStatus.h:130-147`). The producer stamps `++SnapshotGeneration` on every
  publish including empty ones (`VaCuusRmlDocumentHost.cpp:507`, `:555-559`).
- **The consumer copies out of the triple buffer once per frame** into a game-thread-owned cache
  (`UVaCuusView::CachedSnapshot`, `VaCuusView.h:410-419`), refreshed only when the generation moved
  (`VaCuusView.cpp:313-320`). The stated guarantee is "ONE STABLE SNAPSHOT PER DISPATCH BATCH, AT
  MOST ONE FRAME OLD" (`VaCuusView.h:266-296`).
- **It lives on the per-view shared object, not in a registry on the thread.** The reason is
  controller decision D7 at `VaCuusViewStatus.h:45-51`: this object "is already the one channel
  both sides of a view share, so the snapshot rides it rather than growing a second registry keyed
  by view id on FVaCuusUIThread."

### 1.6 The view-status channel — channel 5/6

`FVaCuusViewStatus` (`VaCuusViewStatus.h:56-157`) is a plain struct held by
`TSharedRef`/`TSharedPtr` on both sides: created in `UVaCuusSubsystem::CreateView`
(`VaCuusSubsystem.cpp:146`), handed to the UI thread on the `AddView` command
(`VaCuusSubsystem.cpp:150` → `VaCuusUIThread.cpp:501-511` → `AddView` at `:982`), and held by
`UVaCuusView::Status` (`VaCuusView.h:398-399`). It is "Held by a thread-safe TSharedRef so the UI
thread's copy stays valid even if the UObject view is garbage-collected first"
(`VaCuusViewStatus.h:53-55`), and `Invalidate()` deliberately keeps it
(`VaCuusView.cpp:36-38`).

The load-result protocol is the reference for a **result-carrying** field pair:

- GT stamps a strictly increasing serial into the command and into
  `LoadRequestSerial` (relaxed, GT-only) — `VaCuusView.cpp:141-143`.
- UI writes `LoadResult` **relaxed first**, then `LoadCompletedSerial` **release**:
  `VaCuusRmlDocumentHost.cpp:226-232`. "a game-thread reader that sees the new serial is guaranteed
  to see the matching result."
- GT reads `LoadCompletedSerial` **acquire** and only then the result:
  `VaCuusView.cpp:375`, `:397-398`.
- **One slot, on purpose** — coalescing is the contract, and the gap between serials is logged
  Verbose so it is not silent (`VaCuusView.cpp:386-391`, rationale at `VaCuusViewStatus.h:38-44`).

`FramesRecorded` / `FramesPublished` (`VaCuusViewStatus.h:88`, `:110`) are `fetch_add(..., release)`
from the UI thread at `VaCuusRmlDocumentHost.cpp:483-487`. The distinction between them is the idle
gate's per-view observable (§4.4).

### 1.7 UI → render thread — channel 7

Straight from the UI thread, no game-thread hop:

```cpp
		ENQUEUE_RENDER_COMMAND(VaCuusPublishUIFrame)(
			[LocalElement = Element, Buf = MoveTemp(Buffer)](FRHICommandListImmediate& RHICmdList) mutable
			{
				LocalElement->SetPendingBuffer_RenderThread(RHICmdList, MoveTemp(Buf));
			});
```
— `VaCuusRmlDocumentHost.cpp:448-452`; the legality note ("FRenderThreadCommandPipe has no
game-thread requirement") is at `:444-447`.

Buffers **queue rather than replace** on the render side, because each carries a resource *delta*
(`VaCuusSlateElement.h:32-41`); the drain draws only the newest and consumes older ones for
resources alone. Backlog is bounded at `MaxPendingBuffers = 4` (`VaCuusSlateElement.h:56-57`).

---

## 2. The UI thread's frame loop, step by step

`FVaCuusUIThread::RunFrame()` — `VaCuusUIThread.cpp:798-821`, in full:

| Step | Code | Line |
|---|---|---|
| 0 | `check(IsInUIThread())` — the affinity assert at the top of the frame | `:802` |
| 1 | `DrainCommands()` — AddView/RemoveView/ClearAssetCaches/load/close/resize/visible/shutdown | `:807` |
| 2 | `DrainInput()` — every queued `FVaCuusInputEvent` into `Host->GetContext()` | `:808` |
| 3 | **`// (data snapshots: M3)`** — the reserved slot, currently a comment | `:809` |
| 4 | for each host with `HasView()`: `RecordAndPublishFrame()` | `:814-820` |

`Run()` then does `FrameCount.fetch_add(1, release)` (`:744`). The inline path is identical
(`RunFrameInline`, `:408-421`).

Inside step 4, per view — `FVaCuusRmlDocumentHost::RecordAndPublishFrame()`,
`VaCuusRmlDocumentHost.cpp:368-489`:

| Sub-step | Code | Line |
|---|---|---|
| 4a | `check(IsInUIThread())`, `check(HasView())` | `:370-371` |
| 4b | one-shot thread-attribution log | `:373-382` |
| 4c | `bOwesClearingFrame = false` | `:393` |
| 4d | `Recorder->BeginFrame(ViewSize)` — **drains finished async image decodes** | `:395` (drain at `VaCuusRecordingRenderInterface.cpp:765`) |
| 4e | `VACUUS_PERF_SCOPE(Update); Context->Update();` | `:397-400` |
| 4f | `PublishInteractiveSnapshot()` — **unconditional, every recorded frame** | `:423` |
| 4g | `VACUUS_PERF_SCOPE(Record); Context->Render(); Buffer = Recorder->EndFrameAndPublish();` | `:429-434` |
| 4h | if `Buffer` is non-null, `ENQUEUE_RENDER_COMMAND` | `:441-453` |
| 4i | `FVaCuusPerfLog::AddUIFrame(bPublished)` | `:457` |
| 4j | idle-transition Verbose log | `:469-476` |
| 4k | `FramesRecorded` +1, `FramesPublished` +1 if published | `:478-488` |

And inside 4e, RmlUi's own order (`Source/Core/Context.cpp:181-219`):

```
181  bool Context::Update()
188    next_update_timeout = infinity
190-192  scroll controller
193-194  if (mouse_active) UpdateHoverChain(...)
195    // Update all the data models before updating properties and layout.
196-197  for (auto& data_model : data_models) data_model.second->Update(true);
205    root->Update(dp_ratio, dimensions)
207-214  for each document: doc->UpdateLayout(); doc->UpdatePosition();
216-217  ReleaseUnloadedDocuments();
219    return true;
```

### 2.1 Where "apply pending data-model updates" must go

**Correct insertion point: step 3 — `VaCuusUIThread.cpp:809`, after `DrainInput()` and before the
per-host record loop.** Equivalently (and preferably, see §2.2) a new
`IVaCuusDocumentHost` entry point invoked from that line for every host.

Why there:

1. **It must precede `Context::Update()`.** RmlUi evaluates data models *inside* `Update()`, at
   `Context.cpp:195-197`, and does so **before** `root->Update()` (`:205`) and the per-document
   `UpdateLayout()` (`:207-214`). So a `DirtyVariable` call issued before `Update()` becomes DOM
   text, layout, geometry and finally recorded draw commands **within the same UI frame** — the
   change reaches the screen with no extra frame of latency.
2. **It must follow `DrainCommands()`.** `AddView` is drained there
   (`VaCuusUIThread.cpp:863-867` → `:965-1000`), and that is where a view's `Rml::Context` is
   created (`VaCuusRmlDocumentHost.cpp:68`) — so an apply pass running *before* the drain has no
   context for a view created this frame and would have to drop or defer the first update.
   `RemoveView` is symmetric (`:869-873`). Iterating `Hosts` after the drain is also exactly what
   the record loop does (`:814-820`), so the two loops see the same set.

   **This is an ordering-hygiene argument, not a correctness one**, and the difference is worth
   stating so nobody "optimises" it back: applying either side of a `LoadDocument` converges before
   `Render()`. `Context::LoadDocument(Stream*)` runs `data_model.second->Update(false)` at
   `Source/Core/Context.cpp:304-305` — initialising the new document's views from whatever the model
   holds at that moment — and the `false` means it does **not** clear `dirty_variables`
   (`Source/Core/DataModel.cpp:377-378`), so anything dirtied before the load is applied again by
   the `Update(true)` inside `Context::Update()` a moment later. Either order shows the new values
   on the frame they were published.
3. **It should follow `DrainInput()`.** Input is what drives RmlUi's own `DataController`s, i.e.
   UI→model writes. Draining input first and then applying the game's snapshot makes "the game
   thread wins the tie within a frame" a stated rule rather than an accident. **INFERENCE** — no
   controller decision covers this yet and M3 should record one.

Why the alternatives are wrong:

- **Between `Update()` and `Render()`** (i.e. inside `RecordAndPublishFrame`, where the snapshot
  publish sits at `:423`). `DataModel::Update(true)` has already run and cleared `dirty_variables`
  (`Source/Core/DataModel.cpp:373-381`), so nothing applied here affects this frame's DOM at all.
  `Render()` records the *old* content, the gate correctly withholds it as unchanged, and the
  update surfaces one frame later. Not invisible — but a whole frame of avoidable latency on the
  channel whose entire purpose is per-frame freshness.
- **After `RecordAndPublishFrame`** — same, one frame late, plus it makes the "the value the
  snapshot published describes" question ambiguous.
- **On the game thread, calling `DirtyVariable` / touching the bound storage directly.** This
  violates the cardinal invariant (`spec §4`: "No RmlUi or QuickJS API is ever called from any other
  thread"), races `DataModel::dirty_variables` and every `DataView`, and would fire
  `check(FVaCuusUIThread::IsInUIThread())` only if the call happened to be routed through a guarded
  VaCuus function — the raw Rml call has no guard of its own.
- **Riding the existing command queue** (a new `EVaCuusCommandKind::SetModelValue`). Ordering would
  be correct, but the queue has **no coalescing** — a 60 Hz writer that outruns the UI thread by a
  frame grows the queue without bound, every entry carries an `FString`, and the drain would do the
  work N times. The existing channels reserve the unbounded queue for *events whose order is the
  payload* (input) and *state transitions* (commands), and use latest-wins triple buffering for
  *state* (the snapshot). Per-frame gameplay data is state.

### 2.2 The seam the apply should use

Two precedents say the same thing, and both are documented as deliberate:

- **Input dispatch** lives in `VaCuus` and reaches RmlUi through `Host->GetContext()`
  (`VaCuusUIThread.cpp:952-961`), so the whole RmlUi input vocabulary stays in one module and
  `SVaCuusWidget` never includes an RmlUi header (`VaCuusDocumentHost.h:109-123`).
- **Snapshot building** is the mirror image: the host hands its context to VaCuus code
  (`BuildVaCuusInteractiveSnapshot`, called at `VaCuusRmlDocumentHost.cpp:506-507`).

So the shape that matches: a `VaCuus/Private` translation unit owning the `Rml::DataModelHandle`
work, called from `VaCuusUIThread.cpp:809` with `Host->GetContext()`. The one thing that argues for
a new `IVaCuusDocumentHost` method instead is that the *model handle* must be stored per view and
`Hosts` is where per-view UI-thread state lives; a `TMap<uint32, FModelState>` on
`FVaCuusUIThread` would be the second per-view registry that D7 (`VaCuusViewStatus.h:45-51`)
explicitly argued against.

---

## 3. `UVaCuusView` and `UVaCuusSubsystem` as they stand

### 3.1 `UVaCuusView` — `VaCuusView.h:100-460`

`UCLASS(BlueprintType)`, `UObject`, **game thread only** (`VaCuusView.h:98`); every mutator opens
with `check(IsInGameThread())`. It "Owns nothing thread-affine. Every method here is a non-blocking
enqueue onto the UI thread's command queue, stamped with this view's id"
(`VaCuusView.h:86-98`).

**Blueprint-exposed surface — exactly six `UFUNCTION`s:**

| Function | Line |
|---|---|
| `void LoadDocument(const FString& VfsPath)` | `:125-126` |
| `void LoadDocumentFromMemory(const FString& RmlSource)` | `:174-175` |
| `void Close()` | `:183-184` |
| `void SetVisible(bool bVisible)` | `:190-191` |
| `bool IsViewValid() const` | `:194-195` |
| `bool IsLoadPending() const` | `:263-264` |

**C++-only public surface:** `InitializeView` (`:114`), `Invalidate` (`:118`), `GetDocumentPath`
(`:149`), `ReloadDocument` (`:171`), `Resize` (`:198`), `SendInput` (`:207`), `GetViewId` (`:209`),
`GetNumInputEventsQueued` (`:224`), `GetFramesRecorded` (`:234`), `GetFramesPublished` (`:250`),
`GetLastRequestedLoadSerial` / `GetLastCompletedLoadSerial` (`:259-260`), `GetSnapshot` (`:296`),
`UpdateIme` (`:311`), `NotifyImeTextInputClicked` (`:319`), `DetachIme` (`:328`), `GetImeStatus`
(`:355`), `GetImeContextForTesting` (`:365`), `PollStatus` (`:373`), `BeginDestroy` (`:377`).
One public delegate: `FOnVaCuusViewLoadCompleted OnLoadCompleted` (`:107`, declared `:83`).

**State** (`:395-459`): weak `OwningSubsystem`, `TSharedPtr<FVaCuusViewStatus> Status`,
`TSharedPtr<FVaCuusImeHandler> ImeHandler`, `FVaCuusInteractiveSnapshot CachedSnapshot`,
`uint32 ViewId`, `bool bRegistered`, `FString DocumentPath`, `uint64 NextLoadSerial = 1`,
`uint64 LastBroadcastLoadSerial`, `FIntPoint LastViewSize`.

**The `bRegistered` gate.** Every enqueue goes through `GetUIThread()`, which returns null unless
`bRegistered` **and** the subsystem is alive (`VaCuusView.cpp:119-126`); after `Invalidate()`
every call is a logged no-op. A new binding API must respect this: bind-on-a-dead-view is a
Warning-and-return, not a crash.

**The per-load serial is the pattern for "did my request land".** `NextLoadSerial++`,
store into the status, stamp into the command (`VaCuusView.cpp:141-143`). A model-generation
counter should look identical.

### 3.2 `UVaCuusSubsystem` — `VaCuusSubsystem.h:46-154`

`UGameInstanceSubsystem` + `FTickableGameObject`. **No `UFUNCTION`s at all** — it is reachable from
Blueprint only via the stock `GetGameInstanceSubsystem` node, with no exposed methods. The
`FTickableGameObject` choice is documented at `VaCuusSubsystem.h:37-44` (no
`UTickableGameInstanceSubsystem` in 5.8; `FTSTicker` fires after Slate has drawn).

Public surface:

- `Initialize` / `Deinitialize` (`:54-55`)
- the tickable overrides (`:60-67`), including `IsTickableWhenPaused() → true` (`:65`) and
  `GetTickableGameObjectWorld()` so multi-PIE ticks once per instance (`:67`)
- `UVaCuusView* CreateView(TUniquePtr<IVaCuusDocumentHost> Host, FIntPoint InitialViewSize)` (`:79`)
- `void DestroyView(UVaCuusView* View)` (`:82`)
- `static int32 ClearAssetCachesAndReloadAllViews(const TCHAR* Reason)` (`:108`)
- `FOnVaCuusDocumentsReloadRequested OnDocumentsReloadRequested` (`:123`, declared `:23`)
- `FVaCuusUIThread* GetUIThread() const` (`:126`)

Private: `int32 ReloadAllDocuments()` (`:146`), `TArray<TObjectPtr<UVaCuusView>> Views` (`:149-150`),
`bool bInitialized` (`:153`).

`Tick` — `VaCuusSubsystem.cpp:55-95` — is one perf scope around: poll every view (`:69-75`), then
either `RunFrameInline()` (`:87`) or `Trigger()` (`:93`). The scope placement is deliberate:
"Around the WHOLE body rather than only the loop: Trigger() is game-thread work this design costs,
and a scope that excluded it would understate the budget by exactly the amount nobody thought to
measure" (`:63-65`).

### 3.3 View lifetime, end to end

**Creation** — `UVaCuusSubsystem::CreateView`, `VaCuusSubsystem.cpp:118-160`:
`FVaCuusModule::Get().GetOrStartUIThread()` (`:130`, starts the thread on the first view in the
process, `VaCuus.cpp:73-110`) → refuse if `IsStopping()` (`:137-143`) → `AllocateViewId()` (`:145`,
process-unique, `VaCuusUIThread.cpp:494-499`) → `MakeShared<FVaCuusViewStatus>()` (`:146`) →
`EnqueueAddView` (`:150`) → `NewObject<UVaCuusView>(this)` + `InitializeView` + `Views.Add` (`:152-154`).
The host is booted on the UI thread when `AddView` is drained (`VaCuusUIThread.cpp:965-1000`), and a
host whose `Initialize()` fails is simply dropped, with no `Shutdown()` — a contract stated at
`VaCuusDocumentHost.h:52-53` and honoured at `VaCuusUIThread.cpp:982-988`.

**Destruction** — `DestroyView` (`VaCuusSubsystem.cpp:162-180`): `EnqueueRemoveView`,
`View->Invalidate()`, `Views.Remove`. `RemoveView` on the UI thread (`VaCuusUIThread.cpp:1002-1022`)
calls `Host->Shutdown()` and moves the host to `RetiredHosts` — it is **not destroyed**, because
RmlUi keeps a `RenderManager` keyed on the host's render interface until `Rml::Shutdown()`
(`VaCuusUIThread.h:265-272`). Retired hosts die in `Exit()` (`:791-792`).

**Deinitialize** (`VaCuusSubsystem.cpp:32-53`) removes and invalidates this instance's views and
**deliberately leaves the UI thread running** — which is exactly why RmlUi's asset caches outlive a
PIE session and need their own clear (§6).

**UMG path** — `UVaCuusWidget::RebuildWidget` (`VaCuusUMGWidget.cpp:28`) builds the
`FVaCuusSlateElement`, then `Subsystem->CreateView(MakeUnique<FVaCuusRmlDocumentHost>(NewElement),
FIntPoint::ZeroValue)` (`VaCuusUMGWidget.cpp:75-76`, with a comment at `:69-74` on why the initial
size is zero); `RetireView()` (`VaCuusUMGWidget.cpp:252`) detaches the Slate widget
first and then calls `DestroyView`. `RebuildWidget` can fire several times for one object, so both
`ReleaseSlateResources()` and `RebuildWidget()` retire any view they find — the invariant is
documented at `VaCuusUMGWidget.h:42-55`.

### 3.4 Where a `BindModel(...)` API fits

- It has to be **on `UVaCuusView`** (that is the per-view door, and `UVaCuusSubsystem` exposes
  nothing to Blueprint).
- Its header **cannot name an Rml type** (`VaCuus.Build.cs:27-33`, spec §10's engine-neutrality
  stance). `UStruct*`/`UScriptStruct*` + a `void*`/`FInstancedStruct` is the natural signature.
- It must be an **enqueue**, like everything else here, and must go through `GetUIThread()` so a
  dead view no-ops.
- **The model must exist on the context before the document loads.** `Element::SetDataModel`'s
  attach path looks the model up by the `data-model` attribute at `Source/Core/Element.cpp:2201-2216`
  and logs `LT_ERROR "Could not locate data model '%s' in element %s."` when it is missing.
  Since commands are FIFO from one producer, `View->BindModel(...)` before `View->LoadDocument(...)`
  on the game thread is sufficient — but the API should say so, because the failure is a log line
  in RmlUi's output and an unbound document, not an error the game thread ever sees.

---

## 4. The idle short-circuit — and does a data change reliably force a publish?

### 4.1 The gate, precisely

`FVaCuusRecordingRenderInterface::EndFrameAndPublish()` —
`VaCuusRecordingRenderInterface.cpp:872-958`. The decision is four lines:

```cpp
	const bool bHasResourceTraffic = Published->HasResourceTraffic();
	const uint64 ContentHash = VaCuusHashFrameContent(*Published);

	const bool bGateEnabled = CVarVaCuusIdleGate.GetValueOnAnyThread() != 0;
	if (bGateEnabled && Generation > 0 && ContentHash == LastPublishedContentHash && !bHasResourceTraffic)
	{
		++NumFramesSkipped;
		return nullptr;
	}

	LastPublishedContentHash = ContentHash;
	Published->Generation = ++Generation;
	return Published;
```
— `:936-957`

So a frame is **withheld** iff all four hold:

1. `vacuus.IdleGate != 0` (default 1; the kill switch, declared `:47-51` with its rationale at
   `:35-46` — "the gate's failure mode is a frozen UI with no error").
2. `Generation > 0` — the recorder has published at least once. A view's **first frame always
   publishes** (`:939-941`).
3. `VaCuusHashFrameContent(buffer) == LastPublishedContentHash`.
4. `!buffer->HasResourceTraffic()`.

**What the hash covers** (`VaCuusCommandBuffer.h:362-431`): a 3×`uint64` header of
`ViewSize.X`, `ViewSize.Y`, `Commands.Num()` (`:410-411`), then one XXH3 update per command over a
padding-free 112-byte image of all six `FVaCuusCommand` fields — `Type`, `Geometry`, `Texture`,
`Translation[2]`, `Scissor[4]`, `Transform[16]` (`:413-428`). It does **not** cover `Generation`
(by design, `:288-289`) nor any of the four resource arrays (`:290-294`).

**What `HasResourceTraffic()` covers** (`VaCuusCommandBuffer.h:220-241`):

```cpp
	return NewGeometry.Num() > 0 || NewTextures.Num() > 0 || ReleasedGeometry.Num() > 0 ||
		ReleasedTextures.Num() > 0;
```
— `:239-240`

with a member-count `static_assert` guarding completeness (`:233-237`), matching the one on
`FVaCuusCommand` (`:379-381`) and the layout assert (`:382-387`). The reasoning for those tripwires
— that a scalar added in `FVaCuusCommand`'s seven bytes of padding shifts neither `sizeof` nor any
`offsetof` and would silently escape the hash — is at `VaCuusCommandBuffer.h:48-77`.

**Ordering that makes the resource leg sufficient:** finished async image decodes are drained into
`NewTextures` at the *top* of `BeginFrame()` (`VaCuusRecordingRenderInterface.cpp:763-765`), i.e.
before `Context::Update()`, so they are already in the pending buffer when the gate looks
(`:786-789`).

**Per view, not per process**: `LastPublishedContentHash` lives on the recorder, and there is one
recorder per `Rml::Context` (`VaCuusRecordingRenderInterface.h:222-233`,
`VaCuusRmlDocumentHost.cpp:63`).

**What withholding costs**: nothing on screen. `Draw_RenderThread`'s replay is inside
`if (PendingBuffers.Num() > 0)` but the composite that follows is outside it and reads the
persistent RT directly, so the UI stays up with no buffer in flight
(`VaCuusRecordingRenderInterface.cpp:911-919`, citing `VaCuusSlateElement.cpp:91-140`).

Measured effect (spec §11): a 60 s soak of the static M1 HUD **published 2 of 13,074 recorded
frames**; the M2 demo published 75 of 13,571.

### 4.2 The traced answer: does a data-model change defeat the gate?

**Yes — reliably, and by two independent mechanisms — provided the update is applied by calling
`DirtyVariable` (or an equivalent that reaches `DataModel::dirty_variables`) on the UI thread
before `Context::Update()`.** The full trace, for the canonical case `<span>{{score}}</span>`
changing from `120` to `130`:

1. **Dirty.** `DataModelHandle::DirtyVariable(name)` inserts into `DataModel::dirty_variables`.
2. **Evaluate.** `Context::Update()` runs `data_model.second->Update(true)` at
   `Source/Core/Context.cpp:196-197` — *before* layout. `DataModel::Update` forwards to
   `views->Update(*this, dirty_variables)` and then clears the set
   (`Source/Core/DataModel.cpp:373-381`).
3. **Dispatch to views.** `DataViews::Update` collects every view registered against a dirty
   variable name (`Source/Core/DataView.cpp:90-95`), dedupes, sorts by tree depth, and calls
   `view->Update(model)` (`:106-114`).
4. **Write the DOM.** `DataViewText::Update` re-runs its expressions, compares
   `entry.value != value` (`Source/Core/DataViewDefault.cpp:354`) and — only on a real change —
   calls `ElementText::SetText(text)` (`:371`).
5. **Dirty layout.** `ElementText::SetText` early-outs when the string is equal; otherwise it
   assigns and calls `DirtyLayout()` (`Source/Core/ElementText.cpp:108-117`) — gated on
   `dirty_layout_on_change`, which is `true` from the constructor (`:101`) and is cleared only by
   `SuppressAutoLayout()` (`:354`). The **only** callers of that are `WidgetTextInput`
   (`Source/Core/Elements/WidgetTextInput.cpp:186`, `:189`), i.e. the text elements *inside* an
   `<input>`/`<textarea>`, which drive their own layout. **INFERENCE:** a binding onto a form
   control's value therefore does not travel this exact route (it goes through the value
   controller), and M3 should trace that case separately rather than assuming this one covers it.
6. **Re-layout, same frame.** Still inside the same `Context::Update()`,
   `doc->UpdateLayout()` runs at `Source/Core/Context.cpp:207-214`. Re-laying out the text runs
   `ClearLines()`/`AddLine()`, both of which set `geometry_dirty = true`
   (`Source/Core/ElementText.cpp:341`, `:351`).
7. **Regenerate geometry, during `Render()`.** `ElementText::OnRender` sees `geometry_dirty` and
   calls `GenerateGeometry` (`Source/Core/ElementText.cpp:147-148`). That function reuses existing
   geometry **only when the mesh compares equal**:

```cpp
		if (!geometry[i].geometry || geometry[i].geometry.GetMesh() != mesh_list[i].mesh)
			geometry[i].geometry = render_manager.MakeGeometry(std::move(mesh_list[i].mesh));
```
— `Source/Core/ElementText.cpp:535-536`

   `Mesh::operator==` compares vertices and indices (`Include/RmlUi/Core/Mesh.h:14`) and
   `Vertex::operator==` compares position, colour and tex_coord
   (`Include/RmlUi/Core/Vertex.h:20-23`). Different glyphs ⇒ different quads/UVs ⇒ mesh differs ⇒
   `MakeGeometry`. **The converse is the correct behaviour, not a hole:** if the new text produces
   a byte-identical mesh, the pixels are identical, and withholding is right.
8. **Both gate legs fire.**
   - The move-assignment onto the live `UniqueRenderResource` calls `ReleaseInDerived()` first
     (`Include/RmlUi/Core/UniqueRenderResource.h:30-35`), which reaches
     `RenderManager::ReleaseResource(const Geometry&)` → `render_interface->ReleaseGeometry(...)`
     (`Source/Core/RenderManager.cpp:345-354`) ⇒ **`ReleasedGeometry` non-empty**.
   - The new handle's compile is deferred to first draw
     (`RenderManager::GetCompiledGeometryHandle` → `render_interface->CompileGeometry` at
     `Source/Core/RenderManager.cpp:206`), i.e. to `Context::Render()`, which is *inside* the
     recorder's open frame ⇒ **`NewGeometry` non-empty**.
   - `HasResourceTraffic()` is therefore true ⇒ leg 4 of the gate fails ⇒ **publish**.
   - Independently, the recorder mints strictly increasing geometry handles
     (`NextGeometryHandle++`, `VaCuusRecordingRenderInterface.cpp:140`) and never recycles one —
     `ReleaseGeometry` only appends to `ReleasedGeometry` (`:173-181`) — so the `Geometry` field of
     every command drawing that element changes ⇒ **the content hash moves too** ⇒ leg 3 also fails.

   This is exactly the argument the codebase already makes for a `:hover` colour change
   (`VaCuusCommandBuffer.h:300-317`), and it is asserted end-to-end by
   `VaCuus.Render.IdleGate.HoverRecolour` (`Tests/VaCuusIdleGateTest.cpp:635-786`), which drives a
   real `Rml::Context`, requires the publish (`:741-745`), requires a *different* geometry handle
   (`:763-766`, `"...and re-compiled its geometry rather than recolouring it in place"`), and then
   proves the exclusion by showing that repainting the vertices behind an unchanged handle leaves
   the hash identical (`:767-781`).

Structural generalisation, from the same comment: `Rml::RenderInterface` has **no operation that
mutates an already-compiled geometry** — the vocabulary is `CompileGeometry` / `RenderGeometry` /
`ReleaseGeometry` (`Include/RmlUi/Core/RenderInterface.h:40-48`) plus consumers of a handle. Any
visual change therefore *must* arrive as new resource traffic. The one place a handle's content
changes underneath a stable handle is the async texture swap, which arrives through `NewTextures`
and so through the traffic leg (`VaCuusCommandBuffer.h:319-323`).

`data-if` / `data-for` / `data-class` / `data-style` are strictly easier cases: they add, remove or
restyle elements, all of which produce geometry traffic through the same route. **INFERENCE**, but
on the same structural argument.

### 4.3 What the update path must guarantee — the invisible-bug shapes

The gate is safe *given the path is correct*. Three ways to get it wrong, each of which produces a
silently stale UI with no log line anywhere:

1. **Writing the bound storage without dirtying the variable.** `DataViews::Update` only visits
   views reached from `dirty_variables` (`Source/Core/DataView.cpp:90-95`). A value changed in
   memory with no `DirtyVariable` never reaches the DOM, the recorded frame is byte-identical, the
   gate withholds, and the screen keeps the old number forever. **This is the single most likely
   M3 bug and the gate makes it invisible rather than causing it.**
   Nastier still: newly added views are *always* updated — `views_to_add` is unconditionally pushed
   into `dirty_views` at `Source/Core/DataView.cpp:76-88` — so the value appears correct after any
   document reload, which turns a systematic bug into a heisenbug that "only happens until you
   press `vacuus.ReloadUI`".
2. **Applying after `Context::Update()`** (§2.1) — one frame of latency, and if a future
   "only run a UI frame when something changed" optimisation ever lands, potentially permanent.
3. **Suppressing the update when the value did not change, in VaCuus's own diff.** This is *fine* —
   it is what `DataViewText::Update` already does at `Source/Core/DataViewDefault.cpp:354` — as long
   as the comparison is over the value actually bound, not over a rounded/formatted proxy.

**The guarantee to write down for M3:** *every applied field whose value differs from the one the UI
thread last applied must reach `DirtyVariable` in the same UI frame, before `Context::Update()`.*
Anything weaker is only detectable by looking at the screen.

**And the observable to build alongside it**, because the codebase's own standard demands one: the
idle gate was only testable because `GetNumFramesPublished()` / `GetNumFramesSkipped()` exist
(`VaCuusRecordingRenderInterface.h:163-172`: "an invariant with no observable cannot be tested").
A model channel needs the equivalent — an applied-generation counter on the view status and a count
of variables dirtied per frame — or "the value got through" is untestable.

### 4.4 The per-view idle observables that already exist

- `UVaCuusView::GetFramesRecorded()` / `GetFramesPublished()` (`VaCuusView.h:234`, `:250`) — read
  against each other; divergence *is* the view's idle signal (`:236-249`).
- The per-**transition** Verbose log at `VaCuusRmlDocumentHost.cpp:469-476` ("View %u UI frames went
  IDLE/ACTIVE"), deliberately not per frame ("a static HUD records ~13,000 frames per minute and
  publishes one", `:460-464`).
- The process-wide `PerfLog window UI frames published=... skipped=... (%.1f%% idle)` line
  (`VaCuusStats.cpp:254-259`), with an explicit warning at `:237-248` not to read `skipped` as
  missing `Replay` samples.
- `vacuus.IdleGate 0` to rule the gate out entirely.

---

## 5. The cost budget and how it is measured

### 5.1 The instrument

`VACUUS_PERF_SCOPE(Name)` (`VaCuusStats.h:138-140`) expands to a `SCOPE_CYCLE_COUNTER` **and** an
RAII wall-clock sampler feeding `FVaCuusPerfLog` (`VaCuusStats.h:120-135`). Two readouts from one
line: `stat vacuus` (interactive, `STATGROUP_VaCuus`, `VaCuusStats.h:8`) and
`vacuus.M1HUD.PerfLog 1` (5-second avg/p50/p99/max windows to the log, `VaCuusStats.cpp:20-23`,
printing at `:230-268`).

Seven scopes (`VaCuusStats.h:29-41`, `:59-87`):

| Scope | Thread | Rate | Where |
|---|---|---|---|
| `Update` | UI | 1/frame/view | `VaCuusRmlDocumentHost.cpp:398` — wraps `Context::Update()` only |
| `Record` | UI | 1/frame/view | `VaCuusRmlDocumentHost.cpp:431` — wraps `Context::Render()` **and** `EndFrameAndPublish()`, so the content hash is inside it, deliberately (`:425-428`) |
| `Replay` | RT | 1/paint that found a buffer | `VaCuusReplayRenderer.cpp:42` |
| `Composite` | RT | 1/paint | `VaCuusSlateElement.cpp:101` |
| `GameTick` | GT | 1/frame | `VaCuusSubsystem.cpp:66` — the whole `Tick` body |
| `SlateTick` | GT | 1/frame/hosted view | `SVaCuusWidget.cpp:200` |
| `Input` | GT | 1/**event** | `SVaCuusWidget.cpp:402, 420, 494, 547, 606, 720, 766, 798, 861` (nine sites; `:606` is `OnCursorQuery`, a query rather than an event — see §9.4) |

The three GT scopes are deliberately separate because only their **sum** is the per-frame figure:
"GameTick and SlateTick are once per frame each, Input is once per EVENT. Folding them into a
single scope would make avg/p50 the average of a frame's parts rather than of a frame"
(`VaCuusStats.h:34-38`).

Counters: `FVaCuusPerfLog::AddDraws` (`VaCuusReplayRenderer.cpp:374`) and `AddUIFrame(bPublished)`
(`VaCuusRmlDocumentHost.cpp:457`).

### 5.2 What is outside the scopes today

Verified by reading `SVaCuusWidget.cpp` handler by handler:

- **`OnPaint`** (`SVaCuusWidget.cpp:271-287`) — **no scope**, once per frame per view, and it
  contains an `ENQUEUE_RENDER_COMMAND(VaCuusSetDestRect)` with a lambda allocation (`:279-283`) plus
  `FSlateDrawElement::MakeCustom` (`:285`).
- **`OnMouseEnter`** (`:568`), **`OnMouseLeave`** (`:578`), **`OnMouseCaptureLost`** (`:586`) — no scope.
- **`OnFocusReceived`** (`:1205`), **`OnFocusLost`** (`:1224`) — no scope.
- `OnMouseButtonDoubleClick` (`:530`) has no scope of its own but forwards to `OnMouseButtonDown`
  (`:544`), which does, so it is covered.
- `FVaCuusPerfLog::TickLog()` is **deliberately outside** `SlateTick` (`SVaCuusWidget.cpp:240-242`),
  so the logger's own 5-second print is never the max sample of the window it prints.

Spec §11 records the consequence honestly: the game-thread gate is
**"met with ~25× headroom; an inference from margin, not a complete measurement"** — measured
0.004 ms typical / 0.011 ms p99-sum @1080p — and says **"Whoever tightens this budget must add
`OnPaint` to the scope set first."** That is bead `VaCuus-akj.6.38`.

### 5.3 What that means for a per-frame diff-and-copy on the game thread

- **A new game-thread cost lands inside `GameTick` for free** *if* it is driven from
  `UVaCuusSubsystem::Tick` — the scope is around the whole body (`VaCuusSubsystem.cpp:66`), and the
  comment at `:63-65` says that placement is intentional. Driving the diff from anywhere else
  (a component tick, a Blueprint node, an actor) puts it **outside every scope**, and the budget
  becomes an inference on top of an inference.
- **INFERENCE:** the current real headroom is not "25×" for a *new* per-frame cost, because 0.004 ms
  typical is the *measured* part only. The honest statement is: the budget is 0.10 ms/frame; the
  measured part is 0.004 ms typical / 0.011 ms p99-sum; the unmeasured part is one render-command
  enqueue plus rare handlers (spec §11 argues single-digit µs). A diff-and-copy over a
  few-dozen-field struct at 60 Hz should be budgeted against ~0.09 ms, and it should be measured,
  not argued.
- **What to add.** A new `EScope` in `FVaCuusPerfLog::EScope` (`VaCuusStats.h:59-87`) plus a name in
  `GScopeNames` (`VaCuusStats.cpp:29-37`, guarded by a `static_assert` on the count at `:38-39`) and
  a `DECLARE_CYCLE_STAT_EXTERN` + `DEFINE_STAT` pair (`VaCuusStats.h:29-45`, `VaCuusStats.cpp:10-18`).
  Name it for its rate: the diff is once per frame per bound model, which is a third rate again.
- **Caveat on the tool itself:** `TickLog` re-copies and re-sorts the whole cumulative array every
  5 s on the game thread while holding the lock the UI and render threads also take
  (`VaCuusStats.cpp:224-227`, `:261-268`, lock at `:181`). That is bead `VaCuus-akj.6.39`. With
  `Input` already producing one sample per event, a per-model scope adds another unbounded array;
  a long soak with the PerfLog on will get worse, not better.
- **The UI-thread side lands in `Update`**, since `DirtyVariable` costs are paid inside
  `Context::Update()`'s data-model loop (`Source/Core/Context.cpp:196-197`) — which is inside the
  `Update` scope (`VaCuusRmlDocumentHost.cpp:397-400`). The *apply* step at
  `VaCuusUIThread.cpp:809`, however, is **outside every scope** — `RunFrame` has none. If the apply
  does the string/array copying, it needs its own scope or the UI-thread budget acquires the same
  blind spot the game thread already has.

---

## 6. Live reload, and what a bound model must survive

### 6.1 The reload path

Two triggers, one door:

- **File watcher** (editor only, `FVaCuusLiveReload`, `VaCuusLiveReload.h:45-248`): watches the
  DevUI roots, filters by action + extension (`ShouldTrackChange`, `:96`), debounces
  (`QuietSeconds = 0.15`, `MaxDeferSeconds = 1.0`, `:194`, `:200`), then `FlushAt` →
  `ReloadAllLiveViews(TEXT("file change"))` (`VaCuusLiveReload.cpp:461`).
- **`vacuus.ReloadUI`** console command (`VaCuusLiveReload.cpp:505-515`).

Both call `FVaCuusLiveReload::ReloadAllLiveViews` (`:469-485`), which is now a one-line wrapper over
`UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews(Reason)`.

That function (`VaCuusSubsystem.cpp:182-255`) does, in order:

1. `UIThread->EnqueueClearAssetCaches()` — **first, and unconditionally**, even when zero views are
   found (`:199-204`). On the UI thread this is `Rml::Factory::ClearStyleSheetCache()` +
   `ClearTemplateCache()` (`VaCuusUIThread.cpp:1041-1042`).
2. Walk `GEngine->GetWorldContexts()` (`:222`), find each `UVaCuusSubsystem` (`:239`), call the
   **private** `ReloadAllDocuments()` (`:248`).
3. `ReloadAllDocuments` (`:257-276`) calls `UVaCuusView::ReloadDocument()` on every view with a
   non-empty `DocumentPath`, then broadcasts `OnDocumentsReloadRequested(NumReloaded)` for views the
   loop cannot reach (an inline fallback has no path — `VaCuusView.h:141-147`).
4. `ReloadDocument` (`VaCuusView.cpp:146-175`) allocates a new load serial and enqueues
   `EnqueueLoadDocumentFile(ViewId, DocumentPath, Serial, LastViewSize)`.

The pairing of "clear the caches" and "reload the views" is enforced structurally: the fan-out is
private and the static entry point is a member, so nothing outside the class can call half of it
(`VaCuusSubsystem.h:89-108`, `:128-146`). **A data-binding hook that wants to trigger a reload must
call `ClearAssetCachesAndReloadAllViews`, never invent its own fan-out** — the header explicitly
names "M3's data binding" as the caller it is protecting against (`VaCuusSubsystem.h:100-103`).

Textures are deliberately **not** released on reload (`VaCuusLiveReload.h:38-43`).

### 6.2 What survives a reload, for a bound model

The reload is a `LoadDocumentFile` command; on the UI thread that runs
`FVaCuusRmlDocumentHost::LoadDocumentFromFile` → `Context->LoadDocument(...)` → `AdoptDocument`
(`VaCuusRmlDocumentHost.cpp:148-217`), which calls `CloseDocument()` and then `Document->Show(...)`.
**The `Rml::Context` is untouched.**

That answers the question directly:

| Thing | Survives a reload? | Evidence |
|---|---|---|
| The `Rml::Context` | **Yes** | only `Shutdown()` calls `Rml::RemoveContext` (`VaCuusRmlDocumentHost.cpp:104-111`) |
| The recorder, and therefore `LastPublishedContentHash` | **Yes** | recorder is created in `Initialize()` only (`:63`) |
| The **data model** and its variable *values* | **Yes** | models live in `Context::data_models` (`Include/RmlUi/Core/Context.h:383`); nothing in the load path removes one |
| The `DataView`s / `DataController`s bound to elements | **No — destroyed and rebuilt** | `Element::SetDataModel(nullptr)` on detach → `DataModel::OnElementRemove` (`Source/Core/Element.cpp:2156-2157`, `Source/Core/DataModel.cpp:365-370`); the new document re-attaches by name at `Source/Core/Element.cpp:2201-2216` |
| The view's `FVaCuusViewStatus`, `ViewId`, `DocumentPath`, snapshot generation | **Yes** | all owned by `UVaCuusView` / the host, none touched by a load |
| RmlUi's parsed stylesheet/template caches | **No** — dropped on purpose | `VaCuusUIThread.cpp:1041-1042` |
| The IME context's shadow state | Stale for ≥1 game frame | bead `VaCuus-akj.6.19` |

**So: the model must be rebuilt only if its *name* or its *bound addresses* change; the values do
not need re-publishing.** RmlUi re-initialises the new document's views itself, twice over:

- during the load, `Context::LoadDocument(Stream*)` runs `data_model.second->Update(false)` at
  `Source/Core/Context.cpp:304-305` — after the `load` event, before `document->UpdateDocument()`,
  and **without clearing `dirty_variables`** (the `false` argument; see
  `Source/Core/DataModel.cpp:377-378`), so any pending dirty flags survive into the next frame;
- and in any case `DataViews::Update` unconditionally treats every newly added view as dirty
  (`Source/Core/DataView.cpp:76-88`).

**Two consequences an implementer must not miss:**

1. **Do not tear the model down and recreate it on every reload.** Doing so would drop the values
   between the destroy and the next game-thread publish, and — worse — would race the load: if the
   document is instanced before the model is re-created, `Element::SetDataModel` logs
   `"Could not locate data model '%s'"` (`Source/Core/Element.cpp:2214`) and the document comes up
   unbound. The correct action on reload is **nothing**.
2. **Because RmlUi re-initialises new views from current values regardless of dirtiness, a
   "forgot to call `DirtyVariable`" bug is masked by every reload.** See §4.3.1. Any test for the
   dirty path must *not* reload between the write and the assertion.

---

## 7. Conventions the implementer must follow

### 7.1 Thread affinity

- The assertion is `check(FVaCuusUIThread::IsInUIThread())`, backed by a single file-scope
  `std::atomic<uint32> GVaCuusUIThreadId` (`VaCuusUIThread.cpp:37`, read at `:664-668`).
- **Every** RmlUi-touching function opens with it: `VaCuusRmlDocumentHost.cpp:55, 84, 129, 150, 166,
  181, 236, 289, 341, 364, 370, 493, 546`; `VaCuusUIThread.cpp:756, 802, 825, 929, 967, 1004, 1026,
  1053`; `VaCuusSystemInterface.cpp:51`.
- Game-thread functions open with `check(IsInGameThread())`: `VaCuusView.cpp:17, 28, 53, 76, 88, 130,
  148, 179, 201, 214, 229, 252, 307, 351`; `VaCuusSubsystem.cpp:120, 164, 184, 259`.
- The **one** deliberate exception is the recorder's own `CheckOwnerThread()`, which is an
  `ensureMsgf` rather than a `check`, with the trade spelled out:
  "those guard RmlUi itself, whose global state a wrong-thread call silently corrupts... This one
  guards OUR buffer, and the worst case is a mis-recorded frame — recoverable"
  (`VaCuusRecordingRenderInterface.cpp:973-988`).
- Comments state affinity at the type level, not just the call level: `VaCuusDocumentHost.h:29-39`,
  `VaCuusRmlDocumentHost.h:26-34`, `VaCuusSlateElement.h:19-22`, `VaCuusView.h:98`.

### 7.2 Tests

Two shapes, both `#if WITH_DEV_AUTOMATION_TESTS`, both
`IMPLEMENT_SIMPLE_AUTOMATION_TEST(FName, "VaCuus.<Area>.<Case>", EditorContext | EngineFilter)`:

- **Unit, driven directly.** `Tests/VaCuusIdleGateTest.cpp` drives `FVaCuusRecordingRenderInterface`
  with no `Rml::Context` at all, and **says so at the top of the file** with the consequences
  (`:19-41`). Six tests, one `IMPLEMENT_SIMPLE_AUTOMATION_TEST` each: `:78`, `:156`, `:288`, `:339`,
  `:428`, `:496`.
- **Integration, with a real `Rml::Context`.** `VaCuus.Render.IdleGate.HoverRecolour`
  (`Tests/VaCuusIdleGateTest.cpp:635-786`) boots RmlUi on the test thread, guarded by
  `TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized())`
  and `ON_SCOPE_EXIT` teardown for both the engine and the context. The same precondition guard is
  in `Tests/VaCuusUIThreadTest.cpp:25-37` with a long comment on why a confusing pass is worse than
  a refusal.
- **Probe hosts.** A test-only `IVaCuusDocumentHost` is the standard way to exercise the UI thread
  headlessly. Two flavours: a **real-context probe** (`Tests/VaCuusMultiViewTest.cpp:53`,
  `FProbeHost` — real `Rml::Context`, real recorder, no Slate element, publishing into a shared
  `FProbe` of atomics declared at `:27`) and a **stub** that does nothing at all when the assertion is a game-thread
  fact (`Tests/VaCuusReloadTest.cpp:24-44`, `FStubHost`, with the justification written down).
  There are five near-identical copies today — bead `VaCuus-akj.6.15`; **do not add a sixth.**
- **Standalone game instance** for subsystem-level tests: `FStandaloneInstance` in
  `Tests/VaCuusReloadTest.cpp:46-80` uses `UGameInstance::InitializeStandalone()` so the instance
  really appears in `GEngine->GetWorldContexts()`.
- **PIE proof harnesses** live behind `-vacuusproof` (`Tests/VaCuusProofCommon.h:43-57`, with the
  reasoning for a run-time gate over `EAutomationTestFlags::Disabled`), write before/after
  screenshots to `Saved/VaCuusProof`, and **verify the files exist** rather than trusting queued
  latent commands (`VaCuusProofCommon.h:85-103`). Artefacts are committed under
  `docs/research/proofs/<task>/`.
- **Skip honestly** when the configuration cannot support the test: `AddInfo(...)` + `return true`
  (`Tests/VaCuusUIThreadTest.cpp:16-23`).

### 7.3 The "restore-the-bug" proof standard

The house rule is that a guard is only credible once the bug it prevents has been reintroduced and
seen to fire. Examples to copy:

- `"Verified by restoring the bug rather than argued: adding a fifth resource map to the struct
  without naming it below fails this assert at compile time, with this message."` —
  `VaCuusCommandBuffer.h:231-232`
- `"that hole was verified by building with such a field present, not argued"` —
  `VaCuusCommandBuffer.h:368-369`
- The positive control inside `HoverRecolour`: without asserting the colour really differs, "the
  test would pass while the colour change itself went unnoticed"
  (`Tests/VaCuusIdleGateTest.cpp:748-762`), followed by the *exclusion* assertion showing the hash
  alone is blind to vertex colour (`:767-781`).
- **Arm the gate before asserting it fired**: `HoverRecolour` records an identical frame and
  requires it to be withheld first (`:731-736`) — "Without this the publish below would prove
  nothing: a recorder that published everything would pass every assertion after it."
- **Relabel an assertion when its evidence does not support its name**, and say so:
  `Tests/VaCuusIdleGateTest.cpp:597-605`.

### 7.4 Comment and citation convention

- Comments explain **why**, in caps-led paragraphs for the load-bearing ones, and they cite
  `file:line` for every claim about code outside the current file — including the vendored RmlUi
  (`ThirdParty/RmlUi/Source/Core/Context.cpp:219`) and the engine
  (`ModuleManager.cpp:940-944`, `TripleBuffer.h:199`, `SlateApplication.cpp:6046-6050`).
- Corrections of previous comments are written as corrections, not silently rewritten:
  `"This sentence used to claim the empty frames keep publishing; that has been false since M2
  Task 12"` (`VaCuusDocumentHost.h:96-99`); `"The call-overhead argument that used to lead here was
  WRONG IN DIRECTION and is gone"` (`VaCuusCommandBuffer.h:400-403`); `"NOT relabelled idly: this
  used to claim..."` (`Tests/VaCuusIdleGateTest.cpp:~597-605`).
- Decisions carry their identifier (`D7`, `D9`, `D11`, `D13`, `D14a/b`, `D15`, `D17`, `D18`, `D19`,
  `D20`, `D21`, `D22`) and beads carry theirs (`VaCuus-akj.6.13`, `.6.12`, `.6.34`, `.6.36`).
- Alternatives that were rejected are named and the reason recorded, so nobody "restores" them:
  `VaCuusSubsystem.cpp:228-238` ("NO `Context.World() != nullptr` CHECK either... deliberately, so
  nobody 'restores' it").

---

## 8. Existing debt a data-binding implementer will collide with

From `/w/Unreal/VaCuus/.beads/issues.jsonl` (53 issues; 22 open). `VaCuus-akj.7` is M3 itself:
*"FVaCuusModelBuilder, snapshot protocol per spec §6, BP nodes; reference HUD completed and driven
from USTRUCT at 60Hz. Accept: spec §11 gates pass (except provisional replay row)."*

**Will collide, high confidence:**

| Bead | P | Why it lands on M3 |
|---|---|---|
| `VaCuus-akj.6.38` — *Game-thread work outside the perf scopes (add OnPaint first)* | 3 | M3's acceptance is "§11 gates pass", and the game-thread gate is currently an inference from margin. A per-frame diff-and-copy is the first new game-thread cost since the scopes were drawn. **Fix this before claiming the gate.** |
| `VaCuus-akj.6.39` — *PerfLog TickLog sorts an unbounded array on the game thread holding the shared lock* | 3 | M3 will add at least one scope and will be measured with 60 s soaks. The tool degrades with sample count and stalls the UI and render threads while it prints. |
| `VaCuus-akj.6.42` — *Test coverage gaps the final review named* | 3 | Gap (a): **no test drives the idle gate through a real `Rml::Context`** except `HoverRecolour`; gap (b): **no test covers live reload through the gate**. §4 and §6 of this note are exactly those two gaps, and M3 is the milestone that has to close them for data changes. |
| `VaCuus-akj.6.15` — *five near-identical FProbeHost copies across two modules* | 3 | M3 needs a probe host with a data model on it. That is copy six. The bead says "Do before another test adds a sixth." |

**Will interact, worth knowing:**

| Bead | P | Note |
|---|---|---|
| `VaCuus-akj.6.19` — IME stale-shadow window after a document reload | 2 | Same reload path M3 must not disturb; unobservable on Linux. |
| `VaCuus-akj.6.40` — inline mode makes `IsInUIThread()` true on the game thread at all times | 4 | In `-nothreading`/commandlet configs the affinity assertion protecting a new data-apply path does **not** mean what its comment says. Any M3 automation test run in a commandlet inherits this. |
| `VaCuus-akj.6.37` — citation drift the correction pass did not reach | 4 | The convention is `file:line`; both clusters confirmed stale, and the bead's own numbers have drifted too (see §9.2, §9.3). New comments will be held to a standard the tree does not yet fully meet. |
| `VaCuus-akj.6.9` — itlib `flat_map::at()` out-of-range is UB with asserts compiled out | 3 | RmlUi's data model uses itlib containers throughout `DataModel`/`DataTypeRegister`; a shipping build with a bad variable name is the shape of this risk. |
| `VaCuus-akj.6.27` — decodes launched while a view is unsized sit undrained | 3 | Same "recorded but never published" family as the gate; a UMG-hosted view starts at `FIntPoint::ZeroValue` (`VaCuusUMGWidget.cpp:75-76`) and only gets a size on the first `SVaCuusWidget::Tick` (`SVaCuusWidget.cpp:216`). A model bound before the first size will apply into a context that records no frames — see §9.7. |
| `VaCuus-akj.6.18` — move `vacuus.ReloadUI` into the runtime module | 3 | Today reload is editor-only for the *trigger*; the dispatch is already runtime (`VaCuusSubsystem::ClearAssetCachesAndReloadAllViews`). |

---

## 9. Things that look wrong, or worth flagging

Found on a fresh read; none is a blocker, all are cheap.

### 9.1 `RunFrame()` has no perf scope at all

`FVaCuusUIThread::RunFrame` (`VaCuusUIThread.cpp:798-821`) is unscoped. `DrainCommands` (which can
run a full document parse and layout via `LoadDocumentFromFile`) and `DrainInput` (which runs RmlUi
hit-testing, focus changes and IME mutation) are therefore **outside every UI-thread measurement**;
only `Update` and `Record` are sampled, and both are inside `RecordAndPublishFrame`. The UI-thread
budget in spec §11 is stated as "Update + command record", so this is not a *mis*-statement — but it
is the exact same blind spot that bead `VaCuus-akj.6.38` records for the game thread, and it is the
step M3's data apply is about to land in. Worth a bead of its own.

### 9.2 Two stale RmlUi citations in `VaCuusUIThread.cpp` — confirmed, and the bead that files them has drifted too

Bead `VaCuus-akj.6.37(a)` names them. Opened and checked, all four numbers:

- `VaCuusUIThread.cpp:218` cites `Element::Blur` as `Element.cpp:2016-2031`. `Element.cpp:2016` is
  the opening line of `Element::ProcessDefaultAction`. The real `Element::Blur` is
  `Element.cpp:1208-1225`, and the *claim* is true — `parent->Focus()` at `:1214`.
- `VaCuusUIThread.cpp:220` cites `Element::Focus` as `Element.cpp:2003-2008`, which is inside
  `Element::GetClosestScrollableContainer` (`:1996`). The real `Element::Focus` is
  `Element.cpp:1179-1206`, and the claim is again true — the only gate is
  `focus_property == Style::Focus::None` at `:1182-1183`, with no tab-index test.
- **The bead's own line numbers are two lines off**: it says `VaCuusUIThread.cpp:220 and :222`;
  the citations are at `:218` and `:220`.

Both cited ranges land in unrelated functions, so a reader who follows them reads code that does
not support the sentence — exactly the failure mode this project has been bitten by. Cheap to fix
and worth fixing before M3 adds comments held to the same standard. It also matters directly here:
§6's whole argument rests on `Element.cpp:2145-2216`, i.e. very near where those stale citations now
point, so a reader checking §6 against them would be doubly misled.

### 9.3 Minor citation drift in `VaCuusCommandBuffer.h`'s RmlUi trail

Spot-checked the whole trail at `VaCuusCommandBuffer.h:325-347`. Correct: `RenderManager.cpp:345-354`
(`ReleaseResource(const Geometry&)` is 345-354 exactly), `RenderManager.cpp:205-206` (the deferred
`CompileGeometry` is at 206), `UniqueRenderResource.h:30-35` (the move-assign is 30-35),
`ElementText.cpp:530-539` (the mesh-reuse block is 530-539), `Mesh.h:14`, `Vertex.h:20-23`.
Slightly off by 2-3 lines: `ElementBackgroundBorder.cpp:131-137` (the pair is at 132 and 137, so the
range is right at the top end and one line early at the bottom); `ElementText.cpp:425-428` and
`:430-439` (the `geometry_dirty = true` is at 428 and the decoration re-colour block is 430-440).
Nothing misleading — recording it only because §7.4's standard is exactness and because a reader
checking this trail will find the same small offsets I did.

### 9.4 The `Input` scope's own comment does not quite describe one of its nine sites

`VaCuusStats.h:78-80` defines the `Input` scope as *"one sample per input EVENT, covering the whole
handler: the screen-to-view transform, the snapshot scan that produces the FReply, and the
enqueue."* Eight of the nine sites match. The ninth, `SVaCuusWidget.cpp:606`, is
`SVaCuusWidget::OnCursorQuery` (`:604`) — a Slate **query**, not an event: it is `const`, it does the
transform and the snapshot scan but produces an `FCursorReply` and enqueues nothing
(`:606-612`). Including it in the budget is right — it is real game-thread cost VaCuus causes — but
it is sampled at Slate's cursor-query rate, not at the input-event rate, so the `Input` scope's
avg/p50 mix two populations. Named here because §5's numbers are read against that sentence.

### 9.5 `FVaCuusPerfLog::AddSample` reads the cvar twice per call

`AddSample` calls `IsEnabled()` (`VaCuusStats.cpp:119`) and then re-tests `State.bEnabled` under the
lock (`:127`). With `Input` already sampling once per event, and M3 adding a per-frame scope, this is
two atomic cvar reads plus a lock acquisition per sample. Not a bug — the double test is what makes
enable/disable transitions clean — but it is the second half of the `VaCuus-akj.6.39` cost story and
is worth measuring alongside it.

### 9.6 The `AddView` failure path leaves the game thread holding a live handle

`UVaCuusSubsystem::CreateView` creates and returns the `UVaCuusView` **unconditionally** after
enqueuing `AddView` (`VaCuusSubsystem.cpp:150-159`). If the host's `Initialize()` then fails on the
UI thread, `AddView` logs `"View %u failed to boot; it will produce no frames"` and drops it
(`VaCuusUIThread.cpp:982-988`) — but the game thread's `UVaCuusView` stays `bRegistered == true`, so
`IsViewValid()` keeps returning true and every subsequent command is routed to a `ViewId` with no
host and dropped Verbose (`VaCuusUIThread.cpp:881-887`). This is pre-existing and benign today
(nothing polls for it), but a data-binding API that reports "bound" on such a view would be
reporting a lie. **INFERENCE:** the cheap fix is a `Failed` value on the existing
`EVaCuusLoadResult`-style channel, or an explicit boot-status field on `FVaCuusViewStatus`.

### 9.7 `IVaCuusDocumentHost` has no hook that runs for a host without `HasView()`

`RunFrame` only calls into hosts where `HasView()` is true (`VaCuusUIThread.cpp:816-819`), and
`HasView()` requires a document **and** a positive view size
(`VaCuusRmlDocumentHost.cpp:354-355`). A view that is bound but has no document yet, or is not sized
yet (every UMG-hosted view before its first `SVaCuusWidget::Tick`), gets no per-frame callback at
all. If the data apply is added as a host method called from inside the record loop, model updates
would silently stop for exactly those views — and then arrive in a burst when the document loads.
Applying at `VaCuusUIThread.cpp:809` over **all** hosts, not only recordable ones, avoids this;
`Context` exists from `Initialize()` onward (`VaCuusRmlDocumentHost.cpp:68`), so it is safe.

---

## 10. The shape a new data channel should take — summary

Assembled from the four channels that already work:

1. **Direction and container.** GT → UI, **per view**, **latest-wins**. That is a
   `TTripleBuffer<T>` on `FVaCuusViewStatus`, mirroring the snapshot exactly but with the
   producer/consumer roles swapped (`VaCuusViewStatus.h:112-156`). Not the SPSC command queue —
   per-frame state has no business in an unbounded FIFO (§2.1).
2. **Build in place, never publish by value**, so a steady-state frame allocates nothing
   (`VaCuusViewStatus.h:116-124`).
3. **A strictly increasing generation on every publish**, because the consumer's read swap is a
   no-op when nothing was published and hands the same buffer back
   (`VaCuusViewStatus.h:130-147`, producer at `VaCuusRmlDocumentHost.cpp:507`).
4. **Dirty bits accumulate into the unpublished slot** (spec §6's own wording), so repeated writes
   before a consume coalesce — the same "latest wins, and say so in a log when it coalesced" rule as
   the load-result channel (`VaCuusView.cpp:386-391`).
5. **The game thread stamps a serial; the UI thread echoes it back** through a release store, and
   the game thread reads it acquire — the load-serial protocol verbatim
   (`VaCuusView.cpp:141-143`, `VaCuusRmlDocumentHost.cpp:226-232`, `VaCuusView.cpp:375-398`).
6. **Enqueue, do not `Trigger()`.** `UVaCuusSubsystem::Tick` is the single per-frame pulse
   (`VaCuusUIThread.h:176-179`, `VaCuusSubsystem.cpp:93`).
7. **Consume at `VaCuusUIThread.cpp:809`**, over every host, before any `Context::Update()` — and
   route through `Host->GetContext()` into `VaCuus/Private` code, following the input and snapshot
   precedents (`VaCuusDocumentHost.h:109-123`).
8. **Every changed field must reach `DirtyVariable` in the same frame**, or the idle gate turns a
   missed dirty flag into a permanently stale screen with no log line (§4.3).
9. **Ship the observables with the feature**: an applied-generation counter and a per-frame dirtied
   count on `FVaCuusViewStatus`, plus a new `FVaCuusPerfLog::EScope` for the game-thread diff and one
   for the UI-thread apply. "An invariant with no observable cannot be tested"
   (`VaCuusRecordingRenderInterface.h:163-172`).
10. **Do nothing on live reload.** The model and its values live on the `Rml::Context` and survive;
    RmlUi re-initialises the new document's views itself
    (`Source/Core/Context.cpp:304-305`, `Source/Core/DataView.cpp:76-88`). Tearing the model down
    would race the load and could leave the document unbound
    (`Source/Core/Element.cpp:2201-2216`).
