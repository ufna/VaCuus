# M2 API notes: editor live reload (DirectoryWatcher, PIE lookup)

## SUMMARY
DirectoryWatcher exists and works on Linux in 5.8 via inotify, but it is 100% pull-based: `IDirectoryWatcher::Tick(float)` is what reads the inotify fd and fires your delegate, and the ONLY engine caller of that Tick is `UEditorEngine::Tick` (EditorEngine.cpp:1948). So the callback always arrives on the game thread inside the editor tick — no thread marshaling is needed at all, and conversely the watcher is completely dead in a non-editor build even if you link it. Register with `IDirectoryWatcher::RegisterDirectoryChangedCallback_Handle(Dir, FDirectoryChanged::CreateRaw(...), OutHandle, Flags)`; the Linux impl `checkf(IsInGameThread())` in Init/WatchDirectoryTree/ProcessNotifications, so registration must also be on the game thread. Debounce is not provided: on Linux a single editor save yields multiple `IN_MODIFY` → several `FCA_Modified` entries, and the engine's own consumers use the "set a dirty flag in the callback, act in a later Tick" shape (SScreenShotBrowser, FMaterialSourceTemplate) rather than timers — pair that with an `FTSTicker::GetCoreTicker().AddTicker(Name, Delay, Fn)` coalescing pass. For poking the runtime in PIE, iterate `GEngine->GetWorldContexts()` for `Context.WorldType == EWorldType::PIE`, take `Context.OwningGameInstance` (or `Context.World()->GetGameInstance()`), and call `GameInstance->GetSubsystem<UVaCuusSubsystem>()` — this is the correct multi-PIE-instance-safe form; `GEditor->PlayWorld` / `GEditor->GetPIEWorldContext()` only give you instance 0. Biggest Linux trap: `RegisterDirectoryChangedCallback_Handle` returns **true with a valid handle even when the directory does not exist** (Linux `Init` ignores `inotify_add_watch` failures), unlike Windows which returns false — so you must `DirectoryExists()` yourself or live reload silently no-ops.

## APIS (22)

### FFileChangeData
```
struct FFileChangeData { enum EFileChangeAction { FCA_Unknown, FCA_Added, FCA_Modified, FCA_Removed, FCA_RescanRequired }; FFileChangeData(const FString& InFilename, EFileChangeAction InAction); FString Filename; int64 TimeStamp = 0; EFileChangeAction Action = FCA_Unknown; };
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Developer/DirectoryWatcher/Public/IDirectoryWatcher.h:8-40
NOTES: The ctor calls FPaths::MakeStandardFilename(Filename) (line 23), so Filename comes back in engine-standard form — typically RELATIVE, e.g. '../../../VcHost/Content/UI/hud.rml', not absolute. Every engine consumer re-absolutizes with FPaths::ConvertRelativePathToFull before comparing (StringTableRegistry.cpp:169, LandscapeImageFileCache.cpp:131). TimeStamp is only populated for FCA_RescanRequired. Not deprecated.

### IDirectoryWatcher::FDirectoryChanged
```
DECLARE_DELEGATE_OneParam(FDirectoryChanged, const TArray<struct FFileChangeData>& /*FileChanges*/);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Developer/DirectoryWatcher/Public/IDirectoryWatcher.h:59
NOTES: Single-cast, non-thread-safe delegate. Create with CreateRaw / CreateSP / CreateUObject / CreateLambda. Invoked synchronously from IDirectoryWatcher::Tick, i.e. the game thread in the editor. Registering/unregistering another watch from inside the callback is explicitly supported — both Linux and Windows Tick snapshot the request list before dispatching (DirectoryWatchRequestLinux.cpp:148-160, DirectoryWatcherWindows.cpp:123-140).

