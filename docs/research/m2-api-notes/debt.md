# M2 API notes: tech-debt items (async texture, idle short-circuit, lifetime, monolithic)

## SUMMARY
(a) `RHIAsyncCreateTexture2D` is NOT deprecated in 5.8, but it is **unusable on our platform**: Vulkan and Metal both `UE_LOGF(..., Fatal, "RHIAsyncCreateTexture2D is not supported")` and `FVulkanDynamicRHI::InitInstance` hard-sets `GRHISupportsAsyncTextureCreation = false` (VulkanRHI.cpp:877). The 5.8 replacement for "build a texture with pixel data off the render thread" is `FRHICommandListBase::CreateTextureInitializer(FRHITextureCreateDesc)` → `FRHITextureInitializer::GetTexture2DSubresource(mip).Data/Stride` → `Finalize()`, recorded onto a **non-immediate `FRHICommandList`** created on a worker thread (legal because Vulkan sets `GRHISupportsMultithreadedResources = true`, VulkanRHI.cpp:455) and handed to `FRHICommandListImmediate::Get().QueueAsyncCommandListSubmit(CmdList)` on the render thread — the exact shape `FStaticMeshStreamIn` uses. Dimension probing without full decode works: `IImageWrapperModule::DetectImageFormat` → `CreateImageWrapper` → `SetCompressed` (header parse only; base class memcpy's the whole compressed buffer) → `GetWidth()/GetHeight()` (int64), with two caveats (1/2/4-bit PNG fully decodes inside `SetCompressed`; libjpeg-turbo retains a decompressor handle until the wrapper dies). `RHIUpdateTextureReference` with an implied immediate list is UE_DEPRECATED(5.7) — use `FRHICommandListBase::UpdateTextureReference` — but the whole `FRHITextureReference` mechanism is marked `@todo … when we eventually remove FRHITextureReference`, costs an `RHIThreadFence(true)` per swap, and cannot back an SRV; VaCuus doesn't need it because the replayer binds `FRHITexture*` per-draw from `TMap<Handle, FTextureRHIRef>` — swapping the map entry IS the placeholder swap.
(b) RmlUi at 0ae381e exposes **no** dirty/needs-redraw signal to an embedder: `Context::Update()` and `Context::Render()` both `return true` unconditionally, `IsLayoutDirty()`/`DirtyLayout()` are `protected` on `Element` and `private` on `ElementDocument`, and `RenderManager` has no change/version counter. The only on-demand-rendering hook is `Context::GetNextUpdateDelay()` (paired with `RequestNextUpdate`), which is a *time* hint (animations, cursor blink, scroll/slider auto-repeat), not a content-change signal — it says nothing about API-driven mutations, so it can gate re-*update* frequency but cannot by itself prove "nothing changed". Practical M2 approach: keep recording, then hash the recorded buffer (FXxHash64Builder over `Commands` **field-by-field** — `FVaCuusCommand` has 7 bytes of uninitialized padding after `Type`, so a raw `MemCrc32`/`HashBuffer` over the array will produce false "dirty") and skip the replay+publish when the hash and the resource delta are unchanged.
(c) The engine-blessed module-owned-singleton pattern is `FImageWriteQueueModule` (ImageWriteQueue.cpp:466-492): module class holds `TUniquePtr<T>`, creates it in `StartupModule`, tears it down in `PreUnloadCallback`+`ShutdownModule`, accessor is a virtual on the module interface. Ordering is safe: `FEngineLoop::Exit` runs `GEngine->PreExit()` (5065) and `AppPreExit()` (5123) — i.e. all UObject/subsystem deinit — *before* `FModuleManager::Get().UnloadModulesAtShutdown()` (5182), which unloads in reverse load order. The current `FVaCuusEngine::Get()` function-local `static` (VaCuusEngine.cpp:57-61) destructs at process static-destruction time, i.e. *after* modules are unloaded — in a modular (editor) build that means touching RmlUi symbols from an already-shut-down VaCuusRml.
(d) The game target builds with `Engine/Build/BatchFiles/Linux/Build.sh VcHost Linux Development -project=/w/Unreal/VcHost/VcHost.uproject` (Game type ⇒ `LinkType == Monolithic` by TargetRules.cs:2688). I verified end-to-end that the monolithic branch of `VaCuusRml.Build.cs` *does* fire (`RMLUI_STATIC_LIB 1` + `IS_MONOLITHIC 1` land in both VaCuusRml's and VaCuusRender's generated `Definitions.h`) — **but the game target does not currently compile**: monolithic/non-editor targets build with exceptions disabled, and `itlib/flat_map.hpp` throws. `bEnableExceptions = false` in our Build.cs is a no-op (UBT does `Result.bEnableExceptions |= Rules.bEnableExceptions` — a module can only turn exceptions ON). Adding `PublicDefinitions.Add("ITLIB_FLAT_MAP_NO_THROW=1")` fixes it: I compiled all 191 VaCuusRml relay TUs plus all 8 VaCuus and 9 VaCuusRender TUs against the real game-target response files with that define — 208/208 OK.

## APIS (25)

### FDynamicRHI::RHIAsyncCreateTexture2D (NOT deprecated, but Fatal on Vulkan/Metal)
```
virtual FTextureRHIRef RHIAsyncCreateTexture2D(uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, ERHIAccess InResourceState, void** InitialMipData, uint32 NumInitialMips, const TCHAR* DebugName, FGraphEventRef& OutCompletionEvent) = 0;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/RHI/Public/DynamicRHI.h:498 (free-function wrapper: RHICommandList.h:5345)
NOTES: NO UE_DEPRECATED on this one in 5.8 (its neighbours RHICreateTexture:5339 and RHIAsyncReallocateTexture2D:5352 ARE UE_DEPRECATED(5.8), and RHIUpdateTextureReference:5327 is UE_DEPRECATED(5.7)). DO NOT USE on our target: FVulkanDynamicRHI::RHIAsyncCreateTexture2D (VulkanTexture.cpp:1571-1575) is `UE_LOGF(LogVulkan, Fatal, "RHIAsyncCreateTexture2D is not supported")`; FMetalDynamicRHI likewise (MetalTexture.cpp:1536-1538). Real impls exist only for D3D12 (D3D12Texture.cpp:1154), D3D11 (D3D11Texture.cpp:877), OpenGL (OpenGLTexture.cpp:1479). Gated on GRHISupportsAsyncTextureCreation (RHIValidation.h:532 `check(GRHISupportsAsyncTextureCreation)`), which Vulkan hard-clears in FVulkanDynamicRHI::InitInstance (VulkanRHI.cpp:877). Also: 'OutCompletionEvent can return null; operation can still be pending after function returns'.

### GRHISupportsAsyncTextureCreation / GRHISupportsMultithreadedResources
```
#define GRHISupportsAsyncTextureCreation   GRHIGlobals.SupportsAsyncTextureCreation
#define GRHISupportsMultithreadedResources GRHIGlobals.SupportsMultithreadedResources
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/RHI/Public/RHIGlobals.h:841 and :873 (fields at :189, :291)
NOTES: On Linux Vulkan: AsyncTextureCreation = FALSE (VulkanRHI.cpp:877), MultithreadedResources = TRUE (VulkanRHI.cpp:455). So the correct M2 story is 'record resource creation onto a worker-thread FRHICommandList', not 'RHIAsyncCreateTexture2D'. Engine validation asserts `check(GRHISupportsMultithreadedResources || RHICmdList.IsImmediate())` (RHIValidation.cpp:1024).

### FRHICommandListBase::CreateTextureInitializer / CreateTexture — the 5.8 create-with-data API
```
[[nodiscard]] inline FRHITextureInitializer CreateTextureInitializer(const FRHITextureCreateDesc& CreateDesc);
inline FTextureRHIRef CreateTexture(const FRHITextureCreateDesc& CreateDesc);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/RHI/Public/RHICommandList.h:857 and :872
NOTES: CreateTexture == CreateTextureInitializer(...).Finalize(). Both are members of FRHICommandListBase, so they work on ANY command list, immediate or not. `CreateDesc.InitialState == ERHIAccess::Unknown` is auto-resolved via RHIGetDefaultResourceState. The free function `RHICreateTexture(const FRHITextureCreateDesc&)` (RHICommandList.h:5340) IS UE_DEPRECATED(5.8) — never use it.

### FRHITextureInitializer / FRHITextureSubresourceInitializer
```
struct FRHITextureInitializer {
  FRHITextureSubresourceInitializer GetSubresource(FSubresourceIndex);
  FRHITextureSubresourceInitializer GetTexture2DSubresource(int32 MipIndex);
  RHI_API FTextureRHIRef Finalize();
};
struct FRHITextureSubresourceInitializer { void WriteData(const void*, size_t); void WriteColor(FColor); void* Data; uint64 Size; uint64 Stride; };
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/RHI/Public/RHITextureInitializer.h:49 (subresource struct :28, GetTexture2DSubresource :88, Finalize :132)
NOTES: NO COPIES ALLOWED (move-only). Requires `CreateDesc.SetInitActionInitializer()` (RHIResources.h:2134) or GetSubresource checkf's. Destructor calls RemovePendingTextureUpload() if not Finalized — always Finalize or let it die deliberately. `Stride` may exceed Width*BPP: copy row-by-row (see GlobalRenderResources.cpp:448-462). Header carries `// @todo dev-pr switch to using IRHIUploadContext` (:138) — mildly in-flux but it is the current API and is what RenderCore itself uses.

### FRHITextureCreateDesc builders used by the upload path
```
static FRHITextureCreateDesc Create2D(const TCHAR* DebugName, FIntPoint Size, EPixelFormat Format);
static FRHITextureCreateDesc Create2D(const TCHAR* DebugName, int32 SizeX, int32 SizeY, EPixelFormat Format);
FRHITextureCreateDesc& SetInitActionInitializer();
FRHITextureCreateDesc& SetInitActionBulkData(FResourceBulkDataInterface* InBulkData);
FRHITextureCreateDesc& SetInitialState(ERHIAccess InInitialState);
FRHITextureCreateDesc& SetNumMips(uint8);
FRHITextureCreateDesc& SetFlags(ETextureCreateFlags);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/RHI/Public/RHIResources.h:1997, :2004, :2134, :2139, :2106, :2100
NOTES: ERHITextureInitAction enum at RHIResources.h:1953 { Default, BulkData, Initializer }. Today VaCuus does CreateTexture(Default) + UpdateTexture2D (VaCuusReplayRenderer.cpp:201-208) — a two-step that cannot leave the render thread; SetInitActionInitializer collapses it into one worker-thread-legal step.

### Worker-thread command list + async submit (the RHIAsyncCreateTexture2D replacement shape)
```
FRHICommandList(FRHIGPUMask GPUMask = FRHIGPUMask::All());
RHI_API ERHIPipeline SwitchPipeline(ERHIPipeline Pipeline);
RHI_API void FinishRecording();
struct FQueuedCommandList { FRHICommandListBase* CmdList = nullptr; };
enum class ETranslatePriority { Disabled, Normal, High };
RHI_API void QueueAsyncCommandListSubmit(TArrayView<FQueuedCommandList> CommandLists, ETranslatePriority ParallelTranslatePriority = ETranslatePriority::Disabled, int32 MinDrawsPerTranslate = 0);
inline void QueueAsyncCommandListSubmit(FQueuedCommandList QueuedCommandList, ETranslatePriority = ETranslatePriority::Disabled, int32 MinDrawsPerTranslate = 0);
```
SRC: RHICommandList.h:3610 (ctor), :742 (SwitchPipeline), :519 (FinishRecording), :4429 (FQueuedCommandList), :4440 (ETranslatePriority), :4453/:4456 (QueueAsyncCommandListSubmit)
NOTES: Precedent: FStaticMeshStreamIn — `new FRHICommandList()` + `SwitchPipeline(ERHIPipeline::Graphics)` on an async thread (StaticMeshUpdate.cpp:163-164), resources created into it, `FinishRecording()` (:198), then on the render thread `FRHICommandListImmediate::Get().QueueAsyncCommandListSubmit(StreamingRHICmdList)` (:220). Same file has `check(!StreamingRHICmdList)` in the dtor — the cmdlist must be handed off or leaked-checked. QueueAsyncCommandListSubmit MUST be called on the immediate list (render thread).

### FRHICommandListBase::CreateTextureReference / UpdateTextureReference (placeholder-swap primitive)
```
inline FTextureReferenceRHIRef CreateTextureReference(FRHITexture* InReferencedTexture = nullptr);
RHI_API void UpdateTextureReference(FRHITextureReference* TextureRef, FRHITexture* NewTexture);
```
SRC: RHICommandList.h:899 and :905; impl RHICommandList.cpp:2482 → DynamicRHI.cpp:577-591
NOTES: NON-deprecated forms. The free `RHIUpdateTextureReference(FRHITextureReference*, FRHITexture*)` at RHICommandList.h:5327 IS UE_DEPRECATED(5.7). `RHIClearTextureReference(FRHITextureReference*)` at :5339 is NOT deprecated. Costs: the base impl enqueues a lambda AND issues `RHICmdList.RHIThreadFence(true)` on every update (DynamicRHI.cpp:590) — a per-swap RHI-thread serialization point. Epic's own comment at DynamicRHI.cpp:581: `@todo dev-pr - This should be refactored out when we eventually remove FRHITextureReference`. Hard limit: you CANNOT create an SRV of a texture reference — `checkf(Texture->GetTextureReference() == nullptr, TEXT("Creating a shader resource view of an FRHITextureReference is not supported."))` (RHICommandList.h:915).

### FRHITextureReference
```
class FRHITextureReference : public FRHITexture {
  RHI_API FRHITextureReference(FRHITexture* InReferencedTexture);
  inline FRHITexture* GetReferencedTexture() const;
  static inline FRHITexture* GetDefaultTexture();
 protected: void SetReferencedTexture(FRHITexture*); TRefCountPtr<FRHITexture> ReferencedTexture;
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/RHI/Public/RHITextureReference.h:7-65
NOTES: Passing nullptr to UpdateTextureReference substitutes FRHITextureReference::GetDefaultTexture() (global black), so a 'placeholder' is free if black is acceptable (DynamicRHI.cpp:586). VaCuus almost certainly does not need this: FVaCuusReplayRenderer binds via SHADER_PARAMETER_TEXTURE(Texture2D, UITexture) per draw from `TMap<FVaCuusTextureHandle, FTextureRHIRef> Textures` (VaCuusReplayRenderer.h:88), so 'swap the placeholder' == replace the map value on the render thread. Reserve FRHITextureReference for a future UTexture/material-facing surface.

### IImageWrapperModule — format sniff + one-shot decode
```
virtual EImageFormat DetectImageFormat(const void* InCompressedData, int64 InCompressedSize) = 0;
virtual TSharedPtr<IImageWrapper> CreateImageWrapper(const EImageFormat InFormat, const TCHAR* InOptionalDebugImageName = nullptr) = 0;
virtual bool DecompressImage(const void* InCompressedData, int64 InCompressedSize, FImage& OutImage) = 0;
virtual bool CompressImage(TArray64<uint8>& OutData, EImageFormat ToFormat, const FImageView& InImage, int32 Quality = 0) = 0;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/ImageWrapper/Public/IImageWrapperModule.h:109, :99, :77, :67
NOTES: DetectImageFormat is documented as needing only 8-16 bytes. CreateImageWrapper's doc-comment says 'Deprecated. Prefer CompressImage/DecompressImage' (comment only, no UE_DEPRECATED) — but DecompressImage gives no way to learn dimensions without decoding, so the wrapper is still the right tool for the probe. Module is stateless: FImageWrapperModule (ImageWrapperModule.cpp:72-530) has no members and no locks; CreateImageWrapper just MakeShared's a fresh wrapper → safe from any thread ONCE LOADED. `FModuleManager::LoadModuleChecked` itself must run on the game thread — cache the `IImageWrapperModule&` at startup.

### IImageWrapper — probe dimensions without full decode
```
virtual bool SetCompressed(const void* InCompressedData, int64 InCompressedSize) = 0;
virtual int64 GetWidth() const = 0;
virtual int64 GetHeight() const = 0;
virtual int32 GetBitDepth() const = 0;
virtual ERGBFormat GetFormat() const = 0;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/ImageWrapper/Public/IImageWrapper.h:151, :368, :376, :386, :394
NOTES: Header doc at :147 confirms the contract: 'after SetCompressed, image info queries like GetWidth and GetBitDepth are allowed ... decompression is not done until GetRaw'. COST of SetCompressed = (1) unconditional full memcpy of the compressed buffer into `CompressedData` (FImageWrapperBase::SetCompressed, ImageWrapperBase.cpp:104-122, whose own comment calls it 'usually an unnecessary allocation and copy'), plus (2) a header parse: PNG → LoadPNGHeader (PngImageWrapper.cpp:566, libpng png_read_info), JPEG → tjDecompressHeader3 (JpegImageWrapper.cpp:336). Note GetWidth/GetHeight return int64, not int32.

### IImageWrapper::GetRaw — which overload to use
```
bool GetRaw(TArray64<uint8>& OutRawData);                                   // PREFERRED (native format/depth)
bool GetRawImage(FImage& OutImage);                                          // RECOMMENDED
virtual bool GetRaw(const ERGBFormat InFormat, int32 InBitDepth, TArray64<uint8>& OutRawData) = 0;   // legacy
bool GetRaw(const ERGBFormat InFormat, int32 InBitDepth, TArray<uint8>& OutRawData);                 // legacy 32-bit
```
SRC: IImageWrapper.h:213, :262, :291, :318
NOTES: M1 currently calls the 4th form: `ImageWrapper->GetRaw(ERGBFormat::RGBA, 8, RawRGBA)` (VaCuusRecordingRenderInterface.cpp:131). Its own doc says 'this is often broken, should only be used with InFormat == GetFormat()' and 'DEPRECATED, use GetRaw() with 1 argument or GetRawImage()'. No UE_DEPRECATED macro, so it compiles clean — flag as debt, not a build break. Note JPEG-turbo reports GetFormat()==ERGBFormat::BGRA (JpegImageWrapper.cpp:353) while PNG can report RGBA/BGRA/Gray, so switching to the native-format GetRaw means VaCuus must handle channel order at premultiply time.

### Rml::Context::Update / Render — return values are meaningless
```
bool Update();
bool Render();
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Include/RmlUi/Core/Context.h:58, :60 (impl Source/Core/Context.cpp:181-220 and :222-242)
NOTES: Both bodies end in an unconditional `return true;`. There is NO 'nothing changed' path, no early-out, and Render() unconditionally walks the whole element tree (`root->Render()`) and calls render_manager->PrepareRender/ResetState. Context::Update() also unconditionally calls doc->UpdateLayout()/UpdatePosition() for every document (Context.cpp:207-214), which clears layout_dirty — so polling layout state after Update is always false.

### Rml::Context::RequestNextUpdate / GetNextUpdateDelay — the only on-demand hook that exists
```
void RequestNextUpdate(double delay);
double GetNextUpdateDelay() const;
```
SRC: Include/RmlUi/Core/Context.h:288, :294 (impl Source/Core/Context.cpp:1626-1635; member `double next_update_timeout` Context.h:391)
NOTES: Semantics per the header: 'The returned value can be infinity, in which case Update() should be invoked after user input was received. A value of 0 means render as fast as possible'. `next_update_timeout` is RESET TO INFINITY at the top of Context::Update() (Context.cpp:186), so it must be read AFTER Update() and it only reflects requests made during that Update. Complete list of callers at this SHA: Context.cpp:189 (scroll controller active → 0), Element.cpp:148-153 (element has running animations AND IsVisible → 0), Elements/WidgetTextInput.cpp:448 (caret blink), WidgetScroll.cpp:142 and WidgetSlider.cpp:127 (arrow auto-repeat), Debugger/ElementInfo.cpp:91. It says NOTHING about SetInnerRML / SetProperty / data-model changes, so infinity does NOT mean 'the frame is identical to the last one'.

### Rml layout-dirty state is not embedder-reachable
```
protected: virtual void DirtyLayout(); virtual bool IsLayoutDirty();            // Rml::Element
private:   void DirtyLayout() override; bool IsLayoutDirty() override;            // Rml::ElementDocument
```
SRC: Include/RmlUi/Core/Element.h:635/:637 (inside the `protected:` block that starts at :593) and Include/RmlUi/Core/ElementDocument.h:143/:146 (inside the `private:` block that starts at :135)
NOTES: Not callable from VaCuus without patching the vendored tree. Even if made public it is useless as an idle signal — Context::Update clears it every frame (ElementDocument::UpdateLayout, ElementDocument.cpp:474-493).

### Rml::RenderManager — no change counter
```
class RMLUICORE_API RenderManager : NonCopyMoveable { ... private: StableVector<GeometryData> geometry_list; UniquePtr<TextureDatabase> texture_database; int compiled_filter_count; int compiled_shader_count; RenderState state; Vector2i viewport_dimensions; Vector<LayerHandle> render_stack; };
```
SRC: Include/RmlUi/Core/RenderManager.h:45-126
NOTES: compiled_filter_count / compiled_shader_count are resource-lifetime counters, not frame-change counters, and are private with only `friend class RenderManagerAccess`. There is no version/generation/dirty field anywhere in the class. Conclusion: no upstream signal to reuse; M2 must compute its own.

### FXxHash64 / FXxHash64Builder (recommended for command-buffer hashing)
```
[[nodiscard]] CORE_API static FXxHash64 FXxHash64::HashBuffer(const void* Data, uint64 Size);
class FXxHash64Builder { CORE_API void Reset(); CORE_API void Update(const void* Data, uint64 Size); [[nodiscard]] CORE_API FXxHash64 Finalize() const; };
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Hash/xxhash.h:30, :204-222
NOTES: Alternative: `FCrc::MemCrc32(const void* Data, int32 Length, uint32 CRC = 0)` (Misc/Crc.h:29) — 32-bit and int32-limited, prefer xxhash. FXxHash64Builder holds `alignas(64) char StateBytes[576]` — cheap to stack-allocate per frame, do not make it a member with an eye to cache locality. Both APIs hash raw bytes, which is the padding hazard for FVaCuusCommand (see pitfalls).

### IModuleInterface lifecycle hooks
```
virtual void StartupModule();
virtual void PreUnloadCallback();
virtual void PostLoadCallback();
virtual void ShutdownModule();
virtual bool SupportsDynamicReloading();
virtual bool SupportsAutomaticShutdown();
virtual bool IsGameModule() const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Modules/ModuleInterface.h:49, :59, :68, :79, :88, :98, :108
NOTES: UnloadModulesAtShutdown skips modules whose SupportsAutomaticShutdown() is false (ModuleManager.cpp:1472) and calls PreUnloadCallback on ALL modules before any ShutdownModule (:1481-1482), sorted by descending LoadOrder (:1456-1459) — i.e. last loaded shuts down first. VaCuus (LoadingPhase Default) loads after VaCuusRender (PostConfigInit), so VaCuus::ShutdownModule runs BEFORE VaCuusRender's.

### Engine shutdown ordering: subsystems die before modules
```
GEngine->PreExit();                              // LaunchEngineLoop.cpp:5065
AppPreExit();                                    // :5123  (FCoreDelegates::OnPreExit / OnExit broadcast)
FModuleManager::Get().UnloadModulesAtShutdown(); // :5182
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp:5065, :5123, :5181-5182; FEngineLoop::AppPreExit body at :6923; FModuleManager::UnloadModulesAtShutdown at Core/Private/Modules/ModuleManager.cpp:1438
NOTES: THE ordering fact for (c): all UObject subsystem Deinitialize()s (via GEngine->PreExit and world teardown) complete BEFORE any ShutdownModule() runs. A module-owned FVaCuusEngine is therefore guaranteed alive for the whole life of every UVaCuusSubsystem — which a function-local static also happens to satisfy, but the static then survives *past* UnloadModulesAtShutdown and destructs during C++ static destruction, after VaCuusRml's module was shut down. Also note FSlateApplication::Shutdown() at :5094 and ShutdownRenderingThread() at :5136 both precede UnloadModulesAtShutdown — so ShutdownModule cannot touch Slate or enqueue render commands.

### Engine precedent: module-owned singleton (FImageWriteQueueModule)
```
class FImageWriteQueueModule : public IImageWriteQueueModule {
  virtual void StartupModule() override      { Queue = MakeUnique<FImageWriteQueue>(); }
  virtual void PreUnloadCallback() override  { Queue->BeginShutdown(); }
  virtual void ShutdownModule() override     { Queue->BeginShutdown(); Queue.Reset(); }
  virtual IImageWriteQueue& GetWriteQueue() override { return *Queue; }
  TUniquePtr<FImageWriteQueue> Queue;
};
IMPLEMENT_MODULE(FImageWriteQueueModule, ImageWriteQueue)
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/ImageWriteQueue/Private/ImageWriteQueue.cpp:466-492 (public interface at Public/ImageWriteQueue.h:78-90)
NOTES: Header comment at ImageWriteQueue.h:76-78 states the rationale verbatim: 'Access is only via the module interface to ensure that the queue is flushed correctly on shutdown'. Note it does the shutdown work TWICE (PreUnloadCallback and ShutdownModule) because PreUnloadCallback fires for all modules before any teardown — the right place to stop accepting new work while peers are still alive.

### Current VaCuus module + singleton shape (what to change)
```
class FVaCuusModule : public IModuleInterface { static FVaCuusModule& Get() { return FModuleManager::LoadModuleChecked<FVaCuusModule>("VaCuus"); } };
FVaCuusEngine& FVaCuusEngine::Get() { static FVaCuusEngine Instance; return Instance; }
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuus/Public/VaCuus.h:16-19 and /w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuus/Private/VaCuusEngine.cpp:57-61
NOTES: FVaCuusModule already has the correct LoadModuleChecked accessor and an empty Startup/Shutdown (VaCuus.cpp:11-19) — the change is mechanical: move `TUniquePtr<FVaCuusEngine> Engine` into FVaCuusModule, make FVaCuusEngine's ctor/dtor private with `friend class FVaCuusModule`, and reroute FVaCuusEngine::Get() to `FVaCuusModule::Get().GetEngine()`. FVaCuusEngine already holds TUniquePtr<FVaCuusSystemInterface/FileInterface/NullRenderInterface> whose destruction currently happens at static-destruction time (VaCuusEngine.h:57-59).

### TargetRules.LinkType — why the Game target is monolithic
```
public TargetLinkType LinkType {
  get => (LinkTypePrivate != TargetLinkType.Default) ? LinkTypePrivate : ((Type == TargetType.Editor) ? TargetLinkType.Modular : TargetLinkType.Monolithic);
}
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Programs/UnrealBuildTool/Configuration/Rules/TargetRules.cs:2686-2690; forwarded read-only at Rules/ReadOnlyTargetRules.cs:540 (`public TargetLinkType LinkType => Inner.LinkType;`)
NOTES: VcHostTarget sets `Type = TargetType.Game` and never touches LinkType (/w/Unreal/VcHost/Source/VcHost.Target.cs:10), so `Target.LinkType == TargetLinkType.Monolithic` in VaCuusRml.Build.cs:23. VERIFIED empirically: /w/Unreal/VcHost/Intermediate/Build/Linux/x64/VcHostGCD/Development/VaCuusRml/Definitions.h:19 `#define IS_MONOLITHIC 1`, :97 `#define RMLUI_STATIC_LIB 1`, :121 `#define VACUUSRML_API ` (empty). Public propagation works: VaCuusRender/Definitions.h:282 also has RMLUI_STATIC_LIB 1. Editor build for contrast: Plugins/VaCuus/Intermediate/Build/Linux/x64/UnrealEditor/Development/VaCuusRml/Definitions.h:19 IS_MONOLITHIC 0, :96 RMLUI_CORE_EXPORTS 1.

### UBT exceptions rules — the real monolithic blocker
```
// target-global:
GlobalCompileEnvironment.bEnableExceptions = Rules.bForceEnableExceptions || (Rules.bCompileAgainstEditor && !Rules.bUseAutoRTFMCompiler);
// per-module:
Result.bEnableExceptions |= Rules.bEnableExceptions;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Programs/UnrealBuildTool/Configuration/UEBuildTarget.cs:6179 and Configuration/UEBuildModuleCPP.cs:2666 (TargetRules.bForceEnableExceptions at Rules/TargetRules.cs:1441)
NOTES: The `|=` means a module can only turn exceptions ON. `bEnableExceptions = false;` in VaCuusRml.Build.cs:14 is dead code. Editor target ⇒ bCompileAgainstEditor ⇒ global -fexceptions (confirmed: UnrealEditor/Development/VaCuusRml/*.rsp line 69 is `-fexceptions`). Game target ⇒ no -fexceptions, PLATFORM_EXCEPTIONS_DISABLED=1 (VcHostGCD rsp line 68) ⇒ RmlUi's bundled itlib fails to compile.

### itlib flat_map throw sites + the escape hatch
```
#if !defined(ITLIB_FLAT_MAP_NO_THROW)
#   define I_ITLIB_THROW_FLAT_MAP_OUT_OF_RANGE() throw std::out_of_range("itlib::flat_map out of range")
#else
#   define I_ITLIB_THROW_FLAT_MAP_OUT_OF_RANGE() assert(false && "itlib::flat_map out of range")
#endif
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Include/RmlUi/Core/Containers/itlib/flat_map.hpp:99-105; the two call sites that fail are :340 and :351 (both inside flat_map::at)
NOTES: These are the ONLY throw sites in the whole vendored Core tree that the game target hits. The macro must be defined for every TU that includes the header, and the header lives under Include/ so consumers (VaCuus, VaCuusRender) see it too ⇒ use PublicDefinitions, not PrivateDefinitions. Verified fix: 191/191 VaCuusRml relay TUs + 8/8 VaCuus TUs + 9/9 VaCuusRender TUs compile clean against the real game-target .rsp files with `-DITLIB_FLAT_MAP_NO_THROW=1` appended, and fail with 2 errors without it.

### Game-target build command (Linux)
```
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VcHost Linux Development -project=/w/Unreal/VcHost/VcHost.uproject
```
SRC: /w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh (forwards "$@" verbatim to dotnet Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll); target defined at /w/Unreal/VcHost/Source/VcHost.Target.cs
NOTES: Build.sh cd's to the engine root first, so -project= MUST be an absolute path. Target name is `VcHost` (no suffix) vs the editor's `VcHostEditor`. Configurations: Debug|DebugGame|Development|Shipping|Test. Optional flags: -buildubt (rebuild UBT first), -waitmutex, -NoHotReload. Output binary lands in /w/Unreal/VcHost/Binaries/Linux/VcHost. VaCuusEditor is correctly excluded (no VaCuusEditor dir appears under the game target's intermediate tree). Confirmed for this target: WITH_EDITOR 0, WITH_DEV_AUTOMATION_TESTS 1 in Development (so VaCuusBootTest/VaCuusFileInterfaceTest/VaCuusRecorderTest still compile; they'd vanish in Shipping).

### UE::Tasks::Launch (worker for async decode)
```
template<typename TaskBodyType>
TTask<TInvokeResult_T<TaskBodyType>> Launch(const TCHAR* DebugName, TaskBodyType&& TaskBody, ETaskPriority Priority = ETaskPriority::Normal, EExtendedTaskPriority ExtendedPriority = EExtendedTaskPriority::None, ETaskFlags Flags = ETaskFlags::None);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Tasks/Task.h:298-310 (prerequisite overload at :323-336)
NOTES: Non-deprecated 5.8 task API; prefer over FFunctionGraphTask/AsyncTask for the decode job. The prerequisite overload is the clean way to chain 'decode task → RHI cmdlist submit'.

## PATTERNS (7)

### Off-render-thread texture creation with initial data (the RHIAsyncCreateTexture2D replacement that actually works on Vulkan)
```cpp
// --- worker thread (decode task) ---
FRHICommandList* UploadCmdList = new FRHICommandList();
UploadCmdList->SwitchPipeline(ERHIPipeline::Graphics);

const FRHITextureCreateDesc Desc =
    FRHITextureCreateDesc::Create2D(TEXT("VaCuusUITexture"), Size, PF_R8G8B8A8)
        .SetFlags(ETextureCreateFlags::ShaderResource)
        .SetInitialState(ERHIAccess::SRVMask)
        .SetInitActionInitializer();

FRHITextureInitializer Init = UploadCmdList->CreateTextureInitializer(Desc);
{
    const FRHITextureSubresourceInitializer Sub = Init.GetTexture2DSubresource(0);
    const uint64 SrcPitch = uint64(Size.X) * 4;
    for (int32 Y = 0; Y < Size.Y; ++Y)  // Sub.Stride may exceed SrcPitch
    {
        FMemory::Memcpy((uint8*)Sub.Data + Y * Sub.Stride, Pixels + Y * SrcPitch, SrcPitch);
    }
}
FTextureRHIRef Texture = Init.Finalize();
UploadCmdList->FinishRecording();

// --- render thread ---
FRHICommandListImmediate::Get().QueueAsyncCommandListSubmit(UploadCmdList);
```
(precedent: FStaticMeshStreamIn::CreateBuffers/DoFinishUpdate — /w/Unreal/UnrealEngine/Engine/Source/Runtime/Engine/Private/Streaming/StaticMeshUpdate.cpp:163-164, :198, :220. Row-by-row initializer fill: /w/Unreal/UnrealEngine/Engine/Source/Runtime/RenderCore/Private/GlobalRenderResources.cpp:440-466.)

### Probe image dimensions without decoding (for Rml::RenderInterface::LoadTexture, which must return dims synchronously)
```cpp
// Cache this reference at module startup — LoadModuleChecked is game-thread only.
IImageWrapperModule& IW = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");

const EImageFormat Fmt = IW.DetectImageFormat(FileData.data(), FileData.size()); // ~16 bytes
if (Fmt == EImageFormat::Invalid) { return Rml::TextureHandle(0); }

TSharedPtr<IImageWrapper> Probe = IW.CreateImageWrapper(Fmt);
if (!Probe.IsValid() || !Probe->SetCompressed(FileData.data(), FileData.size()))
{
    return Rml::TextureHandle(0);  // header parse only (see pitfalls for the 1/2/4-bit PNG exception)
}
OutDimensions = Rml::Vector2i(int(Probe->GetWidth()), int(Probe->GetHeight())); // int64 -> int
Probe.Reset();   // MUST drop it: turbo holds a live tjInitDecompress handle until dtor

// hand FileData (moved) to a worker; Probe is NOT reused, decode re-parses there
```
(precedent: Current synchronous version at /w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuusRender/Private/VaCuusRecordingRenderInterface.cpp:120-138. Contract documented at IImageWrapper.h:145-151.)

### Async decode on a worker, publish through the existing command-buffer resource delta
```cpp
UE::Tasks::Launch(UE_SOURCE_LOCATION,
    [Bytes = MoveTemp(FileData), Handle, &IW, Sink = TWeakPtr<FVaCuusTextureSink>(SinkPtr)]
    {
        FImage Decoded;
        if (!IW.DecompressImage(Bytes.GetData(), Bytes.Num(), Decoded)) { return; }
        // premultiply here (same loop as VaCuusRecordingRenderInterface.cpp:145-160)
        if (TSharedPtr<FVaCuusTextureSink> S = Sink.Pin())
        {
            S->PushDecoded(Handle, MoveTemp(Decoded)); // lock-free queue -> next frame's NewTextures
        }
    });
```
(precedent: UE::Tasks::Launch — Core/Public/Tasks/Task.h:298. Resource-delta plumbing already exists: FVaCuusCommandBuffer::NewTextures / ReleasedTextures (/w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuusRender/Public/VaCuusCommandBuffer.h:96-102) and FVaCuusReplayRenderer::UploadNewResources (VaCuusReplayRenderer.cpp:190-211).)

### Placeholder swap WITHOUT FRHITextureReference (what VaCuus should actually do)
```cpp
// render thread — the shader parameter is set per draw from this map,
// so replacing the value IS the swap. No RHIThreadFence, no SRV restriction.
void FVaCuusReplayRenderer::SwapTexture_RenderThread(FVaCuusTextureHandle H, FTextureRHIRef New)
{
    check(IsInRenderingThread());
    if (FTextureRHIRef* Slot = Textures.Find(H)) { *Slot = MoveTemp(New); }
}

// only if a UTexture/material ever needs to observe the swap:
// FTextureReferenceRHIRef Ref = RHICmdList.CreateTextureReference(Placeholder);
// RHICmdList.UpdateTextureReference(Ref, Real);   // costs RHIThreadFence(true)
```
(precedent: Binding site: SHADER_PARAMETER_TEXTURE(Texture2D, UITexture) in /w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuusRender/Private/VaCuusUIShaders.h:21; storage TMap<FVaCuusTextureHandle, FTextureRHIRef> Textures in VaCuusReplayRenderer.h:88. Reference API: RHICommandList.h:899/:905; cost + '@todo remove' at DynamicRHI.cpp:577-591.)

### Idle short-circuit: hash the recorded buffer field-by-field (raw memory hashing is UNSOUND here)
```cpp
static uint64 HashBuffer(const FVaCuusCommandBuffer& B)
{
    FXxHash64Builder H;
    H.Update(&B.ViewSize, sizeof(B.ViewSize));
    for (const FVaCuusCommand& C : B.Commands)   // NOT H.Update(Commands.GetData(), ...)
    {                                            // -> 7 padding bytes after `Type`
        H.Update(&C.Type,        sizeof(C.Type));
        H.Update(&C.Geometry,    sizeof(C.Geometry));
        H.Update(&C.Texture,     sizeof(C.Texture));
        H.Update(&C.Translation, sizeof(C.Translation));
        H.Update(&C.Scissor,     sizeof(C.Scissor));
        H.Update(&C.Transform,   sizeof(C.Transform));
    }
    return H.Finalize().Hash;
}
// skip publish only when: hash unchanged AND NewGeometry/NewTextures/Released* all empty
// AND Context::GetNextUpdateDelay() (read AFTER Update()) is > 0.
```
(precedent: FXxHash64Builder — Core/Public/Hash/xxhash.h:204-222. Struct layout: FVaCuusCommand at VaCuusCommandBuffer.h:26-44 (EVaCuusCommandType uint8 at offset 0, uint64 Geometry at offset 8). Hook point: the M1 tick at /w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuusRender/Private/VaCuusM1Harness.cpp:110-126, between Recorder->EndFrameAndPublish() and ENQUEUE_RENDER_COMMAND.)

### Module-owned FVaCuusEngine
```cpp
class FVaCuusModule : public IModuleInterface
{
public:
    virtual void StartupModule() override { Engine = TUniquePtr<FVaCuusEngine>(new FVaCuusEngine()); }
    virtual void PreUnloadCallback() override { if (Engine) { Engine->ForceShutdown(); } }
    virtual void ShutdownModule() override { Engine.Reset(); }

    FVaCuusEngine& GetEngine() { check(Engine); return *Engine; }
    static FVaCuusModule& Get() { return FModuleManager::LoadModuleChecked<FVaCuusModule>("VaCuus"); }
private:
    TUniquePtr<FVaCuusEngine> Engine;
};
// FVaCuusEngine::Get() -> return FVaCuusModule::Get().GetEngine();
// ctor/dtor stay private, add `friend class FVaCuusModule;`
```
(precedent: FImageWriteQueueModule — /w/Unreal/UnrealEngine/Engine/Source/Runtime/ImageWriteQueue/Private/ImageWriteQueue.cpp:466-492, with the 'access only via the module interface so shutdown is correct' rationale at Public/ImageWriteQueue.h:76-78. Existing accessor to keep: /w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuus/Public/VaCuus.h:16-19.)

### VaCuusRml.Build.cs monolithic fix
```cpp
// unconditional — the header lives under Include/ so consumers see it too
PublicDefinitions.Add("ITLIB_FLAT_MAP_NO_THROW=1");

// delete this line: it is a no-op (UBT does `Result.bEnableExceptions |= Rules.bEnableExceptions`)
// bEnableExceptions = false;

if (Target.LinkType == TargetLinkType.Monolithic)
{
    PublicDefinitions.Add("RMLUI_STATIC_LIB=1");   // verified present in the game target
}
```
(precedent: Current file /w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuusRml/VaCuusRml.Build.cs:14 (bEnableExceptions) and :23-25 (monolithic branch). UBT rule at UEBuildModuleCPP.cs:2666. Macro at ThirdParty/RmlUi/Include/RmlUi/Core/Containers/itlib/flat_map.hpp:99-105.)

## PITFALLS
- RHIAsyncCreateTexture2D is a trap on our platform: it is not deprecated and it compiles fine, but FVulkanDynamicRHI::RHIAsyncCreateTexture2D is `UE_LOGF(LogVulkan, Fatal, ...)` (VulkanTexture.cpp:1573) and GRHISupportsAsyncTextureCreation is hard-set false in FVulkanDynamicRHI::InitInstance (VulkanRHI.cpp:877). Metal is identical (MetalTexture.cpp:1538). Any plan text that says 'use RHIAsyncCreateTexture2D' produces a Linux crash, not a compile error.
- FImageWrapperBase::SetCompressed unconditionally memcpy's the ENTIRE compressed file into an internal TArray64 before parsing the header (ImageWrapperBase.cpp:114-116; the source comment itself calls it 'usually an unnecessary allocation and copy'). A 'cheap dimension probe' is therefore O(filesize) in allocation+copy, not O(header). For a large atlas PNG that is a real game-thread cost — budget it, or do the probe on the worker and stall the Rml LoadTexture behind a size cache.
- PNG SetCompressed is NOT always header-only: for BitDepth 1/2/4 with no alpha channel (palette or grayscale PNGs) FPngImageWrapper::SetCompressed calls UncompressPNGData() inline (PngImageWrapper.cpp:309-331) — a FULL decode on whatever thread called the probe. Either forbid sub-8-bit PNGs in content, or always run the probe off the game thread.
- FJpegImageWrapper::SetCompressedTurbo allocates a libjpeg-turbo decompressor and explicitly retains it ('Decompressor is retained until Uncompress', JpegImageWrapper.cpp:362). A probe-only wrapper leaks that handle until the TSharedPtr dies, so the probe must Reset() the wrapper (dtor tjDestroy at JpegImageWrapper.cpp:92-108) rather than caching it.
- Only the immediate command list may call QueueAsyncCommandListSubmit, and the worker-created FRHICommandList must have FinishRecording() called before hand-off; FStaticMeshStreamIn's destructor asserts `check(!StreamingRHICmdList)` (StaticMeshUpdate.cpp:154) precisely because a dropped cmdlist is a silent leak on the cancel path. VaCuus's decode-cancelled path needs the same guard.
- FRHITextureReference is on Epic's removal list — DynamicRHI.cpp:581 says '@todo dev-pr - This should be refactored out when we eventually remove FRHITextureReference' — and each UpdateTextureReference issues `RHICmdList.RHIThreadFence(true)` (DynamicRHI.cpp:590). It also cannot back an SRV: `checkf(Texture->GetTextureReference() == nullptr, ...)` (RHICommandList.h:915). Do not architect the M2 placeholder swap on it; swap the FTextureRHIRef in FVaCuusReplayRenderer::Textures instead.
- The free-function `RHIUpdateTextureReference(FRHITextureReference*, FRHITexture*)` is UE_DEPRECATED(5.7) and `RHICreateTexture(const FRHITextureCreateDesc&)` / `RHIAsyncReallocateTexture2D` are UE_DEPRECATED(5.8) (RHICommandList.h:5327, :5339, :5352). Use the FRHICommandListBase members, never the implied-immediate globals.
- RmlUi at 0ae381e gives the embedder NO dirty signal. `Context::Update()`/`Context::Render()` return an unconditional `true` (Context.cpp:219, :241); `IsLayoutDirty()` is protected on Element / private on ElementDocument; RenderManager has no version counter. Any plan that says 'ask RmlUi whether anything changed' is unimplementable without patching the vendor tree.
- GetNextUpdateDelay() is a *timer*, not a change flag. It is reset to +infinity at the top of every Update() (Context.cpp:186) so it must be read AFTER Update(), and its only sources are running animations, caret blink, scroll/slider auto-repeat and the scroll controller. It returns infinity for a document that was just mutated via SetInnerRML or a data-model write, so it can never be the sole gate for skipping Render().
- Do NOT hash FVaCuusCommand with a single MemCrc32/FXxHash64::HashBuffer over Commands.GetData(): `EVaCuusCommandType Type` (uint8) at offset 0 is followed by 7 bytes of padding before `uint64 Geometry`, and TArray does not zero-initialize padding. That yields nondeterministic hashes and spurious 'dirty' frames — hash member-by-member (or memzero each command on construction, which costs more than it saves).
- Hashing alone is insufficient for the idle short-circuit: the same command list can still carry a non-empty resource delta. Gate on hash-unchanged AND NewGeometry/NewTextures/ReleasedGeometry/ReleasedTextures all empty; otherwise the replayer never sees the create/release traffic (FVaCuusReplayRenderer::ConsumeResources exists exactly for the buffers you skip drawing — VaCuusReplayRenderer.cpp:79-92).
- `FVaCuusEngine::Get()`'s function-local `static FVaCuusEngine Instance` (VaCuusEngine.cpp:57-61) destructs during C++ static destruction, which is AFTER FModuleManager::UnloadModulesAtShutdown (LaunchEngineLoop.cpp:5182). In a modular editor build that means its TUniquePtr members can outlive the VaCuusRml module they call into. UObject subsystems are NOT the risk (GEngine->PreExit at :5065 runs first) — module-unload ordering is.
- Modules shut down in REVERSE load order (ModuleManager.cpp:1456-1459). VaCuusRender is LoadingPhase PostConfigInit and VaCuus is Default, so VaCuus::ShutdownModule runs BEFORE VaCuusRender's — a module-owned FVaCuusEngine must therefore be safe to tear down while the render module is still alive, and must not assume the reverse.
- ShutdownModule cannot touch Slate or the rendering thread: FSlateApplication::Shutdown() (LaunchEngineLoop.cpp:5094) and ShutdownRenderingThread() (:5136) both run before UnloadModulesAtShutdown (:5182). Any render-side handle flush described in spec §4's shutdown order must happen at PreUnloadCallback time or earlier (e.g. on FCoreDelegates::OnPreExit), not in ShutdownModule.
- THE game-target blocker: `bEnableExceptions = false` in VaCuusRml.Build.cs:14 is a no-op because UBT does `Result.bEnableExceptions |= Rules.bEnableExceptions` (UEBuildModuleCPP.cs:2666). Editor targets get -fexceptions globally (UEBuildTarget.cs:6179 keys off bCompileAgainstEditor), which is the ONLY reason RmlUi compiles today. A Game/Client/Server target has exceptions off and RmlUi's bundled itlib/flat_map.hpp:340,351 fails with 'cannot use throw with exceptions disabled'. This will not surface in any editor build or editor automation run.
- ITLIB_FLAT_MAP_NO_THROW must be a PublicDefinition, not Private: flat_map.hpp lives under ThirdParty/RmlUi/Include/ which is a PublicIncludePath, so VaCuus and VaCuusRender TUs that include RmlUi headers need the same define or they get an ODR-ish mismatch on flat_map::at.
- Verification residue in the workspace: /w/Unreal/VcHost/Intermediate/Build/Linux/x64/VcHostGCD/ (~140 MB of game-target .rsp/.o files from a -Mode=GenerateClangDatabase run plus my direct clang invocations) is safe to delete. I also generated and then removed /w/Unreal/UnrealEngine/compile_commands.json — if anything relied on that file it needs regenerating for the editor target.
