# M2 API notes: UE threading primitives (FRunnable, TTripleBuffer, queues)

## SUMMARY
UE 5.8.1 ships everything M2's threading contract needs, no custom primitives required. Use `FRunnable` + `FRunnableThread::Create(...)` (Init/Run/Exit all execute on the worker thread despite the misleading header comment on `Exit`), with an auto-reset `FEventRef` for the per-game-frame trigger — auto-reset FEvent is a binary latch, so N Triggers before one Wait coalesce into a single wakeup exactly as §4 requires. `TTripleBuffer<T>` exists at `Core/Public/Containers/TripleBuffer.h` and is a real lock-free SPSC publish-swap with a dirty flag (consumer keeps its last buffer when nothing new arrived) — this is the publish primitive for both the command buffer and the interactive-region snapshot. For queues, prefer `TSpscQueue`/`TMpscQueue` (`Containers/SpscQueue.h`, `Containers/MpscQueue.h`): both `TQueue` and `TCircularQueue` carry in-source "planned for deprecation in favor of TSpscQueue" warnings; `TCircularQueue` is additionally POD-only in practice (`TCircularBuffer` uses `AddZeroed`). `TAtomic` is explicitly documented as "DEPRECATED! ... Use std::atomic<T> for new code" — use `std::atomic` throughout. **ENQUEUE_RENDER_COMMAND from a non-game thread is legal**: `FRenderThreadCommandPipe::Enqueue` (RenderingThread.h:500) branches only on `!IsInRenderingThread() && ShouldExecuteOnRenderThread()`, has zero `IsInGameThread()` checks anywhere on that path, and the shared context is protected by `UE::FMutex` (RenderingThread.h:434, RenderingThread.cpp:1912) — the header's own `FRenderCommandList` docs even show recording render commands from `UE::Tasks::Launch`. So the UI thread may publish directly to the render thread without bouncing through the game thread. For the per-frame trigger, `FTSTicker` fires very late in `FEngineLoop::Tick` (line 6103, after Slate has already drawn); a `FTickableGameObject`-derived GameInstance subsystem (copy `UTickableWorldSubsystem` verbatim) is the correct hook and gives exactly one tick per frame in both PIE and `-game`.

## APIS (21)

### FRunnableThread::Create
```
static CORE_API FRunnableThread* Create(class FRunnable* InRunnable, const TCHAR* ThreadName, uint32 InStackSize = 0, EThreadPriority InThreadPri = TPri_Normal, uint64 InThreadAffinityMask = FPlatformAffinity::GetNoAffinityMask(), EThreadCreateFlags InCreateFlags = EThreadCreateFlags::None);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/HAL/RunnableThread.h:44-50 (impl: Core/Private/HAL/ThreadingBase.cpp:900)
NOTES: InStackSize==0 means platform default. On Unix the value is passed through FRunnableThreadUnix::AdjustStackSize (Core/Private/Unix/UnixPlatformRunnableThread.cpp:178) which clamps any non-zero request up to >=128KB. CRITICAL: ThreadingBase.cpp:908 does `bCreateRealThread = FPlatformProcess::SupportsMultithreading()`; if false AND the runnable returns nullptr from GetSingleThreadInterface(), Create() returns nullptr (no thread, no error). Plan a fallback (run the UI frame inline on the GT) or implement FSingleThreadRunnable. Header include: "HAL/RunnableThread.h".

### EThreadPriority / EThreadCreateFlags
```
enum EThreadPriority { TPri_Normal, TPri_AboveNormal, TPri_BelowNormal, TPri_Highest, TPri_Lowest, TPri_SlightlyBelowNormal, TPri_TimeCritical, TPri_Num };
enum class EThreadCreateFlags : int8 { None = 0, SMTExclusive = (1 << 0) };  // ENUM_CLASS_FLAGS
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/GenericPlatform/GenericPlatformAffinity.h:25-43
NOTES: Note TPri_SlightlyBelowNormal sits between Normal and BelowNormal (non-obvious ordering in the enum). For a UI thread that must not starve the game/render thread, TPri_BelowNormal or TPri_SlightlyBelowNormal is the sane default. FPlatformAffinity::GetNoAffinityMask() == 0xFFFFFFFFFFFFFFFF (same file:88).

### FRunnable (Init/Run/Stop/Exit)
```
virtual bool Init() { return true; }
virtual uint32 Run() = 0;
virtual void Stop() { }
virtual void Exit() { }
virtual class FSingleThreadRunnable* GetSingleThreadInterface() { return nullptr; }
virtual ~FRunnable() = default;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/HAL/Runnable.h:32,45,53,61,69,75
NOTES: The header comment on Exit() says 'Called in the context of the aggregating thread' — THIS IS WRONG. Core/Private/HAL/PThreadRunnableThread.cpp:16-27 shows Init(), Run(), and Exit() all run on the worker thread; Exit() is called immediately after Run() returns, before FreeTls(). So RmlUi/QuickJS teardown belongs in Exit() (or at the tail of Run()), satisfying the spec's 'no RmlUi API from any other thread' rule. Stop() is the ONLY method called from the caller's thread.

### FRunnableThread::Kill / WaitForCompletion
```
virtual bool Kill(bool bShouldWait = true) = 0;
virtual void WaitForCompletion() = 0;
const uint32 GetThreadID() const;
const FString& GetThreadName() const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/HAL/RunnableThread.h:86,89,114,125
NOTES: Kill(true) calls Runnable->Stop() then joins (pthread: Core/Private/HAL/PThreadRunnableThread.h:271-294; Windows: Core/Private/Microsoft/MicrosoftRunnableThread.h:79). Both platform destructors already call Kill(true) (PThreadRunnableThread.h:233-242, MicrosoftRunnableThread.h:51-57), so `delete Thread` IS a correct stop+join — provided Stop() also signals the wake event, otherwise the join deadlocks on an infinite Wait(). The base FRunnableThread::~FRunnableThread (ThreadingBase.cpp:892) only unregisters from FThreadManager; the join comes from the platform subclass.