### IDirectoryWatcher::RegisterDirectoryChangedCallback_Handle
```
virtual bool RegisterDirectoryChangedCallback_Handle(const FString& Directory, const FDirectoryChanged& InDelegate, FDelegateHandle& OutHandle, uint32 Flags = 0) = 0;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Developer/DirectoryWatcher/Public/IDirectoryWatcher.h:72
NOTES: Flags come from IDirectoryWatcher::WatchOptions (h:49-56): IncludeDirectoryChanges = 1<<0, IgnoreChangesInSubtree = 1<<1. Default Flags=0 means: recurse into subdirectories, and DO NOT report directory-level add/remove — which is what live reload wants. Path may be relative; both Linux and the proxy call FPaths::ConvertRelativePathToFull internally (DirectoryWatcherLinux.cpp:44, DirectoryWatcherProxy.cpp:12). MUST be called on the game thread on Linux (checkf below). LINUX RETURN-VALUE TRAP: returns true even for a non-existent directory.

### IDirectoryWatcher::UnregisterDirectoryChangedCallback_Handle
```
virtual bool UnregisterDirectoryChangedCallback_Handle(const FString& Directory, FDelegateHandle InHandle) = 0;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Developer/DirectoryWatcher/Public/IDirectoryWatcher.h:80
NOTES: Directory string must match what you registered with (proxy keys on ConvertRelativePathToFull + trailing slash; Linux keys on ConvertRelativePathToFull). Guard with FModuleManager::Get().IsModuleLoaded("DirectoryWatcher") during shutdown so you don't resurrect an unloading module — see StringTableRegistry.cpp:45.

### IDirectoryWatcher::Tick
```
virtual void Tick(float DeltaSeconds) { }
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Developer/DirectoryWatcher/Public/IDirectoryWatcher.h:85
NOTES: THE dispatch pump. Linux override reads the inotify fd and invokes all delegates (DirectoryWatcherLinux.cpp:111-123). Called exactly once per editor frame by UEditorEngine::Tick at EditorEngine.cpp:1948, guarded by !FApp::IsProjectNameEmpty(). Can be called manually to force a synchronous flush — precedent passes -1.0f: WorldPartitionEditorModule.cpp:719.

### FDirectoryWatcherModule::Get
```
virtual IDirectoryWatcher* Get();   // class FDirectoryWatcherModule : public IModuleInterface
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Developer/DirectoryWatcher/Public/DirectoryWatcherModule.h:13-27
NOTES: Returns an FDirectoryWatcherProxy* (never null once the module started — DirectoryWatcherModule.cpp:13). On unsupported platforms the proxy's Inner is FDirectoryWatcherStub whose Register always returns false (DirectoryWatcherStub.h:15). Engine code still null-checks the return. Module name string is exactly "DirectoryWatcher"; IMPLEMENT_MODULE at DirectoryWatcherModule.cpp:8.

### FDirectoryWatcherModule::RegisterExternalChanges
```
virtual void RegisterExternalChanges(TArrayView<const FFileChangeData> FileChanges) const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Developer/DirectoryWatcher/Public/DirectoryWatcherModule.h:23
NOTES: Inject synthetic change events the OS watcher can't see. This is the ONE path that is thread-safe: FDirectoryWatcherProxy::RegisterExternalChanges checks IsInGameThread() and otherwise bounces via FFunctionGraphTask to ENamedThreads::GameThread (DirectoryWatcherProxy.cpp:76-86). Useful if VaCuus ever gets its own poll-based fallback and wants to reuse the same delegate plumbing.

### DirectoryWatcher module Build.cs dependency
```
if (Target.bBuildEditor == true) { PrivateIncludePathModuleNames.Add("DirectoryWatcher"); DynamicallyLoadedModuleNames.Add("DirectoryWatcher"); }
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Core.Build.cs:62-66 (identical shape at Runtime/AssetRegistry/AssetRegistry.Build.cs:22-26)
NOTES: Two valid forms. (a) Editor-only modules (VaCuusEditor is "Type": "Editor" per VaCuus.uplugin:33-37) may simply add "DirectoryWatcher" to PrivateDependencyModuleNames unconditionally — UnrealEd itself does this in PublicDependencyModuleNames (UnrealEd.Build.cs:59). (b) If it ever lands in a Runtime module, use the bBuildEditor guard + DynamicallyLoadedModuleNames above. The module's own rules set bRequiresPlatformSDK = true (DirectoryWatcher.Build.cs:36) and depend only on Core.

