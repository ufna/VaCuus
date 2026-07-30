# VaCuus M2 — UI Thread + Input + UMG + Live Reload Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move all RmlUi/document work off the game thread onto a dedicated UI thread with
non-blocking queues and triple-buffered publishing, and make the UI interactive (mouse,
keyboard, wheel, cursor, gamepad spatial navigation, IME) plus editor live reload — while
absorbing the M1 tech debt.

**Architecture:** A `FVaCuusUIThread` (FRunnable) owns every `Rml::Context`, document and
(later) the JS runtime. The game thread writes into SPSC queues (input events, commands,
data snapshots) and triggers a coalescing wake event once per frame from a tickable
GameInstance subsystem. The UI thread publishes two `TTripleBuffer`s: the render command
buffer **straight to the render thread** (verified legal: `ENQUEUE_RENDER_COMMAND` has no
game-thread requirement) and an **interactive-region snapshot** back to the game thread,
which is what lets `SVaCuusWidget` answer Slate's synchronous Handled/Unhandled.

**Tech Stack:** UE 5.8.1 (`/w/Unreal/UnrealEngine`), vendored RmlUi `0ae381e`, existing M1
modules (`VaCuusRml`, `VaCuus`, `VaCuusRender`, `VaCuusEditor`).

**Beads:** implements `VaCuus-akj.6` (claimed) and its children:
`6.1` EOF quirk · `6.2` async LoadTexture · `6.3` content location · `6.4` Build.cs dep ·
`6.5` engine lifetime · `6.6` monolithic build · `6.7` M1HUD fallback · `6.8` idle
short-circuit. Close each in the task that lands it.

**API ground truth — READ THESE BEFORE CODING** (verified against 5.8.1 + vendored RmlUi in
the research phase; every signature below is quoted from them):
- `docs/research/m2-api-notes/threading.md` — FRunnable/FEvent/TTripleBuffer/TSpscQueue,
  ENQUEUE_RENDER_COMMAND off-game-thread, tickable subsystem.
- `docs/research/m2-api-notes/rmlui-input.md` — every `Context::Process*` signature, the
  `KeyIdentifier` enum, return-value semantics, the snapshot DFS algorithm, nav-*.
- `docs/research/m2-api-notes/slate-input.md` — SLeafWidget overrides, FReply capture,
  IInputProcessor, UWidget wrapping.
- `docs/research/m2-api-notes/ime.md` — ITextInputMethodContext contract, RmlUi
  TextInputHandler, the callback mapping table, **Linux has no IME system**.
- `docs/research/m2-api-notes/editor-livereload.md` — DirectoryWatcher, Linux inotify
  caveats, PIE subsystem lookup.
- `docs/research/m2-api-notes/debt.md` — async texture path that actually works on Vulkan,
  hashing rules, module-owned singleton, monolithic build fix.

**Environment:** work in `/w/Unreal/VcHost/Plugins/VaCuus` (git clone of the plugin inside
the host project — never a symlink), branch `m2-ui-thread`:

```bash
cd /w/Unreal/VcHost/Plugins/VaCuus && git fetch origin && git switch -c m2-ui-thread origin/master
```

Build: `cd /w/Unreal/UnrealEngine && ./Engine/Build/BatchFiles/Linux/Build.sh VcHostEditor Linux Development -project=/w/Unreal/VcHost/VcHost.uproject`
Tests: `/w/Unreal/UnrealEngine/Engine/Binaries/Linux/UnrealEditor-Cmd /w/Unreal/VcHost/VcHost.uproject -ExecCmds="Automation RunTests VaCuus; Quit" -unattended -nullrhi -nosplash`
Visual: `-game -RenderOffscreen -resx=1920 -resy=1080 -ForceRes -ExecCmds="vacuus.M1HUD.AutoShot 10, vacuus.M1HUD"` (`-ForceRes` is required or the window comes up 888×500).

---

## File structure (end state of M2)

```
Source/VaCuus/
├─ Public/VaCuusEngine.h              # module-owned lifetime (Task 1)
├─ Public/VaCuusSubsystem.h           # UVaCuusSubsystem (tickable GameInstance) (Task 4)
├─ Public/VaCuusView.h                # UVaCuusView handle: commands in, state out (Task 4)
├─ Private/VaCuusUIThread.h/.cpp      # FRunnable, frame loop, queues, publish (Tasks 2-3)
├─ Private/VaCuusUIQueues.h           # command/input queue payload types (Task 3)
├─ Private/VaCuusInteractiveSnapshot.h# snapshot type + DFS builder (Task 5)
├─ Private/VaCuusInputMap.h/.cpp      # FKey -> Rml::Input::KeyIdentifier, modifiers (Task 6)
├─ Private/VaCuusTextInput.h/.cpp     # IME context + RmlUi TextInputHandler (Task 9)
└─ Private/Tests/…                    # per-task automation tests
Source/VaCuusRender/
├─ Public/VaCuusReplayRenderer.h      # + async texture upload plumbing (Task 11)
├─ Private/SVaCuusWidget.h/.cpp       # renamed from SVaCuusHUDWidget, input-capable (Task 6)
├─ Private/VaCuusSlateElement.h/.cpp  # unchanged contract, consumes published buffers
└─ Private/VaCuusUMGWidget.h/.cpp     # UVaCuusWidget (UWidget) (Task 8)
Source/VaCuusEditor/
└─ Private/VaCuusLiveReload.h/.cpp    # DirectoryWatcher + debounce + PIE dispatch (Task 10)
```

---

### Task 0: Branch + monolithic (game target) build fix  · closes `VaCuus-akj.6.6`

**Files:** Modify `Source/VaCuusRml/VaCuusRml.Build.cs`.

- [ ] **Step 0.1: Branch** (commands above). Verify `git log --oneline -1` shows the merged
      M1 head and the working tree is clean.
- [ ] **Step 0.2: Reproduce the failure.** The Game target is monolithic and builds with
      exceptions disabled; `itlib/flat_map.hpp` (vendored inside RmlUi) throws:

```bash
cd /w/Unreal/UnrealEngine
./Engine/Build/BatchFiles/Linux/Build.sh VcHost Linux Development -project=/w/Unreal/VcHost/VcHost.uproject 2>&1 | tail -20
```

Expected: compile errors from `itlib/flat_map.hpp` about `throw` with exceptions disabled.
- [ ] **Step 0.3: Fix.** In `VaCuusRml.Build.cs`, next to the existing definitions add:

```csharp
// itlib (vendored inside RmlUi) throws in a few container paths; monolithic/non-editor
// targets compile with exceptions disabled and UBT only lets a module turn exceptions ON
// (bEnableExceptions |= ...), so the library's no-throw mode is the only option.
PublicDefinitions.Add("ITLIB_FLAT_MAP_NO_THROW=1");
```