### FEventRef / FEvent / EEventMode
```
enum class EEventMode { AutoReset, ManualReset };
class FEventRef final { CORE_API explicit FEventRef(EEventMode Mode = EEventMode::AutoReset); CORE_API ~FEventRef(); FEvent* operator->() const; FEvent* Get(); };  // non-copyable, non-movable
// FEvent: virtual void Trigger(); virtual void Reset(); virtual bool Wait(uint32 WaitTime, const bool bIgnoreThreadIdleStats = false); bool Wait();  // Wait() == Wait(MAX_uint32)
bool Wait(const FTimespan& WaitTime, const bool bIgnoreThreadIdleStats = false);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/HAL/Event.h:129,136-160 and 52,59,70,77,89
NOTES: COALESCING CONFIRMED: Core/Public/HAL/PThreadEvent.h Trigger() sets `Triggered = TRIGGERED_ONE` under a mutex for auto-reset events — it is a binary latch, so K Trigger()s while a UI frame is in flight wake the thread exactly once. Exactly the §4 'triggers arriving while a UI frame is in flight are coalesced' semantic, for free. FEvent::Create() is UE_DEPRECATED(5.0) — never call it; use FEventRef (RAII over the pool) or FPlatformProcess::GetSynchEventFromPool. Header: "HAL/Event.h".

### FPlatformProcess::GetSynchEventFromPool / ReturnSynchEventToPool / SupportsMultithreading
```
static CORE_API class FEvent* GetSynchEventFromPool(bool bIsManualReset = false);
static CORE_API void ReturnSynchEventToPool(FEvent* Event);
static CORE_API bool SupportsMultithreading();
static CORE_API void Sleep(float Seconds);
static CORE_API void SleepNoStats(float Seconds);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/GenericPlatform/GenericPlatformProcess.h:786,799,879,749,751
NOTES: Raw-pointer alternative to FEventRef, used by FQueuedThread. Prefer FEventRef unless you need to null the member out mid-life. FPlatformProcess::CreateSynchEvent is UE_DEPRECATED(5.0) at line 776.