### FDirectoryWatchRequestLinux game-thread asserts
```
checkf(IsInGameThread(), TEXT("INotify operations only support on main thread"));
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Developer/DirectoryWatcher/Private/Linux/DirectoryWatchRequestLinux.cpp:79 (Init), :144 (RemoveDelegate path / ProcessNotifications entry), :210 (WatchDirectoryTree), :300 (UnwatchDirectoryTree)
NOTES: Hard assert, not an ensure. Registration, unregistration and dispatch are all game-thread-only on Linux. Answers the marshaling question definitively: the callback CANNOT fire off the game thread on Linux; no AsyncTask/ENamedThreads::GameThread hop is required in the handler.

### Linux inotify backend details
```
GFileDescriptor = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);  ...  int32 NotifyFilter = IN_CREATE | IN_MOVE | IN_MODIFY | IN_DELETE | IN_ONLYDIR;  int32 WatchDescriptor = inotify_add_watch(GFileDescriptor, TCHAR_TO_UTF8(*FolderName), NotifyFilter);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Developer/DirectoryWatcher/Private/Linux/DirectoryWatchRequestLinux.cpp:98 and :265-266
NOTES: Yes, it is inotify. One process-wide fd (static GFileDescriptor), one inotify watch PER DIRECTORY in the tree, enumerated with IterateDirectoryRecursively at Init (:238-252) and extended on the fly when IN_CREATE|IN_ISDIR arrives (:399). Event mapping: IN_CREATE/IN_MOVED_TO -> FCA_Added, IN_MODIFY -> FCA_Modified, IN_DELETE/IN_MOVED_FROM/IN_DELETE_SELF -> FCA_Removed (:391-451). IN_Q_OVERFLOW events are silently dropped, not turned into FCA_RescanRequired (:542-543). ENOSPC (watch limit) is recorded into a global error string and dumped with max_user_watches (:271-276, :696-701); this box currently reports max_user_watches=1048576, max_user_instances=1024, so limits are not a practical concern here.

### FTSTicker::AddTicker (named/functor form)
```
CORE_API FDelegateHandle AddTicker(const TCHAR* InName, float InDelay, TUniqueFunction<bool(float)>&& InFunction);   // using FDelegateHandle = TWeakPtr<FElement>;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Containers/Ticker.h:56 (handle typedef :32, delegate form :45)
NOTES: Prefer this named overload over AddTicker(const FTickerDelegate&, float) — the name shows up in profiling. Return true from the functor to reschedule after InDelay, false for one-shot. Note the header's own caveat at :68-76: rescheduling has timer skew, it only guarantees the delegate never fires MORE often than InDelay. FTSTicker::FDelegateHandle is a TWeakPtr, NOT the FDelegateHandle used by DirectoryWatcher — do not mix the two types up in the same class.

### FTSTicker::RemoveTicker / GetCoreTicker
```
static CORE_API void RemoveTicker(FDelegateHandle Handle);   [[nodiscard]] static CORE_API FTSTicker& GetCoreTicker();
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Containers/Ticker.h:66 and :35
NOTES: RemoveTicker is STATIC in 5.8 (call FTSTicker::RemoveTicker(Handle), not MyTicker.RemoveTicker(...)). It blocks if called mid-execution, guaranteeing the delegate is not running after it returns — safe to call from a module's ShutdownModule. The legacy non-thread-safe FTicker class no longer exists in 5.8 (no 'class FTicker' in Runtime/Core/Public); FTSTicker is the only form. GetCoreTicker's Tick is driven on the game thread by the engine loop.