- [ ] **Step 0.4: Verify both targets build:**

```bash
./Engine/Build/BatchFiles/Linux/Build.sh VcHost Linux Development -project=/w/Unreal/VcHost/VcHost.uproject
./Engine/Build/BatchFiles/Linux/Build.sh VcHostEditor Linux Development -project=/w/Unreal/VcHost/VcHost.uproject
```

Both must print `Result: Succeeded`.
- [ ] **Step 0.5: Commit:**

```bash
git commit -am "fix: build the monolithic game target (itlib no-throw)

Closes VaCuus-akj.6.6."
```

---

### Task 1: Module-owned engine lifetime  · closes `VaCuus-akj.6.5`

**Why:** `FVaCuusEngine::Get()` is a function-local static; it destructs during C++ static
destruction, i.e. **after** `FModuleManager::UnloadModulesAtShutdown` — in a modular editor
build its members can call into an already-unloaded `VaCuusRml`. Engine precedent for the
correct shape: `FImageWriteQueueModule` (module holds `TUniquePtr<T>`, creates in
`StartupModule`, destroys in `ShutdownModule`).

**Files:** Modify `Source/VaCuus/Public/VaCuusEngine.h`, `Private/VaCuusEngine.cpp`,
`Private/VaCuus.cpp` (module class); Test: existing suite must stay green.

- [ ] **Step 1.1: Move ownership into the module.** In `VaCuus.cpp`:

```cpp
class FVaCuusModule : public IModuleInterface
{
public:
    virtual void StartupModule() override { Engine = MakeUnique<FVaCuusEngine>(); }
    virtual void ShutdownModule() override { Engine.Reset(); }   // runs before static dtors
    FVaCuusEngine& GetEngine() const { check(Engine.IsValid()); return *Engine; }
private:
    TUniquePtr<FVaCuusEngine> Engine;
};
```

- [ ] **Step 1.2: Re-point the accessor.** `FVaCuusEngine::Get()` becomes:

```cpp
FVaCuusEngine& FVaCuusEngine::Get()
{
    // Module-owned: torn down in ShutdownModule, before static destruction and before
    // VaCuusRml unloads. Load-on-demand keeps early callers (tests) working.
    return FModuleManager::LoadModuleChecked<FVaCuusModule>("VaCuus").GetEngine();
}
```

Make `FVaCuusEngine`'s constructor/destructor public (module owns it) but keep the class
non-copyable. Note the module-unload order fact: `VaCuus::ShutdownModule` runs **before**
`VaCuusRender`'s (reverse load order, VaCuusRender is PostConfigInit) — teardown must not
assume the render module is already gone, and must not require it to still be there.
- [ ] **Step 1.3: Run the suite** (7 tests) — expect all green, no behavioural change.
- [ ] **Step 1.4: Commit:** `git commit -am "refactor: module-owned FVaCuusEngine lifetime (closes VaCuus-akj.6.5)"`

---

### Task 2: UI thread skeleton (TDD)

**Files:** Create `Source/VaCuus/Private/VaCuusUIThread.h/.cpp`,
`Private/Tests/VaCuusUIThreadTest.cpp`.

- [ ] **Step 2.1: Write the failing test** (`VaCuusUIThreadTest.cpp`):

```cpp
#include "Misc/AutomationTest.h"
#include "VaCuusUIThread.h"
#include "HAL/PlatformProcess.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusUIThreadLifecycleTest, "VaCuus.Threading.Lifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusUIThreadLifecycleTest::RunTest(const FString& Parameters)
{
    FVaCuusUIThread UIThread;
    TestTrue(TEXT("Started"), UIThread.Start());
    TestNotEqual(TEXT("UI thread id differs from game thread"),
        UIThread.GetThreadId(), FPlatformTLS::GetCurrentThreadId());

    // Frames only advance when triggered — the loop must not spin.
    const uint64 Before = UIThread.GetFrameCount();
    FPlatformProcess::Sleep(0.05f);
    TestEqual(TEXT("Idle thread does not advance frames"), UIThread.GetFrameCount(), Before);

    for (int32 i = 0; i < 5; ++i) { UIThread.Trigger(); }
    TestTrue(TEXT("Frames advanced after triggers"), UIThread.WaitForFrameCount(Before + 1, 2.0));

    UIThread.Stop();                       // idempotent, joins
    UIThread.Stop();
    TestFalse(TEXT("Stopped"), UIThread.IsRunning());
    return true;
}
#endif
```

- [ ] **Step 2.2: Build → expect failure** (`VaCuusUIThread.h` not found).
- [ ] **Step 2.3: Implement.** Header shape (no RmlUi types — they arrive in Task 3):

```cpp
class FVaCuusUIThread final : public FRunnable
{
public:
    FVaCuusUIThread();
    virtual ~FVaCuusUIThread() override;          // Stop() then joins

    bool Start();                                 // false if thread creation failed
    void Stop() override;                         // sets flag + wakes; safe to call twice
    void Trigger();                               // game thread: wake for one frame
    bool IsRunning() const;
    uint32 GetThreadId() const { return ThreadId.load(std::memory_order_relaxed); }
    uint64 GetFrameCount() const { return FrameCount.load(std::memory_order_relaxed); }
    bool WaitForFrameCount(uint64 Target, double TimeoutSeconds);   // test helper

    static bool IsInUIThread();                   // for check() wrappers (§4)

protected:
    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Exit() override;                 // NOTE: runs ON the UI thread (verified)
private:
    void RunFrame();                              // one UI frame; extended in Task 3

    FRunnableThread* Thread = nullptr;
    FEventRef WakeEvent{ EEventMode::AutoReset }; // auto-reset == coalescing latch
    std::atomic<bool> bStopRequested{ false };
    std::atomic<uint32> ThreadId{ 0 };
    std::atomic<uint64> FrameCount{ 0 };
};
```

Implementation rules (all verified — see `threading.md`):
- `Start()`: `Thread = FRunnableThread::Create(this, TEXT("VaCuusUI"), 0, TPri_BelowNormal);`
  **null-check the result** — `Create` returns nullptr with no log when
  `FPlatformProcess::SupportsMultithreading()` is false (commandlets, `-nothreading`);
  return false and let the caller fall back to inline execution (Task 4 handles that).
- `Run()`: `while (!bStopRequested.load(std::memory_order_acquire)) { WakeEvent->Wait(); if (bStopRequested…) break; RunFrame(); FrameCount.fetch_add(1, std::memory_order_release); }`
- `Stop()`: set the flag **and** `WakeEvent->Trigger()` — without the trigger the join
  deadlocks inside `pthread_join` forever.
- `Init()` records `ThreadId`; `Exit()` clears it. RmlUi teardown will live in `Exit()`
  (Task 3) because `Init/Run/Exit` all execute on the worker thread.