### TTripleBuffer<BufferType>
```
template<typename BufferType> class TTripleBuffer {
  TTripleBuffer();                       // default-constructs all 3
  explicit TTripleBuffer(ENoInit);       // allocates, no value-init
  explicit TTripleBuffer(const BufferType& InValue);
  TTripleBuffer(BufferType (&InBuffers)[3]);   // borrows caller memory, starts Dirty
  bool IsDirty() const;
  BufferType& Read();                    // consumer
  void SwapReadBuffers();                // consumer; NO-OP if !IsDirty()
  BufferType& GetWriteBuffer();          // producer
  void SwapWriteBuffers();               // producer; sets Dirty
  void Write(const BufferType Value);    // BY VALUE
  const BufferType& SwapAndRead();
  void WriteAndSwap(const BufferType Value);
  void Reset();
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Containers/TripleBuffer.h:39,69,76,86,103,126,137,149,172,182,199,207,219,231
NOTES: YES, UE ships one. Header: "Containers/TripleBuffer.h". Documented 'thread-safe in single-producer, single-consumer scenarios' (line 32). Publish is not a pointer swap but a CAS on a packed uint8 index register `MS_ALIGN(16) int32 volatile Flags` via FPlatformAtomics::InterlockedCompareExchange (lines 162, 190) — full barriers, so buffer writes before SwapWriteBuffers() are visible after the consumer's SwapReadBuffers(). Producer may swap repeatedly without the consumer; intermediate frames are silently dropped (the desired coalescing). Canonical usage in Core/Tests/Misc/TripleBufferTest.cpp:18-41. ENoInit is `enum ENoInit {NoInit};` at Core/Public/Misc/CoreMiscDefines.h:153.

### TSpscQueue<T, AllocatorType = FMemory>
```
template<typename T, typename AllocatorType = FMemory> class TSpscQueue final {
  template <typename... ArgTypes> void Enqueue(ArgTypes&&... Args);   // in-place construct
  TOptional<ElementType> Dequeue();
  bool Dequeue(ElementType& OutElem);
  bool IsEmpty() const;
  int32 Num() const;
  ElementType* Peek() const;   // consumer only
  FIterator begin() const; std::nullptr_t end() const;   // consumer may iterate without popping
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Containers/SpscQueue.h:18,55,69,93,107,112,120,181,188
NOTES: The 5.8 replacement TQueue points at. Unbounded, recycles consumed nodes internally (no free until destruction), uses std::atomic with explicit acquire/release — no full barriers. Variadic Enqueue means move-only payloads (TUniquePtr<FVaCuusCommandBuffer>) work directly. Also has UE_AUTORTFM_REPORT_HAZARD_IF_CLOSED guards. Sibling TMpscQueue in "Containers/MpscQueue.h":18,47,58,79,96,110 has the identical shape for multi-producer (use it if anything other than the game thread ever pushes).

### TQueue<T, EQueueMode>
```
enum class EQueueMode { Mpsc, Spsc, SingleThreaded };
template<typename T, EQueueMode Mode = EQueueMode::Spsc> class TQueue {
  bool Dequeue(FElementType& OutItem);  bool Pop();  void Empty();
  bool Enqueue(const FElementType& Item);  bool Enqueue(FElementType&& Item);
  bool IsEmpty() const;  bool Peek(FElementType& OutItem) const;  FElementType* Peek();
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Containers/Queue.h:16,46,80,123,165,206,219,239,261
NOTES: Line 11 of the header: '// WARNING: This queue is planned for deprecation in favor of TSpscQueue or TMpscQueue'. Unbounded intrusive linked list, `new TNode` per Enqueue and `delete` per Dequeue — per-element allocation churn on the input path. Dequeue/Pop do `Tail->Item = FElementType()` so T must be default-constructible AND move-assignable. Spsc mode = exactly ONE producer thread; uses FPlatformMisc::MemoryBarrier() (full fence), heavier than TSpscQueue. Only Dequeue/Pop/Peek/IsEmpty/Empty are consumer-side.

### TCircularQueue<T>
```
template<typename T> class TCircularQueue {
  explicit TCircularQueue(uint32 CapacityPlusOne);   // rounded up to next power of 2
  uint32 Count() const;  bool Dequeue(FElementType& OutElement);  bool Dequeue();  void Empty();
  bool Enqueue(const FElementType& Element);  bool Enqueue(FElementType&& Element);  // false when full
  bool IsEmpty() const;  bool IsFull() const;
  bool Peek(FElementType& OutItem) const;  const FElementType* Peek() const;
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Containers/CircularQueue.h:25,35,51,70,91,111,123,146,171,185,197,218
NOTES: Line 9: '// WARNING: This queue is planned for deprecation in favor of TSpscQueue'. Bounded (Enqueue returns false when full) — the natural fit for §4's fixed-size input ring, but see pitfalls: it uses TAtomic<uint32> Head/Tail (CircularQueue.h:236,239) with sequentially-consistent loads/stores, and its backing TCircularBuffer ctor does `Elements.AddZeroed(FMath::RoundUpToPowerOfTwo(Capacity))` (Containers/CircularBuffer.h:29) — so restrict T to trivially-constructible POD. Usable capacity is Capacity-1.

### TAtomic (deprecation status)
```
template <typename T> class TAtomic final : public UE::Core::Private::Atomic::TAtomicBaseType_T<T>
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Templates/Atomic.h:13 and 528
NOTES: Line 13: '// `TAtomic` is planned for deprecation. Please use `std::atomic`'. Line 528 doc block: 'DEPRECATED! UE atomics are not maintained and potentially will be physically deprecated. Use std::atomic<T> for new code'. House preference in 5.8 is unambiguously std::atomic — the newest Core code (TSpscQueue, TMpscQueue, FTSTicker, FRenderCommandList) all use std::atomic + explicit memory_order. Use `std::atomic<bool> bStopRequested{false}` with acquire/release, not TAtomic and not FThreadSafeCounter (FQueuedThread's TAtomic<bool> TimeToDie at ThreadingBase.cpp:1102 is legacy).

### FPlatformTLS::GetCurrentThreadId
```
static uint32 GetCurrentThreadId(void);   // via "HAL/PlatformTLS.h"
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/GenericPlatform/GenericPlatformTLS.h:29 (reference-only #if 0 block); real impls: Core/Public/Unix/UnixPlatformTLS.h:26 (SYS_gettid, cached in TLS), Core/Public/Windows/WindowsPlatformTLS.h
NOTES: Cheap after first call on Unix (cached in a TLS slot). Model your IsInVaCuusUIThread() on the engine's own globals: `extern CORE_API uint32 GGameThreadId;` / `extern CORE_API uint32 GRenderThreadId;` at Core/Public/CoreGlobals.h:559,563. NOTE 5.8's IsInGameThread()/IsInRenderingThread() (Core/Private/HAL/ThreadingBase.cpp:190, 276) are FTaskTagScope-based, not id-based; a plain FRunnableThread is untagged, so both correctly return false on the UI thread and engine `check(IsInGameThread())` asserts will fire as intended if you call GT-only API by mistake.

### FTSTicker
```
DECLARE_DELEGATE_RetVal_OneParam(bool, FTickerDelegate, float);
class FTSTicker {
  using FDelegateHandle = TWeakPtr<FElement>;
  static CORE_API FTSTicker& GetCoreTicker();
  CORE_API FDelegateHandle AddTicker(const FTickerDelegate& InDelegate, float InDelay = 0.0f);
  CORE_API FDelegateHandle AddTicker(const TCHAR* InName, float InDelay, TUniqueFunction<bool(float)>&& InFunction);
  static CORE_API void RemoveTicker(FDelegateHandle Handle);
  CORE_API void Tick(float DeltaTime);
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Containers/Ticker.h:21,26,32,35,45,56,66,81
NOTES: Thread-safe Add/Remove (RemoveTicker blocks if the delegate is mid-execution — a real teardown guarantee). Fires once per FEngineLoop::Tick. BUT the call site is Runtime/Launch/Private/LaunchEngineLoop.cpp:6103, i.e. AFTER GEngine->Tick (5859) and AFTER FSlateApplication::Get().Tick(TimeAndWidgets) (5991), just before EndFrameRenderThread. Delta is FApp::GetDeltaTime() — undilated, world-agnostic, keeps firing while PIE is paused. Fine as a fallback/editor-only hook, wrong place for 'gather game state then trigger'.

### FTickableGameObject
```
enum class ETickableTickType : uint8 { Always, Conditional, Never, NewObject };
class FTickableGameObject : public FTickableObjectBase {
  ENGINE_API FTickableGameObject(ETickableTickType StartingTickType = ETickableTickType::NewObject);
  ENGINE_API virtual ~FTickableGameObject();
  virtual void Tick(float DeltaTime) = 0;
  virtual TStatId GetStatId() const = 0;
  virtual ETickableTickType GetTickableTickType() const { return ETickableTickType::Conditional; }
  virtual bool IsTickable() const { return true; }
  virtual bool IsTickableWhenPaused() const { return false; }
  virtual bool IsTickableInEditor() const { return false; }
  virtual UWorld* GetTickableGameObjectWorld() const { return nullptr; }
  ENGINE_API void SetTickableTickType(ETickableTickType NewTickType);
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Engine/Public/Tickable.h:19,95,106,114,125,140,158,171,182,194,206 (dispatch: Engine/Private/Tickable.cpp:170-226)
NOTES: THIS is the reliable once-per-game-frame hook. Tickable.cpp:177 asserts check(IsInGameThread()); the dispatch filter at Tickable.cpp:194 is `TickableObject->GetTickableGameObjectWorld() == World`, so each object is ticked by exactly ONE call site per frame — never double-ticked, even with multiple PIE worlds. Return the GameInstance's world => ticked inside UWorld::Tick right after actor ticks + timer manager and before camera update / StartAsyncSendAllEndOfFrameUpdates (Engine/Private/LevelTick.cpp:1821) — the correct slot for 'snapshot game state -> push to UI thread -> Trigger'. Return nullptr => ticked after all world ticks, before viewport tick (GameEngine.cpp:1973 / EditorEngine.cpp:2219). NOTE: UGameInstanceSubsystem (Engine/Public/Subsystems/GameInstanceSubsystem.h:16) has NO Tick of its own — you must multiply-inherit FTickableGameObject.

### UTickableWorldSubsystem (the pattern to copy for a tickable GameInstance subsystem)
```
UCLASS(Abstract, MinimalAPI)
class UTickableWorldSubsystem : public UWorldSubsystem, public FTickableGameObject {
  ENGINE_API UTickableWorldSubsystem();   // : FTickableGameObject(ETickableTickType::Never)
  ENGINE_API UWorld* GetTickableGameObjectWorld() const override;
  ENGINE_API virtual ETickableTickType GetTickableTickType() const override;
  ENGINE_API virtual bool IsAllowedToTick() const override final;
  ENGINE_API virtual void Tick(float DeltaTime) override;
  virtual TStatId GetStatId() const override PURE_VIRTUAL(...);
  ENGINE_API virtual void Initialize(FSubsystemCollectionBase& Collection) override;
  ENGINE_API virtual void Deinitialize() override;
  ENGINE_API virtual void BeginDestroy() override;
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Engine/Public/Subsystems/WorldSubsystem.h:80-106; impl Engine/Private/Subsystems/WorldSubsystem.cpp:57,63,68,80,89,94,104,114
NOTES: There is no UTickableGameInstanceSubsystem in 5.8 — copy this class body onto UGameInstanceSubsystem. The essential shape: ctor passes ETickableTickType::Never (UObjects can be constructed off the GT), Initialize() sets bInitialized then calls SetTickableTickType(GetTickableTickType()), Deinitialize() calls SetTickableTickType(ETickableTickType::Never). GetStatId via RETURN_QUICK_DECLARE_CYCLE_STAT(UVaCuusSubsystem, STATGROUP_Tickables) (Core/Public/Stats/Stats.h:123; example Engine/Classes/Engine/AutoDestroySubsystem.h:52).

### FWorldDelegates tick hooks
```
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnWorldTickStart, UWorld*, ELevelTick, float);        static UE_API FOnWorldTickStart OnWorldTickStart;
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnWorldPreActorTick, UWorld*, ELevelTick, float);     static UE_API FOnWorldPreActorTick OnWorldPreActorTick;
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnWorldPostActorTick, UWorld*, ELevelTick, float);    static UE_API FOnWorldPostActorTick OnWorldPostActorTick;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Engine/Classes/Engine/World.h:4516-4527
NOTES: Global (not per-world) multicasts — every subscriber fires for EVERY ticking world, so with multiple PIE clients a single subsystem would be invoked N times per frame and must filter `World == GetGameInstance()->GetWorld()` itself. Strictly worse than FTickableGameObject, which does that filtering in the engine (Tickable.cpp:194). Use only if you specifically need the pre-actor-tick slot.

### ENQUEUE_RENDER_COMMAND — legal from a non-game thread
```
#define ENQUEUE_RENDER_COMMAND(Type, ...) \
  DECLARE_RENDER_COMMAND_TAG(UE_JOIN(FRenderCommandTag_, Type, __LINE__), Type, __VA_ARGS__) \
  FRenderCommandDispatcher::Enqueue<UE_JOIN(FRenderCommandTag_, Type, __LINE__)>

// dispatch chain:
template <typename RenderCommandTag> static void FRenderCommandDispatcher::Enqueue(TUniqueFunction<void(FRHICommandListImmediate&)>&& Function);
template <typename RenderCommandTag, typename LambdaType> static void FRenderThreadCommandPipe::Enqueue(LambdaType&& Lambda);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/RenderCore/Public/RenderingThread.h:1087 (macro), :993 (dispatcher), :500 (pipe); impl RenderCore/Private/RenderingThread.cpp:1905
NOTES: VERDICT: the UI thread MAY publish directly to the render thread; no game-thread bounce needed. Evidence: (1) RenderingThread.h:504 branches on `!IsInRenderingThread() && ShouldExecuteOnRenderThread()` — no IsInGameThread() check anywhere on the enqueue path; (2) `ShouldExecuteOnRenderThread()` is `(LIKELY(GIsThreadedRendering || !IsInGameThread()))` (RenderingThread.h:149) — true from any non-GT thread even when threaded rendering is off; (3) `CheckNotBlockedOnRenderThread()` is `ensure(!GMainThreadBlockedOnRenderThread.Load(...) || !IsInGameThread())` (RenderingThread.h:81) — vacuously true off the GT; (4) the shared FContext is guarded by `UE::FMutex Mutex` (RenderingThread.h:434) locked in FRenderThreadCommandPipe::EnqueueAndLaunch (RenderingThread.cpp:1912-1914); (5) FRenderCommandList::GetInstanceTLS() (RenderingThread.h:1009) is `static thread_local` (:746) and is simply null on our thread, falling through to the global pipe. The M1 code already uses this macro (VcHost/Plugins/VaCuus/Source/VaCuusRender/Private/VaCuusM1Harness.cpp:122) — the only change for M2 is the calling thread. Requires RenderCore in the module's dependency list (already present: VaCuusRender.Build.cs:26).

### FRenderCommandList / FRecordScope (batched off-thread recording)
```
static FRenderCommandList* Create(ERenderCommandListFlags InFlags = ERenderCommandListFlags::None, EPageSize InPageSize = EPageSize::Small);
class FRecordScope { RENDERCORE_API FRecordScope(FRenderCommandList* InCommandList, EStopRecordingAction StopAction = EStopRecordingAction::None); RENDERCORE_API ~FRecordScope(); };
enum class EStopRecordingAction { None, Close, Submit };
static void FRenderCommandDispatcher::Submit(FRenderCommandList* RenderCommandList, FRenderCommandList* ParentCommandList = nullptr);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/RenderCore/Public/RenderingThread.h:724-745 (doc), 753, 776, 877, 987
NOTES: The class doc (RenderingThread.h:725) literally says 'recorded on a thread and submitted', and its own usage example (RenderingThread.h:809-832) records render commands inside `UE::Tasks::Launch(...)` — Epic's own sanction for off-game-thread enqueue. Binds a thread_local command list so ENQUEUE_RENDER_COMMAND inside the scope costs no lock. Overkill for M2 (one publish command per UI frame); the plain path is fine. Mentioned so the plan author knows the escape hatch exists if the per-publish mutex ever shows on a profile.

### FRenderCommandFence — GAME THREAD ONLY
```
RENDERCORE_API void BeginFence(ESyncDepth SyncDepth = ESyncDepth::RenderThread);
RENDERCORE_API void Wait(bool bProcessGameThreadTasks = false) const;
RENDERCORE_API bool IsFenceComplete() const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/RenderCore/Public/RenderCommandFence.h:35,41,44; impl RenderCore/Private/RenderingThread.cpp:977-984
NOTES: BeginFence() has a hard `check(IsInGameThread())` at RenderingThread.cpp:984. The UI thread CANNOT use render command fences. For §4's 'releases are deferred until the last command buffer referencing the handle has been replayed', use the spec's own generation-counter scheme resolved on the render thread, not a fence.

### UE::FMutex / UE::TScopeLock
```
class FMutex final { inline void Lock(); [[nodiscard]] inline bool TryLock(); inline void Unlock(); };   // "Async/Mutex.h"
template<typename MutexType> class UE::TScopeLock { [[nodiscard]] explicit TScopeLock(MutexType& InMutex); void Unlock(); };   // "Misc/ScopeLock.h"
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Async/Mutex.h:18,38,46,57; Core/Public/Misc/ScopeLock.h:20-47
NOTES: UE::FMutex is the modern 1-byte futex-backed lock 5.8 uses internally (e.g. FRenderCommandPipeBase). Prefer over FCriticalSection where a lock is unavoidable. Not needed on the M2 hot path if you stick to TTripleBuffer + TSpscQueue.

### FThreadManager auto-registration / Insights naming
```
CORE_API void FThreadManager::AddThread(uint32 ThreadId, FRunnableThread* Thread);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Private/HAL/ThreadingBase.cpp:492-521 (UE::Trace::ThreadRegister at :519)
NOTES: Free win: the ThreadName passed to FRunnableThread::Create is auto-registered with Unreal Insights (with a priority-derived sort hint), so the UI thread shows up as its own track in traces with no extra work. The OS-level name on Linux is truncated to 15 chars (UnixThreadNameLimit, Core/Public/Unix/UnixPlatformRunnableThread.h:24) — keep the name short, e.g. TEXT("VaCuusUI").

## PATTERNS (6)

### UI thread runnable: stop flag + coalescing wake event. Stop() is the ONLY method called from the caller's thread; it must set the flag AND trigger, or Kill(true)'s join deadlocks.
```cpp
class FVaCuusUIThread final : public FRunnable
{
public:
    bool Init() override { UIThreadId.store(FPlatformTLS::GetCurrentThreadId(), std::memory_order_release); /* Rml::Initialise, JSRuntime */ return true; }

    uint32 Run() override
    {
        while (!bStopRequested.load(std::memory_order_acquire))
        {
            WakeEvent->Wait();                                   // auto-reset => N triggers coalesce to 1
            if (bStopRequested.load(std::memory_order_acquire)) { break; }
            RunOneUIFrame();                                     // drain queues, Update, Render, publish
        }
        return 0;
    }

    void Stop() override { bStopRequested.store(true, std::memory_order_release); WakeEvent->Trigger(); }
    void Exit() override { /* unload docs -> destroy JS -> Rml::Shutdown, ON THIS THREAD */ }

    void RequestFrame() { WakeEvent->Trigger(); }                // called from the game thread

private:
    FEventRef            WakeEvent{ EEventMode::AutoReset };
    std::atomic<bool>    bStopRequested{ false };
    std::atomic<uint32>  UIThreadId{ 0 };
};
```
(precedent: FQueuedThread (Core/Private/HAL/ThreadingBase.cpp:1093-1216 + Run() at :1465) uses exactly this DoWorkEvent/TimeToDie shape; std::atomic substituted for its legacy TAtomic<bool> per Templates/Atomic.h:13.)

### Create / destroy. `delete Thread` alone performs Stop()+join because both platform destructors call Kill(true) — but destruction ORDER matters: the FRunnableThread must die before the FRunnable it points at.
```cpp
// Start (game thread)
Thread.Reset(FRunnableThread::Create(
    Runnable.Get(),
    TEXT("VaCuusUI"),               // <=15 chars for the Linux OS thread name
    512 * 1024,                     // 0 = platform default; Unix clamps non-zero up to >=128KB
    TPri_BelowNormal,
    FPlatformAffinity::GetNoAffinityMask()));

if (!Thread)                        // SupportsMultithreading()==false and no FSingleThreadRunnable
{
    bRunUIInlineOnGameThread = true;
}

// Shutdown (game thread)
Thread.Reset();                     // ~FRunnableThreadPThread -> Kill(true) -> Stop() + pthread_join
Runnable.Reset();                   // only now is it safe to destroy the FRunnable (owns WakeEvent)
```
(precedent: Core/Private/HAL/PThreadRunnableThread.h:233-242 (dtor -> Kill(true)); Core/Private/Microsoft/MicrosoftRunnableThread.h:51-57; nullptr-on-failure path at Core/Private/HAL/ThreadingBase.cpp:908-930.)

### Publish a UI frame from the UI thread, consume on the game thread (interactive-region snapshot). TTripleBuffer is SPSC in both directions — use two separate instances.
```cpp
// UI thread (producer)
FVaCuusRegionSnapshot& Dst = RegionBuffer.GetWriteBuffer();   // in-place, no copy
Dst = MoveTemp(NewSnapshot);
RegionBuffer.SwapWriteBuffers();                              // sets Dirty; publish

// Game thread (consumer), inside SVaCuusWidget::OnMouseButtonDown
const FVaCuusRegionSnapshot& Snap = RegionBuffer.SwapAndRead();  // no-op swap if !IsDirty(); keeps last good
return Snap.HitTest(LocalPos) ? FReply::Handled() : FReply::Unhandled();
```
(precedent: Containers/TripleBuffer.h:149,172,182,219 + canonical sequence in Core/Tests/Misc/TripleBufferTest.cpp:25-41. Use GetWriteBuffer()/SwapWriteBuffers() rather than Write()/WriteAndSwap(), which take BufferType BY VALUE (TripleBuffer.h:199,231).)

### Game thread -> UI thread per-frame drive, from a tickable GameInstance subsystem.
```cpp
UCLASS()
class UVaCuusSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
    GENERATED_BODY()
public:
    UVaCuusSubsystem() : FTickableGameObject(ETickableTickType::Never) {}

    virtual void Initialize(FSubsystemCollectionBase& C) override { Super::Initialize(C); bInit = true; SetTickableTickType(ETickableTickType::Conditional); }
    virtual void Deinitialize() override { SetTickableTickType(ETickableTickType::Never); bInit = false; Super::Deinitialize(); }

    virtual UWorld* GetTickableGameObjectWorld() const override { return GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr; }
    virtual ETickableTickType GetTickableTickType() const override { return (IsTemplate() || !bInit) ? ETickableTickType::Never : ETickableTickType::Conditional; }
    virtual bool IsTickableWhenPaused() const override { return true; }   // UI must animate while paused
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UVaCuusSubsystem, STATGROUP_Tickables); }

    virtual void Tick(float DeltaTime) override
    {
        PushDataSnapshots(); PushInputEvents();
        UIThread->RequestFrame();     // coalescing Trigger
    }