### ExecuteOnGameThread
```
template<typename FunctorType> void ExecuteOnGameThread(const TCHAR* DebugName, FunctorType&& Functor);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Containers/Ticker.h:168
NOTES: One-shot game-thread marshal built on FTSTicker (AddTicker with 0.0f delay, returns false). Header comment: 'Schedules execution of the given functor on the game thread at a particular moment of a frame when interference with other systems is minimal.' Cleaner than FFunctionGraphTask for a fire-and-forget hop. Not needed for the DirectoryWatcher callback itself (already game thread) but is the right tool if a poll-based fallback ever runs on a worker.

### FTSTickerObjectBase
```
[[nodiscard]] CORE_API FTSTickerObjectBase(float InDelay = 0.0f, FTSTicker& Ticker = FTSTicker::GetCoreTicker());   virtual bool Tick(float DeltaTime) = 0;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Containers/Ticker.h:147 and :158
NOTES: RAII alternative: derive, and the handle is registered in the ctor / removed in the dtor. Good shape for an FVaCuusLiveReloadWatcher object owned by the editor module.

### UEngine::GetWorldContexts
```
const TIndirectArray<FWorldContext>& GetWorldContexts() const { return WorldList; }
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Engine/Classes/Engine/Engine.h:3608
NOTES: The correct 5.8 API for finding ALL PIE worlds (multi-client PIE creates several contexts). Reachable as GEngine->GetWorldContexts() or GEditor->GetWorldContexts(). Iterate as `for (const FWorldContext& Context : GEngine->GetWorldContexts())` — precedent SBlueprintEditorSelectedDebugObjectWidget.cpp:681, SequenceRecorder.cpp:198, ActorMode.cpp:213.

### FWorldContext (PIE-relevant members)
```
struct FWorldContext { TEnumAsByte<EWorldType::Type> WorldType; TObjectPtr<class UGameInstance> OwningGameInstance; int32 PIEInstance; bool bIsPrimaryPIEInstance; inline UWorld* World() const; };
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Engine/Classes/Engine/Engine.h:351 (struct), :357 WorldType, :411 OwningGameInstance, :418 PIEInstance, :433 bIsPrimaryPIEInstance, :478 World()
NOTES: World() returns ThisCurrentWorld and CAN be null mid-transition — always null-check (precedent does: `Context.WorldType == EWorldType::PIE && Context.World() != nullptr`). PIEInstance is INDEX_NONE for non-PIE contexts. OwningGameInstance is the direct route to a UGameInstanceSubsystem without touching UWorld at all.

### UEditorEngine::PlayWorld / GetPIEWorldContext
```
UPROPERTY() TObjectPtr<class UWorld> PlayWorld;   UNREALED_API FWorldContext* GetPIEWorldContext(int32 WorldPIEInstance = 0);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Editor/UnrealEd/Classes/Editor/EditorEngine.h:520 and :2603 (impl EditorEngine.cpp:6401-6412)
NOTES: GEditor->PlayWorld is the cheap 'are we in PIE at all' test (precedent LevelEditorSequencerIntegration.cpp:580 pairs it with GEditor->bIsSimulatingInEditor). GetPIEWorldContext(N) linearly scans WorldList for WorldType==PIE && PIEInstance==N and returns nullptr if absent. Both are single-instance views — use GetWorldContexts() when you need to broadcast a reload to every PIE client.

### EWorldType::Type
```
namespace EWorldType { enum Type { None, Game, Editor, PIE, EditorPreview, GamePreview, GameRPC, Inactive }; }
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Engine/Classes/Engine/EngineTypes.h:1368-1396
NOTES: Include Engine/EngineTypes.h (NOT EngineBaseTypes.h). Standalone-in-editor ('Play As Standalone Game') spawns a separate process whose world is EWorldType::Game — the editor watcher cannot reach it; only PIE/SIE is in scope for live reload.