- Destruction order in `~FVaCuusUIThread`: `delete Thread` (which does Stop+join) **first**,
  then members die. Never destroy the event before the thread.
- `IsInUIThread()` compares `FPlatformTLS::GetCurrentThreadId()` against a
  file-static `std::atomic<uint32>` set in `Init()`.
- Use `std::atomic`, not `TAtomic`/`FThreadSafeBool` (both deprecated in 5.8).
- [ ] **Step 2.4: Run the test** → `VaCuus.Threading.Lifecycle` passes; whole suite (8) green.
- [ ] **Step 2.5: Commit:** `git commit -am "feat: dedicated UI thread skeleton (VaCuus.Threading.Lifecycle green)"`

---

### Task 3: Move RmlUi onto the UI thread + publish to the render thread

**Files:** Create `Source/VaCuus/Private/VaCuusUIQueues.h`; modify `VaCuusUIThread.h/.cpp`,
`Source/VaCuusRender/Private/VaCuusM1Harness.h/.cpp` (becomes a UI-thread-side document
host), `Private/VaCuusSlateElement.h/.cpp` (accept buffers published from the UI thread).

- [ ] **Step 3.1: Queue payloads** (`VaCuusUIQueues.h`):

```cpp
enum class EVaCuusCommandKind : uint8
{
    LoadDocumentFile, LoadDocumentMemory, CloseDocument, Resize, Shutdown
};

struct FVaCuusUICommand
{
    EVaCuusCommandKind Kind = EVaCuusCommandKind::Resize;
    FString Payload;              // path or RML source
    FIntPoint ViewSize = FIntPoint::ZeroValue;
};

// One queue per direction; SPSC (game thread writes, UI thread reads).
using FVaCuusCommandQueue = TSpscQueue<FVaCuusUICommand>;
```

Use `TSpscQueue` (`Containers/SpscQueue.h`) — `TQueue`/`TCircularQueue` both carry
"planned for deprecation in favor of TSpscQueue" warnings and `TCircularQueue` is POD-only.
- [ ] **Step 3.2: Own RmlUi on the UI thread.** Move the M1 harness's context/document
      ownership into a UI-thread-side object created in `FVaCuusUIThread::Init()` and
      destroyed in `Exit()`. `RunFrame()` becomes:

```cpp
void FVaCuusUIThread::RunFrame()
{
    check(IsInUIThread());
    DrainCommands();                       // LoadDocument/Close/Resize/…
    // (input drain: Task 6; data snapshots: M3)
    if (!Context) { return; }
    Recorder.BeginFrame(ViewSize);
    Context->Update();
    Context->Render();
    TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
    PublishToRenderThread(MoveTemp(Buffer));   // Step 3.3
    // (interactive snapshot publish: Task 5)
}
```

- [ ] **Step 3.3: Publish straight to the render thread.** Verified legal from a non-game
      thread (`FRenderThreadCommandPipe::Enqueue` has zero `IsInGameThread()` checks):

```cpp
void FVaCuusUIThread::PublishToRenderThread(TUniquePtr<FVaCuusCommandBuffer> Buffer)
{
    // The Slate element is refcounted (ESPMode::ThreadSafe) and outlives the enqueue.
    TSharedPtr<FVaCuusSlateElement, ESPMode::ThreadSafe> Element = SlateElement;
    if (!Element) { return; }
    ENQUEUE_RENDER_COMMAND(VaCuusPublishUIFrame)(
        [Element, Buf = MoveTemp(Buffer)](FRHICommandListImmediate& RHICmdList) mutable
        {
            Element->SetPendingBuffer_RenderThread(RHICmdList, MoveTemp(Buf));
        });
}
```

This removes the M1 game-thread hop entirely.
- [ ] **Step 3.4: Thread-affinity guards.** Add `check(FVaCuusUIThread::IsInUIThread())` at
      the top of every method that touches RmlUi (`FVaCuusRecordingRenderInterface`'s
      existing owner-thread guard already covers the recorder — extend it to the document
      host). Keep them in `check()` (dev builds) per spec §4.
- [ ] **Step 3.5: Verify** the existing 8 tests stay green (the recorder integration test
      still drives RmlUi directly on the test thread — that is fine, it owns its own
      recorder; if the new guards trip it, give the test its own affinity scope and say so).
- [ ] **Step 3.6: Commit:** `git commit -am "feat: RmlUi document work runs on the UI thread; buffers publish direct to render thread"`

---

### Task 4: Tickable subsystem + view handle + resize

**Files:** Create `Source/VaCuus/Public/VaCuusSubsystem.h` + `Private/VaCuusSubsystem.cpp`,
`Public/VaCuusView.h` + `Private/VaCuusView.cpp`; modify
`Source/VaCuusRender/Private/VaCuusRender.cpp` (console command uses the subsystem).

- [ ] **Step 4.1: Subsystem.** `UVaCuusSubsystem : public UGameInstanceSubsystem, public FTickableGameObject`
      (copy the `UTickableWorldSubsystem` shape: `Tick`, `GetStatId`, `IsTickable`,
      `GetTickableTickType` returning `ETickableTickType::Always` only when initialized).
      `Initialize()` starts `FVaCuusUIThread` (falls back to inline `RunFrame()` on tick when
      `Start()` returned false); `Deinitialize()` stops it. `Tick()` does exactly:

```cpp
void UVaCuusSubsystem::Tick(float DeltaTime)
{
    if (UIThread.IsValid()) { UIThread->Trigger(); }   // coalescing; never blocks
    else if (InlineFallback.IsValid()) { InlineFallback->RunFrameInline(); }
}
```

Why not `FTSTicker`: it fires very late in `FEngineLoop::Tick` (after Slate has drawn).
- [ ] **Step 4.2: View handle.** `UVaCuusView` (UObject) — a proxy that only enqueues
      commands: `LoadDocument(Path)`, `LoadDocumentFromMemory(Rml)`, `Close()`,
      `Resize(FIntPoint)`, `SetVisible(bool)`. It owns nothing thread-affine.
- [ ] **Step 4.3: Resize.** `SVaCuusWidget::Tick` computes the window rect (existing
      `ComputeWindowRect` helper) and, when it changed, enqueues a `Resize` command instead
      of touching RmlUi. The UI thread applies `Context->SetDimensions(...)` in `DrainCommands`.
- [ ] **Step 4.4: Console command** `vacuus.M1HUD` now goes through the subsystem
      (`GetGameInstance()->GetSubsystem<UVaCuusSubsystem>()`); keep the file-or-inline
      document behaviour, and **add the missing fallback**: if loading the VFS path fails
      (not merely missing), fall back to the inline document and log it — closes
      `VaCuus-akj.6.7`.