private:
    bool bInit = false;
};
```
(precedent: Verbatim shape of UTickableWorldSubsystem (Engine/Public/Subsystems/WorldSubsystem.h:80-106, Engine/Private/Subsystems/WorldSubsystem.cpp:57-112); dispatch guarantee at Engine/Private/Tickable.cpp:194.)

### Publish the command buffer straight from the UI thread to the render thread — no game-thread bounce.
```cpp
// running ON the VaCuus UI thread; move-only payload is fine
TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder->EndFrameAndPublish();

ENQUEUE_RENDER_COMMAND(VaCuusPublishUIFrame)(
    [LocalElement = Element, Buf = MoveTemp(Buffer)](FRHICommandListImmediate& RHICmdList) mutable
    {
        LocalElement->SetPendingBuffer_RenderThread(RHICmdList, MoveTemp(Buf));
    });
```
(precedent: Identical to the existing M1 call at /w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuusRender/Private/VaCuusM1Harness.cpp:122-127, only the calling thread changes. Legality established by RenderingThread.h:504 (no IsInGameThread check) + the UE::FMutex guard at RenderingThread.cpp:1912.)

### Debug-only thread-affinity assertion for the RmlUi/QuickJS wrappers required by §4.
```cpp
// VaCuusThreading.h
extern std::atomic<uint32> GVaCuusUIThreadId;   // set in FVaCuusUIThread::Init()