### UGameInstance::GetSubsystem
```
template <typename TSubsystemClass> TSubsystemClass* GetSubsystem() const;   template <typename TSubsystemClass> static inline TSubsystemClass* GetSubsystem(const UGameInstance* GameInstance);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Engine/Classes/Engine/GameInstance.h:440 and :450
NOTES: The static overload null-checks the GameInstance for you — `UGameInstance::GetSubsystem<UVaCuusSubsystem>(Context.OwningGameInstance)` is the tightest form. Matches spec §4: 'one FVaCuusUIThread per UVaCuusSubsystem (UGameInstanceSubsystem — one per PIE instance / game instance)'. Returns nullptr if ShouldCreateSubsystem declined.

### UWorld::GetGameInstance
```
inline UGameInstance* GetGameInstance() const { return OwningGameInstance; }   template<class T> T* GetGameInstance() const;   template<class T> T* GetGameInstanceChecked() const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Engine/Classes/Engine/World.h:4386, :4393, :4400
NOTES: Equivalent to FWorldContext::OwningGameInstance. UWorld also has its own GetSubsystem<T>() at World.h:4312 plus a static null-safe UWorldSubsystem::GetSubsystem(const UWorld*) at :4331 — relevant only if the existing UVaCuusWorldSubsystem (VaCuusWorldSubsystem.h:14) is also a reload target.