- [ ] **Step 4.5: Verify** — PIE + `vacuus.M1HUD` renders as in M1; resize the window and
      confirm the HUD re-lays out; suite green. Headless: AutoShot screenshot at 1920×1080
      must match the M1 baseline in layout.
- [ ] **Step 4.6: Commit:** `git commit -am "feat: UVaCuusSubsystem drives the UI thread; view handle + resize (closes VaCuus-akj.6.7)"`

---

### Task 5: Interactive-region snapshot (TDD)

**Files:** Create `Source/VaCuus/Private/VaCuusInteractiveSnapshot.h` (+ builder in
`.cpp`), `Private/Tests/VaCuusSnapshotTest.cpp`; modify `VaCuusUIThread.cpp` to publish it.

**Contract (spec §4/§8):** the UI thread publishes a value-type snapshot; the game thread
answers Slate synchronously from it. **RmlUi has no API that enumerates interactive
elements** — build it with one DFS per published frame.

- [ ] **Step 5.1: Type:**

```cpp
struct FVaCuusInteractiveSnapshot
{
    uint64 Generation = 0;
    FIntPoint ViewSize = FIntPoint::ZeroValue;
    TArray<FIntRect> InteractiveRects;   // clipped, window pixels, painter order irrelevant
    EMouseCursor::Type Cursor = EMouseCursor::Default;   // latched from SetMouseCursor
    bool bWantsKeyboardFocus = false;    // a document holds focus / a text field is active
    bool Contains(FIntPoint P) const;    // linear scan; tens of rects in practice
};
```

- [ ] **Step 5.2: Write the failing test** — build a document from memory with a known
      layout (a 100×40 button at (20,20), a `pointer-events:none` overlay, and a
      `vacuus-passthrough` region), run one UI frame, and assert:
      the button rect is present; a point inside it is `Contains`; a point in the
      pass-through region is not; a rect nested inside an `overflow:hidden` container is
      clipped to the container. Test path `VaCuus.Input.Snapshot`.
- [ ] **Step 5.3: Implement the DFS** (UI thread, after `Context::Update()`, before publish).
      Rules verified in `rmlui-input.md` — all of them matter:
  - Iterate documents via `Context::GetNumDocuments()/GetDocument(i)`.
  - Recurse with `GetNumChildren(/*include_non_dom_elements=*/true)` — scrollbars and form
    internals are non-DOM children stored last and are genuinely interactive.
  - Skip invisible subtrees (`Element::IsVisible()`), mirroring `AddToStackingContext`.
  - Accumulate the clip rect **top-down in our own DFS**; never call
    `ElementUtilities::GetClippingRegion` per element (it walks all ancestors → O(N·depth)).
  - Rect = `GetAbsoluteOffset(BoxArea::Border)` + `GetBox().GetSize(BoxArea::Border)`,
    intersected with the accumulated clip. If `element->GetTransformState() != nullptr`,
    use `ElementUtilities::GetBoundingBox(out, element, BoxArea::Border)` instead
    (`GetAbsoluteOffset` is untransformed) — gate on the transform state, it is pricier.
  - `pointer-events: none` is checked **per element only, never as a subtree prune** (RmlUi
    itself hit-tests descendants first).
  - Interactive = element has a listener-bearing role: emit a rect when the element matches
    the interactive predicate — `tab-index: auto` computed, or an inline event attribute /
    registered listener is not queryable, so use: computed `pointer-events != none` **and**
    (`tab-index` auto **or** the element has an `onclick`-style attribute **or** it is a
    known interactive tag: `button`, `input`, `select`, `textarea`, `a`). Document this
    heuristic in the header — it is the M2 contract; a future data-attribute opt-in
    (`vacuus-interactive`) refines it.
  - Pass-through opt-out: a plain attribute `vacuus-passthrough` on an element (or any
    ancestor) prunes it and its subtree from the snapshot. Use a plain attribute, **not**
    `data-vacuus-passthrough` — `data-*` names are parsed by RmlUi's data-view machinery.
- [ ] **Step 5.4: Publish** via a second `TTripleBuffer<FVaCuusInteractiveSnapshot>`
      (UI → game). Construct with `Write`/`GetWriteBuffer()+SwapWriteBuffers()`; the game
      thread does `SwapReadBuffers()` + `Read()`. Remember: `Read()` returns a reference
      invalidated by the next swap — copy what you keep; `SwapReadBuffers()` early-returns
      when not dirty (that is the desired "keep last" behaviour) so carry `Generation`.
- [ ] **Step 5.5: Run** `VaCuus.Input.Snapshot` → green; full suite green.
- [ ] **Step 5.6: Commit:** `git commit -am "feat: interactive-region snapshot published UI->game (VaCuus.Input.Snapshot green)"`

---

### Task 6: Input routing (mouse, wheel, keyboard, cursor)

**Files:** Create `Source/VaCuus/Private/VaCuusInputMap.h/.cpp`,
`Private/Tests/VaCuusInputMapTest.cpp`; rename `SVaCuusHUDWidget` →
`SVaCuusWidget` (`git mv`) and implement input; extend `FVaCuusSystemInterface` with
`SetMouseCursor`; extend the UI thread with an input queue drain.

- [ ] **Step 6.1: Key/modifier map (TDD).** Test `VaCuus.Input.KeyMap`: a table of
      representative `FKey`s (`EKeys::A`, `Zero`, `NumPadFive`, `Escape`, `Left`, `F5`,
      `SpaceBar`, `Enter`, `BackSpace`, `Tab`, `LeftShift`) maps to the expected
      `Rml::Input::KeyIdentifier`; unknown keys map to `KI_UNKNOWN`; modifier flags compose
      (`KM_CTRL|KM_SHIFT`). Implement as a lazily-built `TMap<FKey, Rml::Input::KeyIdentifier>`.
- [ ] **Step 6.2: Input events queue.** `FVaCuusInputEvent` (POD-ish: type, position,
      button, wheel delta, key id, modifiers, character) in `VaCuusUIQueues.h`, drained at
      the top of `RunFrame()` and dispatched to `Context::Process*`. Semantics that must be
      respected (all verified):
  - `ProcessMouseWheel(Vector2f, mods)` — the float overload is deprecated; positive Y is
    **down**, UE's `GetWheelDelta()` is positive **up** → pass `Vector2f(0, -Delta)`, and
    1.0 unit == 80 dp, so scale, do not pass pixels.
  - `ProcessTextInput(char)` silently drops bytes > 127 — use the `Rml::Character`
    (UTF-32) or `const String&` (UTF-8) overload; recombine UTF-16 surrogate pairs from
    consecutive `OnKeyChar` calls before dispatch.
  - After `ProcessKeyDown(KI_RETURN)` also send `ProcessTextInput('\n')` for multiline
    fields (RmlUi does not synthesize it; the SDL backend does exactly this).
  - `OnMouseLeave` must send `ProcessMouseLeave()`, or `:hover` sticks forever.
  - Return values are **not** uniform: key/text/wheel return "NOT consumed"; mouse
    move/button/leave return `!IsMouseInteracting()` (a state hint). Never collapse them
    into one "handled" concept — the game thread decides Handled from the **snapshot**.