FORCEINLINE bool IsInVaCuusUIThread()
{
    return FPlatformTLS::GetCurrentThreadId() == GVaCuusUIThreadId.load(std::memory_order_relaxed);
}

#define VACUUS_CHECK_UI_THREAD() checkSlow(IsInVaCuusUIThread())
```
(precedent: Mirrors the engine's own GGameThreadId / GRenderThreadId globals (Core/Public/CoreGlobals.h:559,563) compared against FPlatformTLS::GetCurrentThreadId(). Deliberately does NOT use FTaskTagScope — leaving the UI thread untagged is what makes engine `check(IsInGameThread())` fire correctly if GT-only API is called by mistake (Core/Private/HAL/ThreadingBase.cpp:190).)

## PITFALLS
- FRunnable::Exit()'s header comment ('Called in the context of the aggregating thread', Runnable.h:58) is FALSE. PThreadRunnableThread.cpp:16-27 shows Init/Run/Exit all execute on the worker thread. This is load-bearing for §4's shutdown order — RmlUi/QuickJS teardown MUST be in Exit() or the tail of Run(), never in the FRunnable's destructor (which runs on the game thread).
- Stop() is the only FRunnable method invoked from the caller's thread, and Kill(true)/the platform destructor calls it BEFORE joining. If Stop() only sets the flag and does not Trigger the wake event, `delete Thread` deadlocks forever inside pthread_join because Run() is parked in WakeEvent->Wait(MAX_uint32).
- Destruction order: FEventRef is a member of the FRunnable and is non-movable/non-copyable. Destroy the FRunnableThread first (joins), then the FRunnable. Reversed order = use-after-free of the event from the still-running thread.
- FRunnableThread::Create returns nullptr — silently, no log — when FPlatformProcess::SupportsMultithreading() is false and the runnable has no GetSingleThreadInterface() (ThreadingBase.cpp:908-930). Hit by -nothreading, commandlets, and some server configs. Null-check the result and plan an inline-on-GT fallback, or the whole UI is dead with no diagnostic.
- TTripleBuffer::Write()/WriteAndSwap() take `const BufferType Value` BY VALUE (TripleBuffer.h:199,231) — a full copy of the payload, and uncompilable for move-only types. Always use GetWriteBuffer() + SwapWriteBuffers() instead.
- TTripleBuffer<TUniquePtr<T>> will NOT compile with the default constructor: TripleBuffer.h:72 does `Buffers[0] = Buffers[1] = Buffers[2] = BufferType();`, whose chained assignment copy-assigns from an lvalue. Use `TTripleBuffer<TUniquePtr<T>> Buf(NoInit);` (TripleBuffer.h:76) which only default-constructs the array.
- TTripleBuffer::Read() returns a reference INTO the buffer that is invalidated by the consumer's next SwapReadBuffers()/SwapAndRead(). Do not cache it across frames. Also, the 3-buffer ctor that borrows caller memory (TripleBuffer.h:103) starts with the Dirty bit ALREADY SET, unlike every other ctor.
- SwapReadBuffers() early-returns when !IsDirty() (TripleBuffer.h:151) — the consumer silently keeps its previous buffer. That is the desired 'render thread never waits' behavior, but any code that assumes each read is a fresh frame is wrong; carry an explicit frame/generation counter inside the payload.
- Both TQueue (Queue.h:11) and TCircularQueue (CircularQueue.h:9) carry in-source 'planned for deprecation in favor of TSpscQueue' warnings. Do not build new M2 code on them; use TSpscQueue/TMpscQueue.
- TQueue allocates one node with `new` per Enqueue and `delete`s per Dequeue (Queue.h:125,99). On a per-input-event path that is real allocator churn on the game thread. TSpscQueue recycles nodes internally.
- TQueue<T,Spsc> Dequeue/Pop do `Tail->Item = FElementType()` (Queue.h:98,277) — T must be default-constructible and move-assignable, and the assignment runs a destructor on the consumer thread. Watch out for payloads with UI-thread-affine destructors.
- TCircularQueue's backing TCircularBuffer ctor uses `Elements.AddZeroed(...)` (CircularBuffer.h:29) — memzeroing the storage. Only use it with trivially-constructible POD element types; an input event struct containing FString/TArray is undefined behavior.
- TAtomic is documented 'DEPRECATED! ... Use std::atomic<T> for new code' (Templates/Atomic.h:528) and 'planned for deprecation' (line 13). Do not introduce TAtomic, FThreadSafeBool, or FThreadSafeCounter in new M2 code, even though older engine code (FQueuedThread, TCircularQueue) still uses them.
- FRenderCommandFence::BeginFence() hard-asserts check(IsInGameThread()) at RenderCore/Private/RenderingThread.cpp:984. The UI thread cannot fence the render thread. Resource-release deferral must ride the spec's per-buffer generation counter, resolved render-thread-side.
- Render commands enqueued from the UI thread are NOT ordered against commands enqueued from the game thread — interleaving is decided by which thread wins the UE::FMutex in FRenderThreadCommandPipe::EnqueueAndLaunch (RenderingThread.cpp:1912). Never rely on 'my publish lands after the GT's BeginFrame'; make the render-side consumer order-independent.
- FlushRenderingCommands() on the game thread does not fence UI-thread enqueues that race it. CheckNotBlockedOnRenderThread() (RenderingThread.h:81) only ensures on the game thread, so an off-thread enqueue during a GT flush produces no warning — it just may or may not be included. Gate UI-thread publishing during level teardown/RHI shutdown explicitly (stop accepting commands -> drain UI thread, per §4) rather than relying on the engine to catch it.
- FTSTicker::GetCoreTicker().Tick() runs at LaunchEngineLoop.cpp:6103 — after GEngine->Tick (5859) AND after FSlateApplication::Tick(TimeAndWidgets) (5991). A trigger from there produces UI content one full frame late relative to Slate. It also uses FApp::GetDeltaTime() (undilated, world-agnostic) and keeps firing while PIE is paused. Use FTickableGameObject for the real drive.
- UGameInstanceSubsystem has no Tick (GameInstanceSubsystem.h:16) and there is no UTickableGameInstanceSubsystem in 5.8. You must multiply-inherit FTickableGameObject and replicate the UTickableWorldSubsystem lifecycle by hand — in particular constructing with ETickableTickType::Never and enabling in Initialize(), because UObjects can be constructed on worker threads (FTickableGameObject's default ctor ensures IsInGameThread(), Tickable.cpp:140).
- In the editor, FTickableGameObject::TickObjects(nullptr, ...) is only called when `bAWorldTicked` (EditorEngine.cpp:2217-2220). Returning nullptr from GetTickableGameObjectWorld() therefore gives no tick in an idle editor unless IsTickableInEditor() returns true. Returning the GameInstance's world sidesteps this and is the correct choice for PIE.
- FWorldDelegates::OnWorldTickStart/PreActorTick/PostActorTick are global multicasts fired once per ticking world (World.h:4516-4527). With 2+ PIE clients a single subsystem handler fires N times per frame; you must filter by world yourself. FTickableGameObject does that filtering in the engine (Tickable.cpp:194) — prefer it.
- Linux truncates the OS thread name to 15 characters (UnixPlatformRunnableThread.h:24). Names longer than that are silently cut in gdb/htop (Insights keeps the full name via UE::Trace::ThreadRegister, ThreadingBase.cpp:519). Keep it short, e.g. TEXT("VaCuusUI").
- On Unix, a non-zero InStackSize is clamped up to at least 128KB (UnixPlatformRunnableThread.cpp:178-189) because Logf does stack allocations. Passing a small value like 64KB does not do what it says. QuickJS + RmlUi layout recursion argue for an explicit generous stack (256-512KB) rather than 0/platform default.
- Do NOT wrap the UI thread in an FTaskTagScope. Leaving it untagged is what makes IsInGameThread()/IsInRenderingThread() (ThreadingBase.cpp:190,276 — both FTaskTagScope-based in 5.8, not thread-id-based) return false, so engine-side check(IsInGameThread()) assertions correctly catch accidental GT-only API calls from the UI thread.