### FEditorDelegates PIE lifecycle events
```
DECLARE_MULTICAST_DELEGATE_OneParam(FOnPIEEvent, const bool);  static UNREALED_API FOnPIEEvent BeginPIE; PostPIEStarted; PrePIEEnded; EndPIE; ShutdownPIE;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Editor/UnrealEd/Public/Editor.h:131 (delegate type), :278 BeginPIE, :280 PostPIEStarted, :282 PrePIEEnded, :284 EndPIE, :286 ShutdownPIE
NOTES: Bool param is bIsSimulating. Header docs: PostPIEStarted is 'Sent when a PIE session has fully started and after BeginPlay() has been called' — that is the correct hook to flush any reloads that were queued while no PIE session existed. PrePIEEnded/EndPIE is where you drop cached subsystem pointers. Not deprecated (the deprecated members in this block are EditorModeIDEnter/Exit at :268-272, unrelated).

### FPaths::ProjectContentDir / FPaths::ConvertRelativePathToFull
```
static CORE_API FString ProjectContentDir();
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Misc/Paths.h:276
NOTES: Spec §10 puts dev loose files under <Project>/Content/UI/**, so the watch root is FPaths::ProjectContentDir() / TEXT("UI"). For plugin-shipped UI use IPluginManager::Get().FindPlugin(TEXT("VaCuus"))->GetContentDir() (IPluginManager.h:393 FindPlugin, :154 GetContentDir, :140 GetBaseDir). FPaths::CreateStandardFilename (Paths.cpp:1412) is what makes FFileChangeData::Filename relative-to-root; paths outside the engine root are left absolute (bCannotBeStandardized branch at :1445-1455) — so normalize both sides before comparing.

## PATTERNS (6)

### Register the watcher in an editor module (game-thread, existence-checked, editor-gated)
```cpp
// FVaCuusEditorModule::StartupModule()
if (!IsRunningCommandlet() && GIsEditor)
{
    WatchRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("UI"));
    if (IFileManager::Get().DirectoryExists(*WatchRoot))   // Linux Register lies about this
    {
        FDirectoryWatcherModule& Module =
            FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
        if (IDirectoryWatcher* Watcher = Module.Get())
        {
            Watcher->RegisterDirectoryChangedCallback_Handle(
                WatchRoot,
                IDirectoryWatcher::FDirectoryChanged::CreateRaw(this, &FVaCuusEditorModule::OnUiFilesChanged),
                WatchHandle,
                0 /* recurse subtree, ignore dir-only changes */);
        }
    }
}
```
(precedent: FStringTableRegistry::FStringTableRegistry, StringTableRegistry.cpp:19-36 (IsRunningCommandlet/GIsEditor gate, CreateRaw, handle stored); FMaterialSourceTemplate ctor, MaterialSourceTemplate.cpp:30-48.)

### Symmetric unregister that survives module teardown order
```cpp
// FVaCuusEditorModule::ShutdownModule()
if (WatchHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("DirectoryWatcher")))
{
    FDirectoryWatcherModule& Module =
        FModuleManager::GetModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
    if (IDirectoryWatcher* Watcher = Module.Get())
    {
        Watcher->UnregisterDirectoryChangedCallback_Handle(WatchRoot, WatchHandle);
        WatchHandle.Reset();
    }
}
if (ReloadTickHandle.IsValid())
{
    FTSTicker::RemoveTicker(ReloadTickHandle);   // static in 5.8
}
```
(precedent: FStringTableRegistry::~FStringTableRegistry, StringTableRegistry.cpp:38-60 — note IsModuleLoaded + GetModuleChecked (never LoadModuleChecked during shutdown).)

### Debounce: callback only filters + marks dirty; a coalescing ticker does the work
```cpp
void FVaCuusEditorModule::OnUiFilesChanged(const TArray<FFileChangeData>& Changes)
{
    for (const FFileChangeData& Change : Changes)
    {
        if (Change.Action != FFileChangeData::FCA_Modified &&
            Change.Action != FFileChangeData::FCA_Added)   { continue; }

        const FString Full = FPaths::ConvertRelativePathToFull(Change.Filename);
        if (Full.EndsWith(TEXT(".rml")) || Full.EndsWith(TEXT(".rcss")) || Full.EndsWith(TEXT(".js")))
        {
            PendingReloads.Add(Full);   // TSet<FString> dedupes the IN_MODIFY storm
        }
    }

    if (PendingReloads.Num() > 0 && !ReloadTickHandle.IsValid())
    {
        ReloadTickHandle = FTSTicker::GetCoreTicker().AddTicker(
            TEXT("VaCuus.LiveReload"), 0.2f,
            [this](float) { FlushPendingReloads(); ReloadTickHandle.Reset(); return false; });
    }
}
```
(precedent: Filter-by-action-and-extension is verbatim FMaterialSourceTemplate (MaterialSourceTemplate.cpp:35-46) and FShaderCompilingManager (ShaderCompiler.cpp:959-969, which uses AddUnique to dedupe). The 'flag now, act on a later tick' split is SScreenShotBrowser: OnDirectoryChanged sets bReportsChanged (SScreenShotBrowser.cpp:370), Tick consumes it (:353-362, cleared :468). One-shot self-cancelling ticker shape is SMInstanceElementDetailsProxyObject.cpp:21.)

### Fan a reload out to every live PIE game instance
```cpp
void FVaCuusEditorModule::FlushPendingReloads()
{
    check(IsInGameThread());
    if (!GEditor || !GEditor->PlayWorld) { PendingReloads.Empty(); return; }  // no PIE: drop or stash

    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        if (Context.WorldType != EWorldType::PIE || Context.World() == nullptr) { continue; }

        if (UVaCuusSubsystem* Sub =
                UGameInstance::GetSubsystem<UVaCuusSubsystem>(Context.OwningGameInstance))
        {
            for (const FString& File : PendingReloads)
            {
                Sub->EnqueueReload(File);   // §4 game->UI command queue; non-blocking
            }
        }
    }
    PendingReloads.Empty();
}
```
(precedent: World-context iteration + PIE filter + null World() check: SBlueprintEditorSelectedDebugObjectWidget.cpp:681-687 and SequenceRecorder.cpp:196-208 (GetFirstPIEWorld). GEditor->PlayWorld as the cheap in-PIE test: LevelEditorSequencerIntegration.cpp:580.)

### Force a synchronous watcher flush (for automation tests that must not wait a frame)
```cpp
FDirectoryWatcherModule& Module =
    FModuleManager::Get().LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