- [ ] **Step 6.3: Slate side.** `SVaCuusWidget` overrides `OnMouseMove`,
      `OnMouseButtonDown/Up`, `OnMouseWheel`, `OnKeyDown/Up`, `OnKeyChar`, `OnMouseEnter/Leave`,
      `OnCursorQuery`, `OnFocusReceived/Lost`. Each: convert to widget-local pixels via the
      cached geometry, enqueue the event, and answer Slate **synchronously from the
      snapshot**:

```cpp
FReply SVaCuusWidget::OnMouseButtonDown(const FGeometry& G, const FPointerEvent& E)
{
    const FIntPoint P = ToViewPixels(G, E.GetScreenSpacePosition());
    Enqueue(FVaCuusInputEvent::MouseDown(P, E));
    if (!Snapshot.Contains(P)) { return FReply::Unhandled(); }      // pass-through
    return FReply::Handled().CaptureMouse(SharedThis(this));        // drag support
}
```

Release capture on button-up when no buttons remain pressed. `OnCursorQuery` returns
`Snapshot.Cursor`. Keyboard focus: `OnMouseButtonDown` on an interactive rect also does
`.SetUserFocus(SharedThis(this))` when `Snapshot.bWantsKeyboardFocus`.
- [ ] **Step 6.4: Cursor source.** RmlUi pushes cursor changes through
      `SystemInterface::SetMouseCursor` (only on change). Add the override to
      `FVaCuusSystemInterface`, map RmlUi cursor names (`arrow`, `move`, `pointer`, `resize`,
      `cross`, `text`, `unavailable`) to `EMouseCursor::Type`, latch into the next snapshot.
- [ ] **Step 6.5: Verify interactively.** Extend the M1 HUD document with a `button`
      that changes class on `:hover`/`:active` (RCSS only — no JS yet). PIE: hovering
      highlights it, clicking activates it, clicks outside interactive rects still reach the
      game (verify by observing the game's own input, e.g. camera still moves).
      Headless proof: an automation test `VaCuus.Input.Routing` that pushes synthetic events
      through the queue and asserts the document's hover element changed
      (`Context::GetHoverElement()` on the UI thread via a test hook).
- [ ] **Step 6.6: Commit:** `git commit -am "feat: mouse/keyboard/wheel/cursor routing through the interactive snapshot"`

---

### Task 7: Gamepad + spatial navigation

**Files:** Modify `VaCuusInputMap.h/.cpp`, `SVaCuusWidget`, the demo document/RCSS.

- [ ] **Step 7.1: RCSS.** RmlUi's `nav-*` properties exist at this SHA but are **not
      inherited** and are read via `GetLocalProperty` — every focusable element needs its own
      rule. Add to the demo sheet: `button, input, .slot { nav: auto; tab-index: auto; }`.
      String targets need a leading `#` (`nav-right: "#slot2"`), otherwise it is a silent no-op.
- [ ] **Step 7.2: Focus at load.** `Doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document)`
      — without focus inside a document, `ElementDocument::ProcessDefaultAction` (which owns
      Tab/arrows/Enter) never runs and all navigation is dead.
- [ ] **Step 7.3: Synthesize gamepad keys.** RmlUi has **zero** gamepad support. In
      `SVaCuusWidget::OnAnalogValueChanged` + `OnKeyDown` (gamepad keys arrive as `FKey`s):
      map DPad/left-stick to `KI_UP/DOWN/LEFT/RIGHT`, FaceButton_Bottom to `KI_RETURN`
      (RmlUi's handler calls `focus_node->Click()`), FaceButton_Right to a `Back` command.
      Analog sticks need embedder-side dead zone (0.5) and repeat throttling (initial delay
      0.4 s, repeat 0.12 s) — RmlUi does no key repeat.
      Shift-Tab requires `KM_SHIFT` in the modifier mask or reverse tabbing silently breaks.
- [ ] **Step 7.4: Verify** in PIE with a pad if available; otherwise drive the same code path
      from an automation test (`VaCuus.Input.SpatialNav`): synthesize KI_RIGHT and assert
      `Context::GetFocusElement()` moved to the expected id.
- [ ] **Step 7.5: Commit:** `git commit -am "feat: gamepad-driven spatial navigation (nav-* + synthesized keys)"`

---

### Task 8: UMG widget wrapper

**Files:** Create `Source/VaCuusRender/Private/VaCuusUMGWidget.h/.cpp` (class
`UVaCuusWidget : public UWidget`).

- [ ] **Step 8.1: Implement** the standard UWidget shape: `RebuildWidget()` creates and
      returns `SNew(SVaCuusWidget)`, `SynchronizeProperties()` pushes exposed UPROPERTYs
      (`DocumentPath`, `bAutoLoad`, `ZOrder`), `ReleaseSlateResources(bool)` resets the
      shared pointer. Expose `LoadDocument`/`Close` as `UFUNCTION(BlueprintCallable)` that
      forward to the view handle.
- [ ] **Step 8.2: Verify** by adding the widget to a UMG blueprint in the host project
      (or, headless-friendly: an automation test that constructs the UWidget, calls
      `TakeWidget()`, and asserts a valid Slate widget plus a clean `ReleaseSlateResources`).
- [ ] **Step 8.3: Commit:** `git commit -am "feat: UVaCuusWidget (UMG wrapper)"`

---

### Task 9: IME  · Windows-validated, Linux-degraded

**Files:** Create `Source/VaCuus/Private/VaCuusTextInput.h/.cpp`; modify
`FVaCuusSystemInterface` (`ActivateKeyboard`/`DeactivateKeyboard`), UI thread (install
`Rml::TextInputHandler`), `SVaCuusWidget` (register/activate the context on focus).

**Hard constraint:** `FLinuxApplication` does **not** implement `GetTextInputMethodSystem()`
— it returns null on this dev machine, and Epic's own CEF IME handler is compiled out on
Linux. So: build the full path, gate it on a null check, and make the Linux fallback
(`OnKeyChar` → `ProcessTextInput`) the tested behaviour here; note Windows validation as an
M6 matrix item.

- [ ] **Step 9.1: Shadow state.** `ITextInputMethodContext`'s 14 pure virtuals are
      **synchronous pulls on the game thread** and must never block on the UI thread.
      Publish a `FVaCuusTextFieldState` (text value, selection range, composition range,
      caret rect, read-only flag, generation) inside the interactive snapshot; answer
      `GetTextLength/GetTextInRange/GetSelectionRange/GetTextBounds/GetScreenBounds/IsReadOnly`
      from it. Only mutations (`SetTextInRange`, `SetSelectionRange`,
      `BeginComposition/UpdateCompositionRange/EndComposition`) are queued to the UI thread,
      each stamped with the field generation so stale ones are dropped (RmlUi silently
      ignores `SetSelectionRange` when the element lost focus).
- [ ] **Step 9.2: RmlUi side.** Implement `Rml::TextInputHandler` (`OnActivate`,
      `OnDeactivate`, `OnDestroy`) and install with `Rml::SetTextInputHandler(...)` before
      context creation; cache the active `Rml::TextInputContext*` (raw, non-owning —
      null it in `OnDestroy`, mirroring `FSlateEditableTextLayout::FTextInputMethodContext::KillContext`).
      Caret rect comes from `SystemInterface::ActivateKeyboard(caret_position, line_height)`,
      not from `TextInputContext::GetBoundingBox` (that is the whole element border box).
- [ ] **Step 9.3: Index spaces.** Three of them: RmlUi character offsets, RmlUi byte offsets
      (`GetCompositionRange` returns **bytes**), engine UTF-16 TCHAR. Convert at every
      boundary with `Rml::StringUtilities::ConvertCharacterOffsetToByteOffset` /
      `ConvertByteOffsetToCharacterOffset` / `LengthUTF8`. Never notify
      `NotifyTextChanged/NotifySelectionChanged` while `IsComposing()` is true.
- [ ] **Step 9.4: Coordinates.** `GetTextBounds/GetScreenBounds` are Slate **absolute**
      pixels; RmlUi values are context pixels — transform through the cached widget geometry
      (`LocalToAbsolute`), and wrap `FGeometry::AbsolutePosition` (FVector2f) explicitly into
      `FVector2D`. World-space surfaces have no valid mapping: **disable IME for
      `UVaCuusWorldComponent`** and say so in the header.
- [ ] **Step 9.5: Verify on Linux** — typing ASCII into an `<input>` in the demo document
      works through the `OnKeyChar` path with the IME system absent (automation test
      `VaCuus.Input.TextEntry` asserting the element's value changed). Log once at startup
      when `GetTextInputMethodSystem()` is null so the degradation is visible.
- [ ] **Step 9.6: Commit:** `git commit -am "feat: IME context (shadow-state pulls, queued mutations); Linux degrades to OnKeyChar"`

---

### Task 10: Editor live reload  · closes `VaCuus-akj.6.3`

**Files:** Create `Source/VaCuusEditor/Private/VaCuusLiveReload.h/.cpp`; modify
`VaCuusEditor.Build.cs` (add `DirectoryWatcher`), the editor module class.

**Facts that shape the design** (see `editor-livereload.md`): the watcher is ticked **only**
by `UEditorEngine::Tick` — it is editor-only by construction; on Linux
`RegisterDirectoryChangedCallback_Handle` returns a valid handle even for a **non-existent
directory** (silent no-op); `FFileChangeData::Filename` is **relative**; one save produces a
**burst** of events and the engine provides no debounce; `IN_Q_OVERFLOW` is dropped silently.

- [x] **Step 10.1: Register** on the UI content dir after an explicit
      `IFileManager::Get().DirectoryExists()` check (the return value is not a health check
      on Linux). Registration and callbacks are game-thread only (`checkf(IsInGameThread())`
      inside the Linux backend) — do not add an `AsyncTask` hop.
- [x] **Step 10.2: Debounce.** Collect changed paths into a `TSet<FString>` (normalise every
      incoming filename with `FPaths::ConvertRelativePathToFull`), skip editor temp files
      (`~`, `.tmp`, dotfiles), and flush on an `FTSTicker` after ~150 ms of quiet.
- [x] **Step 10.3: Dispatch to PIE.** Find the PIE game instance via
      `GEngine->GetWorldContexts()` filtering `EWorldType::PIE`, get
      `UVaCuusSubsystem` from its game instance, and enqueue a reload command (the UI thread
      closes + reloads the document; no RmlUi call from the editor thread).
- [x] **Step 10.4: Escape hatch.** Console command `vacuus.ReloadUI` doing the same
      unconditionally — the watcher is not lossless (queue overflow is dropped).
- [x] **Step 10.5: Content location** (`VaCuus-akj.6.3`): decide and implement — mount the
      **plugin's** `Content/DevUI` into the VFS roots alongside the project's, and delete the
      duplicated copy from whichever location loses. State the decision in the commit body.
- [x] **Step 10.6: Verify** — PIE running with the HUD; edit `m1_hud.rcss` (change a colour);
      within ~200 ms the running UI updates without leaving PIE. Screenshot before/after.
- [x] **Step 10.7: Commit:** `git commit -am "feat: editor live reload for UI documents (closes VaCuus-akj.6.3)"`

**10.6 verified: 191 ms**, live PIE session, never exited; one editor save produced 4 inotify
events collapsed to 1 path. Evidence (screenshots + log excerpt + what the automated coverage
actually is) preserved in `docs/research/proofs/m2-t10-live-reload/`, because the original
artifacts lived in untracked `Saved/` and were overwritten by Task 11's automation runs within the
hour. `9e9ca23`'s body records the harness but not the result — the spec review flagged that as
the step's only real gap, and this is the fix.

---

### Task 11: Async texture load  · closes `VaCuus-akj.6.2`

**Files:** Modify `Source/VaCuusRender/Private/VaCuusRecordingRenderInterface.cpp`,
`Public/VaCuusCommandBuffer.h`, `Private/VaCuusReplayRenderer.cpp`.

**Do NOT use `RHIAsyncCreateTexture2D`** — it is not deprecated but Vulkan and Metal both
`Fatal` on it and `GRHISupportsAsyncTextureCreation` is hard-false on Vulkan. The working
5.8 shape is: worker thread → `FRHICommandListBase::CreateTextureInitializer(desc)` →
`GetTexture2DSubresource(mip).Data/Stride` → `Finalize()` on a **non-immediate**
`FRHICommandList` → `FRHICommandListImmediate::Get().QueueAsyncCommandListSubmit(...)`.

**AMENDMENT (controller, at dispatch time) — arrival is in band, not out of band.** Step 11.2
as written routes the finished payload worker → private RHI command list → render thread →
straight into `FVaCuusReplayRenderer::Textures`. Overridden: the decode goes async, the
**arrival travels through the existing command-buffer channel**. `LoadTexture` mints the
handle, puts a 1×1 transparent payload in the pending buffer's `NewTextures` (the Step 11.3
placeholder) and kicks the decode; the UI thread drains completions at the top of its next
frame and adds the real payload under the same handle. `TMap::Add` on an existing key replaces
the value, so the replayer needs no change — that *is* the swap Step 11.3 describes.

Three reasons: **(1)** the out-of-band path breaks Task 12 — a texture arriving while the idle
short-circuit is publishing nothing would be installed but never re-replayed, leaving the
placeholder on screen until an unrelated change dirtied the frame; an in-band arrival is a
non-empty `NewTextures`, which is the signal that gate **will have to** read.

⚠️ **Wording correction (mine).** The original phrasing above said "the signal that gate already
reads", present tense. That was about Task 12's *design*, but it propagated into a Task 11 code
comment as an assertion that an idle short-circuit exists in the plugin **today** — it does not,
and the Task 11 quality review caught it (`grep -rn "idle"` in `VaCuusRender` matched only that
comment). Today every UI frame is published unconditionally and the newest buffer is always fully
replayed, so a late payload forces a re-replay for a completely different reason. Cite the reason
that exists; note the future requirement as a future requirement. This is the third comment on this
milestone whose justification named something that wasn't there, and the first one I authored. **(2)** `FVaCuusCommandBuffer`
documents itself as the single channel for all resource traffic; a second private channel means
two orderings to reason about. **(3)** the replayer is a by-value member of the Slate element,
so reaching it from a worker task needs a weak pointer across two modules plus
teardown-vs-in-flight-upload correctness.

Cost of the choice: the `UpdateTexture2D` upload still happens on the render thread — only the
decode is async. The decode is the expensive part and the content of `6.2`. Async **upload** is
deferred to `VaCuus-akj.6.25` (P3, measurement-gated). Task 11 also folds in `VaCuus-akj.6.12`:
the `IImageWrapperModule` lookup must be cached from the game thread, since it currently runs
on the UI thread where the module manager refuses callers.

- [x] **Step 11.1: Synchronous dimension probe.** `LoadTexture` must return dimensions
      immediately: `IImageWrapperModule::DetectImageFormat` → `CreateImageWrapper` →
      `SetCompressed` → `GetWidth()/GetHeight()`. Two documented traps: `SetCompressed`
      memcpys the entire file (O(filesize), not O(header)), and **1/2/4-bit PNGs fully
      decode inside `SetCompressed`** — do the probe on the UI thread (not the game thread)
      and `Reset()` the wrapper after probing (libjpeg-turbo retains a decompressor).
- [x] **Step 11.2: Async decode + upload.** Kick `UE::Tasks::Launch` for the decode; on
      completion, record the create-with-data on a worker command list and hand it to
      `QueueAsyncCommandListSubmit` from the render thread. `FinishRecording()` before
      hand-off; guard the cancel path (a dropped command list is a silent leak —
      `FStaticMeshStreamIn` asserts on exactly this).
- [x] **Step 11.3: Placeholder swap without `FRHITextureReference`.** The replayer binds
      `FRHITexture*` per draw from `TMap<Handle, FTextureRHIRef>` — **swapping the map entry
      is the swap**. Until the upload lands, the handle maps to a 1×1 transparent texture so
      draws are invisible rather than missing. (`FRHITextureReference` is on Epic's removal
      list, costs an `RHIThreadFence` per swap and cannot back an SRV.)
- [x] **Step 11.4: Test.** Extend `VaCuus.Render.Recorder.LoadTexture`: dimensions are
      correct **immediately**, the RGBA payload arrives on a later frame, and a draw
      referencing the not-yet-ready handle renders transparent instead of asserting.
- [x] **Step 11.5: Commit:** `git commit -am "feat: async texture decode/upload (closes VaCuus-akj.6.2)"`

---

### Task 12: Idle short-circuit  · closes `VaCuus-akj.6.8`

**Files:** Modify `VaCuusUIThread.cpp` (publish gate), `VaCuusCommandBuffer.h` (hash helper).

⚠️ **This `Files:` line was wrong, and three statements below it were too** — corrected here after
implementation rather than quietly, because the plan is the record. (a) The publish is **not** in
`VaCuusUIThread.cpp`; that file only loops hosts. The publish is
`FVaCuusRmlDocumentHost::RecordAndPublishFrame`, and the gate can live in **neither**, because only
the recorder owns `Pending` and `Generation` — and the no-generation-on-skip rule below forbids
touching `Generation` outside it. `VaCuusUIThread.cpp` ended up unchanged. (b) "Replay drops to ~0
on idle frames" is not what happens: `Draw_RenderThread` never calls `Replay()` without a buffer, so
the timing scope yields **no samples at all** — an average of 0.000 over count 0, not a small cost.
The honest observable is the sample count collapsing, which is why a `published=/skipped=` line was
added. (c) "Still advance the frame counter (so `stat vacuus` shows idle frames)" maps onto nothing:
`stat vacuus` has no frame counter, and `FVaCuusUIThread::FrameCount` already advances
unconditionally. (d) The plan also failed to notice `FVaCuusViewStatus::FramesPublished`. Leaving it
publish-only would have **broken `vacuus.M1HUD.AutoShot`** — the static HUD publishes exactly once,
so any threshold ≥2 would never fire and this milestone's own screenshot harness would hang. Renamed
to `FramesRecorded`.

RmlUi exposes **no** dirty signal (`Update`/`Render` return unconditional `true`;
`IsLayoutDirty` is protected/private; `RenderManager` has no version counter;
`GetNextUpdateDelay` is a *timer* hint, not a change flag). So: record as usual, then gate
publication on a content hash.

**HARD CONSTRAINT from Task 11 — gate the publish, never the record.** Task 11 drains completed
async texture decodes at the end of `BeginFrame()`, and that drain is what turns a finished decode
into a `NewTextures` entry. If the idle short-circuit skips `RecordAndPublishFrame()` (or
`BeginFrame`) instead of skipping only the *publish*, the drain never runs and a loaded image stays
a transparent 1×1 placeholder **forever** — precisely the failure the in-band transport was chosen
to avoid, just moved one level up. Record every frame; decide only whether to publish. Because the
drain runs before `Context::Update()`, an arrival is already sitting in `NewTextures` by the time
the gate evaluates the resource deltas, so the "arrays are all empty" test correctly forces a
publish on the frame a texture lands.

- [x] **Step 12.1: Hash field-by-field.** `FXxHash64Builder` over each `FVaCuusCommand`'s
      members — **never** a raw `HashBuffer` over the array: `EVaCuusCommandType` (uint8) at
      offset 0 is followed by 7 bytes of uninitialised padding, producing nondeterministic
      hashes and spurious dirty frames.
- [x] **Step 12.2: Gate.** Skip publish **only if** the hash is unchanged **and**
      `NewGeometry/NewTextures/ReleasedGeometry/ReleasedTextures` are all empty — otherwise
      the replayer never sees the resource traffic. When skipping, still advance the frame
      counter (so `stat vacuus` shows idle frames) and leave the render thread reusing its RT
      (the spec §5 idle model: composite-only, measured 0.004 ms).
- [x] **Step 12.3: Measure.** With the static M1 HUD: Record cost stays (~0.06 ms), Replay
      drops to ~0 on idle frames. Record the numbers; update spec §11's idle row if the
      measured composite-only cost changed.
- [x] **Step 12.4: Commit:** `git commit -am "perf: skip publish when the recorded frame is unchanged (closes VaCuus-akj.6.8)"`

---

### Task 13: Small debt sweep  · closes `VaCuus-akj.6.1`, `6.4`

**Files:** `Source/VaCuus/Private/VaCuusFileInterface.cpp`, `Source/VaCuusRender/VaCuusRender.Build.cs`.

- [ ] **Step 13.1: EOF quirk (`6.1`).** `FFileHandleUnix::Seek` clamps a read-mode seek to
      `FileSize-1`, so seeking to exact EOF under-reports `Tell()`. Track the logical
      position in our own member (clamped to `[0, Size]`) and answer `Tell()` from it rather
      than from the handle. Extend `VaCuus.Core.FileInterface` with an exact-EOF case:
      `Seek(0, SEEK_END)` then `Tell() == Size`, and a subsequent `Read` returns 0.
- [ ] **Step 13.2: Build.cs dep (`6.4`).** No public header of `VaCuusRender` includes RmlUi
      any more (the recorder header moved to Private in M1's wrap-up) — move `VaCuusRml` from
      `PublicDependencyModuleNames` to `PrivateDependencyModuleNames` and delete the stale
      comment. Rebuild to confirm.
- [ ] **Step 13.3: Task 10 re-review follow-ups (`VaCuus-akj.6.29`).** Added mid-flight; the
      re-review of Task 10's fixes approved with five minor items, and this is the right task to
      absorb them. The one that matters is a **contract gap, not a bug**: after the C1 fix the RmlUi
      cache clear lives in the **editor-only** `FVaCuusLiveReload::ReloadAllLiveViews`, while
      `UVaCuusSubsystem::ReloadAllDocuments()` is `VACUUS_API` and its doc still calls itself "the one
      door". An M3 runtime reload hook that trusts that sentence reintroduces C1 verbatim — RML edits
      apply, RCSS edits silently do not — and no existing test catches it, because the C1 test
      exercises the editor dispatcher. One sentence on the declaration closes it. The other four:
      `LiveReload.AssetCaches` should poll the clear counter rather than the frame counter (the
      coalescing wake latch makes the frame wait racy); the `WatcherEvent` probe file needs a
      `.gitignore` line (it must be written inside a watched root, which is git-tracked);
      `DebouncePollSeconds` is still public under a claim no test honours (I5 left one constant
      behind); and `FlushNow()` now has no production caller. Nits listed on the bead.
- [ ] **Step 13.4: Commit:** `git commit -am "fix: exact-EOF Tell(); private VaCuusRml dep; task 10 review follow-ups (closes VaCuus-akj.6.1, VaCuus-akj.6.4, VaCuus-akj.6.29)"`

---

### Task 14: Acceptance — interaction demo, measurements, wrap-up

**Files:** `Content/DevUI/m2_demo.rml|.rcss` (interactive document), spec §11 row updates.

- [ ] **Step 14.1: Demo document** — buttons with `:hover`/`:active` styling, a scrollable
      list (wheel), a text `<input>`, a pass-through region marked `vacuus-passthrough`, and
      `nav`-annotated focusables for pad navigation. Console command `vacuus.M2Demo`.
- [ ] **Step 14.2: Acceptance run (spec §14 M2)** — in PIE:
  1. Mouse hover/click/drag/wheel work; clicks outside interactive rects reach the game.
  2. Keyboard types into the input; Tab moves focus; Shift-Tab moves back.
  3. Gamepad (or synthesized) spatial nav moves focus and activates.
  4. Live reload: edit the RCSS, UI updates without leaving PIE.
  5. `stat vacuus` shows **zero** game-thread RmlUi cost (Update/Record now on the UI thread)
     and no hitches on document load.
- [ ] **Step 14.3: Measure** at 1920×1080 for 60 s with `vacuus.M1HUD.PerfLog 1`: game-thread
      cost (input + snapshot read) must be ≤0.10 ms (spec §11 gate); UI-thread frame cost;
      render replay unchanged. Capture an Insights trace and confirm **no game-thread wait on
      any VaCuus lock** and no `FlushRenderingCommands`.
- [ ] **Step 14.4: Update spec §11** rows with measured M2 numbers (game-thread row, idle row).
- [ ] **Step 14.5: Commit + merge:**

```bash
git commit -am "feat: M2 interaction demo + measured acceptance"
git push origin m2-ui-thread
cd /w/Unreal/VaCuus && git merge --ff-only m2-ui-thread
bd close VaCuus-akj.6 --reason="M2 accepted: <numbers>" --suggest-next
```

- [ ] **Step 14.6: File follow-ups** for anything deferred (Windows IME validation, world-space
      input, JS-side input hooks) as beads under M3.

---

## Acceptance (spec §14 M2, restated)

1. Interaction demo driven by mouse + keyboard + **gamepad spatial navigation**.
2. IME composition verified (Linux: degraded `OnKeyChar` path tested; Windows deferred to
   the M6 matrix with a bead).
3. Thread-affinity checks green (no RmlUi call from a non-UI thread in any test).
4. No game-thread hitches on document load; game-thread cost within the §11 gate.

## Plan self-review notes (done at write time)

- **Spec coverage:** §4 threading (Tasks 2–5), §8 input/IME/gamepad (6, 7, 9), §3 module map
  (1, 8), §9 live reload (10), §11 budgets (12, 14). Data binding (§6) and JS (§7) are M3/M4
  — deliberately absent.
- **Debt coverage:** 6.1 (T13), 6.2 (T11), 6.3 (T10), 6.4 (T13), 6.5 (T1), 6.6 (T0),
  6.7 (T4), 6.8 (T12) — all eight scheduled. Added mid-flight: 6.12 (T11 — same code path).
- **Ordering rationale:** the UI thread lands before input because the snapshot (the input
  contract) can only be produced by a running UI frame; debt items sit where they are
  cheapest (monolithic first — it changes build config; async texture after the thread exists).
- **Known risk:** Task 5's "interactive element" predicate is a heuristic (RmlUi cannot
  enumerate listeners). If the demo shows misses, the fallback is an explicit
  `vacuus-interactive` attribute contract — decide during Task 6's interactive verification,
  not by widening the heuristic silently.