Module.Get()->Tick(-1.0f);   // drains inotify + fires delegates inline, on this thread
```
(precedent: RescanAssets(), WorldPartitionEditorModule.cpp:715-719 — 'Force a directory watcher tick for the asset registry to get notified of the changes'. Must be on the game thread (Linux checkf).)

### Poll-based fallback if inotify is unavailable (network/overlay FS, container without inotify)
```cpp
PollHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("VaCuus.UiPoll"), 0.5f,
    [this](float) -> bool
    {
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *WatchRoot, TEXT("*.rml"), true, false);
        for (const FString& File : Files)
        {
            const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*File);
            FDateTime& Known = KnownStamps.FindOrAdd(File);
            if (Stamp != Known) { Known = Stamp; PendingReloads.Add(File); }
        }
        if (PendingReloads.Num() > 0) { FlushPendingReloads(); }
        return true;   // keep polling
    });
```
(precedent: No engine consumer polls timestamps for live reload, so this is a composed fallback, not a lifted pattern. FTSTicker::GetCoreTicker().AddTicker(Name, Delay, Fn) is the engine-standard registration (AssetEditorSubsystem.cpp:58 uses the delegate overload with a 1.f interval).)

## PITFALLS
- LINUX RETURNS TRUE FOR A NON-EXISTENT DIRECTORY. FDirectoryWatchRequestLinux::Init calls WatchDirectoryTree and returns true unconditionally (DirectoryWatchRequestLinux.cpp:114-117); inotify_add_watch failures only emit a Warning (:279). So RegisterDirectoryChangedCallback_Handle hands back a valid FDelegateHandle and live reload silently never fires. Windows returns false because CreateFile fails (DirectoryWatchRequestWindows.cpp:78-86). Always IFileManager::Get().DirectoryExists() first, and do not rely on the bool return as a Linux health check (ShaderCompiler.cpp:997 checks DirectoryWatcherHandle.IsValid() — that check is also useless on Linux).
- THE WATCHER ONLY WORKS IN THE EDITOR, BY ACCIDENT OF WHO TICKS IT. IDirectoryWatcher::Tick is called from exactly one engine site: UEditorEngine::Tick, EditorEngine.cpp:1943-1949 (with a stale `@todo: Put me into an FTSTicker` comment). Nothing ticks it in a packaged game or a -game standalone process. Registering a watch from a Runtime module is not merely wasteful, it will appear to work and never deliver a single event. Keep all watcher code in VaCuusEditor ("Type": "Editor", VaCuus.uplugin:33-37).
- FFileChangeData::Filename IS RELATIVE, NOT ABSOLUTE. The ctor runs FPaths::MakeStandardFilename (IDirectoryWatcher.h:23), producing '../../../Project/Content/UI/x.rml'. Comparing it directly against an FPaths::ProjectContentDir()-derived string, or feeding it to RmlUi as a load path, will fail. Wrap every incoming Filename in FPaths::ConvertRelativePathToFull, exactly as StringTableRegistry.cpp:169 and LandscapeImageFileCache.cpp:131 do. Extra wrinkle: paths that cannot be made relative to the engine root stay absolute (Paths.cpp:1445-1455), so on this machine a watch on /w/Unreal/VaCuus (outside /w/Unreal/VcHost) yields absolute filenames while a watch on the project content dir yields relative ones — normalize both sides, never assume one form.
- ONE EDITOR SAVE PRODUCES A BURST OF EVENTS AND THE ENGINE GIVES YOU NO DEBOUNCE. Linux maps every IN_MODIFY to a separate FCA_Modified (DirectoryWatchRequestLinux.cpp:406-411) and an editor that writes-then-renames yields FCA_Added + FCA_Modified + FCA_Removed for the temp file. There is no cooldown anywhere in DirectoryWatcher (DirectoryWatcher::FTimeLimit in FileCacheUtilities.h:83-107 is a *work budget* per tick, not a change debounce — do not mistake it for one). Reloading per-event will thrash the UI thread command queue. Dedupe into a TSet and coalesce on an FTSTicker, and expect to also skip editor temp/swap files (~, .tmp, dotfiles).
- ONE INOTIFY WATCH PER DIRECTORY, ADDED EAGERLY AT REGISTRATION. Init runs IterateDirectoryRecursively over the whole tree and calls inotify_add_watch for every subdirectory (DirectoryWatchRequestLinux.cpp:238-266). A deep Content/UI tree means hundreds of watches and a synchronous game-thread directory walk at editor startup. ENOSPC is reported only through a global error string + max_user_watches dump (:271-276, :696-701), never as a Register failure. This box is fine (max_user_watches=1048576) but CI containers commonly ship 8192.
- IN_Q_OVERFLOW IS DROPPED, NOT ESCALATED. ProcessAllINotifyChanges skips overflowed events outright (DirectoryWatchRequestLinux.cpp:542-543) and never synthesizes FCA_RescanRequired. After a bulk operation (git checkout, npm build emitting the whole UI tree) you can silently lose changes. Offer a manual 'Reload All' console command / editor button as the escape hatch; do not treat the watcher as lossless.
- THE CALLBACK IS GAME-THREAD ONLY — AND SO IS REGISTRATION. Linux checkf(IsInGameThread()) fires in Init (:79), WatchDirectoryTree (:210), UnwatchDirectoryTree (:300) and ProcessNotifications (:144). Registering from a worker or from an async module-load continuation is an assert, not a race. Conversely, do NOT add an AsyncTask(ENamedThreads::GameThread, ...) hop inside the handler 'for safety' — it only delays the reload by a frame. The single genuinely thread-safe entry point is FDirectoryWatcherModule::RegisterExternalChanges (DirectoryWatcherProxy.cpp:76-86).
- DO NOT CACHE A UVaCuusSubsystem RAW POINTER ACROSS PIE SESSIONS. The GameInstance and its subsystems are destroyed on EndPIE; a stale pointer survives into the next session as a dangling one. Re-resolve through GEngine->GetWorldContexts() on every flush, or hold TWeakObjectPtr and clear it from FEditorDelegates::EndPIE (Editor.h:284).
- GEditor->PlayWorld AND GetPIEWorldContext() SEE ONLY INSTANCE 0. GetPIEWorldContext defaults to WorldPIEInstance=0 (EditorEngine.h:2603, impl EditorEngine.cpp:6401-6412) and its own doc says 'You need to iterate the context list if you want all the pie world contexts.' With multi-client PIE (server + 2 clients) a reload sent only to PlayWorld updates one window and leaves the others stale — a confusing bug to chase. Iterate GetWorldContexts().
- FWorldContext::World() CAN BE NULL AND THE SUBSYSTEM CAN BE MISSING. Contexts exist before/after their world during PIE start/teardown; every engine precedent null-checks Context.World() (SBlueprintEditorSelectedDebugObjectWidget.cpp:683). Separately, UVaCuusSubsystem::ShouldCreateSubsystem may have declined for that world — the existing UVaCuusWorldSubsystem already returns false for non-game worlds (VaCuusWorldSubsystem.cpp:17-18) — so GetSubsystem<>() legitimately returns nullptr and must not be CastChecked/dereferenced.
- TWO DIFFERENT TYPES ARE BOTH SPELLED 'FDelegateHandle' IN THIS FEATURE. DirectoryWatcher hands back a real ::FDelegateHandle (IDirectoryWatcher.h:72) while FTSTicker::FDelegateHandle is `TWeakPtr<FElement>` (Ticker.h:32). They are not interchangeable and a mixed-up member will fail to compile in a confusing place. Also note FTSTicker::RemoveTicker is static in 5.8 — the instance-method form from older engine code will not compile.
- THERE ARE TWO VaCuus TREES ON DISK AND THEY ARE NOT THE SAME DIRECTORY. /w/Unreal/VaCuus (inode 66309:81797010, the git repo, holds docs/ and Source/ThirdParty/RmlUi) and /w/Unreal/VcHost/Plugins/VaCuus (inode 66309:100152991, the built plugin the editor actually loads) are separate copies, not a symlink or bind mount. inotify watches inodes, so a watch registered on the VcHost copy will not see edits made in the repo copy and vice versa. Decide explicitly which path the watch root resolves to (FPaths::ProjectContentDir() resolves under VcHost), and be aware this also means source edits need syncing before live reload can observe them.
