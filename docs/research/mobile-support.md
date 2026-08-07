# Mobile (Android + iOS) — what it actually costs

Research only: nothing in the plugin was changed to produce this note. Four parallel passes over
UE 5.8.1 — renderer/shaders, threading+JS+memory+lifecycle, input+text, build+packaging+claims.

All paths relative to `/w/Unreal/VcHost/Plugins/VaCuus/` unless rooted. Engine =
`/w/Unreal/UnrealEngine/`. Every cited line was opened; `[inferred]` marks reasoned-not-read.

**Bound on the evidence.** Every claim about what the *engine* does is read from source. Nothing
here is a compile result: this box has no Android NDK (`NDKROOT`/`ANDROID_HOME` unset), no Apple
SDK, and no device or simulator. `Engine/Platforms/Android` and `Engine/Platforms/IOS` in this
install hold only ACL/RigLogic stubs — but Android and Apple support lives in the *main* tree
(`Runtime/Core/{Android,Apple,IOS}`, `OpenGLDrv/Private/Android`, `VulkanRHI`, `Apple/MetalRHI`,
`RHI/Private/{Android,Apple}`, `Engine/Config/{Android,IOS,Apple}`, the UBT platform folders) and
all of it was readable. One pass called that BLOCKED; two called it fine. **Picked: not blocked —
bounded.** The block is toolchains and devices, not source. Also note
`Engine/Config/IOS/DataDrivenPlatformInfo.ini:22` sets `Linux:bIsEnabled=false`: iOS cannot be
targeted from this host at all, so every iOS experiment belongs on the Mac.

---

## 0. STANDING BUG — reproduces on Linux today, no device, no mobile build

> **The world panel's first touch latches input for the whole application, permanently.**
>
> `FVaCuusWorldInputProcessor::HandleMouseButtonUpEvent` clears its latch only when
> `MouseEvent.GetPressedButtons().IsEmpty()` (`Source/VaCuusRender/Private/VaCuusWorldInputProcessor.cpp:518`).
> **That predicate can never be true for a touch event.** `FSlateApplication::OnTouchEnded` builds
> the event with `bPressLeftMouseButton = true` (`SlateApplication.cpp:6832-6845`), and the touch
> constructor bakes `PressedButtons = FTouchKeySet::StandardSet` into the event's value copy
> (`Events.h:938`, `Events.cpp:15`) — so the set is never empty, whatever the application's live
> button state is. The comment at `:509-517` that justifies the predicate cites
> `SlateApplication.cpp:6100-6103`, which is about the *application's* live set and not the event's
> copy: the load-bearing justification is wrong, which is why the predicate looks right.
>
> The processor holds no Slate capture by design, so no engine safety net fires. Every later
> pointer event in the application is consumed. There is no recovery short of the panel dying
> (`ClearLatch` at `:390, :446, :601`, all death paths; `Tick` is empty,
> `VaCuusWorldInputProcessor.h:220`; `IInputProcessor` has no touch or capture-lost hook at all,
> `IInputProcessor.h:26-50`).
>
> **This is not a mobile-only bug and it does not need a device.** `FSlateApplication` converts
> every left click into a real touch event under `-faketouches` (`SlateApplication.cpp:900,
> 5261-5264, 6092-6095`), so it reproduces on a desktop Linux box right now. The suite has never
> seen a touch event: every synthesized `FPointerEvent` under `Tests/` uses `CursorPointerIndex`.
>
> **Not fixed here on purpose** — current mobile scope is "does it build and run", and this is
> filed as its own item. Fix and proof are specified in §3.1. Do not let it fall off the list
> because mobile is deferred: *the bug is already shipping on the supported platforms' touch path.*

---

## 1. The answer to the hypothesis

> *"apart from mipmap generation and maybe small things, everything should work on mobile."*

**Mipmap generation is the one thing that needs no work at all** — the hypothesis is wrong there,
in our favour. `FGenerateMips::Execute` opens with `#if PLATFORM_WINDOWS || PLATFORM_ANDROID ||
PLATFORM_LINUX` + `RHIGetInterfaceType() == ERHIInterfaceType::OpenGL` and bypasses *both* shader
paths for driver fixed-function `RHIGenerateMips` (`Engine/Source/Runtime/RenderCore/Private/GenerateMips.cpp:351-366`
— Android is the only platform in the engine with a dedicated branch, and Epic's comment names the
reason: "lack of proper SRV support"). iOS Metal AutoDetects to **Raster**, because
`WillFormatSupportCompute(PF_B8G8R8A8)` is false — `AllTextureFlags` excludes `TypedUAVStore`
(`Runtime/Core/Public/PixelFormat.h:267`) and Metal's explicit whitelist lists `PF_R8G8B8A8` and
not `PF_B8G8R8A8` (`Runtime/Apple/MetalRHI/Private/MetalRHI.cpp:1060-1090`); that is *the same
outcome macOS produces today*, so an iOS mip bug would already be a Mac bug. Android Vulkan
device-queries the storage-image bit (`VulkanDevice.cpp:766, :1165`) and almost certainly also
lands on raster [inferred: Vulkan does not mandate `STORAGE_IMAGE` for BGRA8]. All three routes
degrade; none refuses, none crashes.

**"Everything else works" holds for the renderer and the build, and fails for input and text.**

Works unchanged, verified: feature-level binding (both platforms bind ES3_1 by default, so
`GMaxRHIFeatureLevel` resolves to the right map — `AndroidDynamicRHI.cpp:144-164`,
`Apple/Platform/IOS/IOSPlatformDynamicRHI.h:59-70`); our six global shaders and the `MD_UI`
material pair are admitted by every mobile shader platform (no `ShouldCompilePermutation` anywhere
— `FShader`'s default `return true`, `Shader.h:848`); the `ICustomSlateElement` + RDG path has no
platform predicate anywhere (`SlateRHIRenderingPolicy.cpp:1017-1019, :1689-1707`; zero hits for
`PLATFORM_ANDROID`/`IsMobilePlatform`/`ES3_1` in `SlateRHIRenderer.cpp`); `PF_B8G8R8A8` maps
natively everywhere and GLES's BGRA swizzle rule misses us because it is installed only for
non-RenderTargetable textures (`OpenGLTexture.cpp:609`); the composite picks the pass-through
gamma permutation because both mobile device profiles pin `r.DefaultBackBufferPixelFormat=0`
(`BaseDeviceProfiles.ini:365, :1000`); quickjs needs *nothing* for iOS W^X — a grep of the whole
vendored tree for `mmap|mprotect|MAP_JIT|PROT_EXEC|dlopen|jit` returns zero hits, the dispatch is a
`const` table of `&&label` addresses in signed `__TEXT` (`quickjs.c:17578-17584`); the 2 MB UI
thread stack survives both clamps (both only clamp *up*: 128 KB Android, 512 KB Apple); gamepad
and the D13 nav work port as-is because they are expressed in platform-stable `FKey` names; every
`.Build.cs` is already mobile-clean and both FreeType2 and libPNG ship Android + iOS + Simulator
archives (verified with `nm -u`: 23 undefined `png_*` symbols in both mobile `libfreetype.a`, same
set as Mac/Linux).

Does **not** work:

| | mechanism |
|---|---|
| **Text entry — absent, not degraded** | `ITextInputMethodSystem` has exactly two implementations, Windows and Mac (`GenericApplication.h:550` returns NULL; overrides only at `WindowsApplication.h:390`, `MacApplication.h:198`). Our comment says the degradation is "typing keeps working through `OnKeyChar`" (`Source/VaCuus/Private/VaCuusTextInput.h:161-165`) — true on Linux, false on a phone, where nothing ever produces a char. Mobile uses `IVirtualKeyboardEntry`, a whole-value push API, which we do not implement. |
| **World panel: first touch kills all input, forever** | `HandleMouseButtonUpEvent` clears its latch only under `MouseEvent.GetPressedButtons().IsEmpty()` (`Source/VaCuusRender/Private/VaCuusWorldInputProcessor.cpp:518`), which is **never true for touch**: `OnTouchEnded` builds the event with `bPressLeftMouseButton=true` (`SlateApplication.cpp:6832-6845`) and the touch ctor bakes `PressedButtons = FTouchKeySet::StandardSet` into the value copy (`Events.h:938`, `Events.cpp:15`). The processor holds no Slate capture by design, so no engine safety net fires; every later pointer event in the application is consumed. |
| **Scrolling** | Our only scroll path is `ProcessMouseWheel` (`Source/VaCuus/Private/VaCuusUIThread.cpp:194`). There is no wheel on a phone, and we never call RmlUi's own `ProcessTouchStart/Move/End/Cancel` (`Source/ThirdParty/RmlUi/Include/RmlUi/Core/Context.h:209-230`) — which is where drag-to-scroll, inertia and click-cancel-on-scroll live (`Context.cpp:922-1014`). A touch-drag over a scrollable list does nothing today. |

**The single biggest surprise:** the world-panel lockout is not a mobile bug that needs a device to
find. `FSlateApplication` turns every left click into a real touch event under `-faketouches`
(`SlateApplication.cpp:900, 5261-5264, 6092-6095`), so it reproduces on this Linux box today, and
the plugin's test suite has never seen a touch event — every synthesized `FPointerEvent` in
`Tests/` uses `CursorPointerIndex`.

---

## 2. Per-area verdicts

| Area | Verdict | Android | iOS | Effort |
|---|---|---|---|---|
| Feature levels / RHI selection | works unchanged | GLES3.2 + Vulkan ES3_1 cooked; SM5 opt-in | METAL_ES3_1 only; SM5/6 opt-in | none |
| Global-shader gates (6 shaders) | works unchanged | admitted; DXC→SPIR-V→SPIRV-Cross hop untested | admitted | none |
| Mobile shader **compile** of those shaders | unknown | never cross-compiled here | never cross-compiled here | unknown |
| `MD_UI` material pair (gate) | works unchanged | same gate Slate's proven pair uses | same | none |
| `MD_UI` material pair (mobile template compile) | unknown | preview-platform compile settles it | Mac only | small |
| `ICustomSlateElement` + RDG path | works unchanged | identical; tiler store/load cost | identical | none |
| `FGenerateMips` | works unchanged | GLES fixed-function; Vulkan → raster | raster, = macOS today | none |
| Glass / backdrop blur | **needs work** | Vulkan: direct SRV, works. GLES: copy attempted where the engine's own blur declines | copy fallback, = macOS | medium |
| `PF_B8G8R8A8` / substitution | works unchanged | swizzle rule misses us; swapchain may substitute (handled) | native, no substitution | none |
| Composite gamma permutation | works unchanged | pinned by device profile | pinned by device profile | none |
| PSO creation at draw time | **needs work** | GLES link = classic multi-ms hitch | Metal first-use compile | small |
| UI thread creation / stack | works unchanged | clamp floor 128 KB | clamp floor 512 KB | none |
| UI thread **priority** | **needs work** (claim DONE) | `TPri_BelowNormal` inert; the comment now says so | works (25 vs 31) | small |
| UI thread affinity | unknown | may inherit the game thread's pin | n/a (no pinning) | small |
| App backgrounding / suspension | **needs work** | safe by accident; zero delegates subscribed | same, plus a grace-window constraint | small |
| Clock discontinuity on resume | **needs work** | full gap in one frame; JS watchdog risk | same | medium |
| quickjs W^X / JIT / entitlements | works unchanged | no exec-memory path at all | no entitlement needed | none |
| quickjs stack budget | works unchanged | unchanged | unchanged | none |
| quickjs under the mobile toolchains | unknown | bionic + NDK r27c clang, untested | Darwin libc, = macOS path | small |
| Mapped bundle — **runtime** | split | mapping likely does **not** engage (OBB/APK) | should engage for real, first non-Windows platform | small |
| Mapped bundle — **our claims about it** | **DONE** | comment, log line and buyer doc were all wrong; §3.3 | same | — |
| `PreloadHint` at mount | **needs work** (claim DONE) | no-op on Android; the comment now says so | works; synchronous game-thread IO at mount | small |
| Memory pressure response | **needs work** | no engine delegate for `onTrimMemory` | single-slot handler owned by `UEngine` | medium |
| Touch → tap reaches RmlUi | works unchanged | via Slate's touch→mouse laundering | same | none |
| Touch **release** predicate (widget) | **needs work** | masked by the engine, doubled MouseLeave | same | small |
| World-panel latch on touch | **blocked — see §0** | app-wide input lockout | app-wide input lockout | small |
| Multi-touch | **needs work** | one shared capture bool, one RmlUi button | same, plus system gesture steal | medium |
| Hover / cursor with no cursor | **needs work** | `:hover` flashes per tap; cursor real for DeX/mouse | cursor reporting unknown | medium |
| Touch scrolling (RmlUi touch API) | **needs work** | unused; no wheel exists | unused; needs `ProcessTouchCancel` | medium |
| Text entry / virtual keyboard | **blocked** | modal dialog by default | popup by default; iPad+keyboard = neither route | large |
| Gamepad + analog nav | works unchanged | same `FKey` names, stick digitized | same, plus Siri Remote | none |
| Hardware Back button | **needs work** | `Android_Back` unmapped, reaches nothing | n/a | small |
| Per-module build gates | works unchanged | monolithic → `RMLUI_STATIC_LIB=1` | monolithic | none |
| FreeType2 / zlib / libPNG | works unchanged | ARM64 + x64 archives present | Device + Simulator present | none |
| Plugin descriptor claims | **DONE** | refused, silently — §5 | refused, silently — §5 | — |
| Content staging | works unchanged | OBB by default; APK under AAB | plain files in the `.app` | none |
| Android toolchain | **blocked** | NDK/SDK/JDK absent on the Mac | n/a | medium |
| iOS toolchain | **needs work** | n/a | Xcode 26 in range; signing missing (Simulator needs none) | small |
| Automation suite on device | **needs work** | 0 of 198 tests enumerate | 0 of 198 tests enumerate | medium |
| Store submission | **needs work** | AAB/target-API are the project's, not ours | no JIT, no post-install script load — clean | small |

**Where two passes disagreed.** The mapped-bundle area came back `WORKS_UNCHANGED` from the
threading pass and `NEEDS_WORK` from the build pass. Both are right about different things, so the
row is split: the iOS *runtime* path is correct as written (alignment included — `MappedPtr` may
sit inside the page and the engine says so at `IoDispatcherFileBackend.cpp:2155-2158`; we use it
only as a byte address, `Source/VaCuus/Private/VaCuusFileInterface.cpp:146`), while what our source
and docs *say* about it is false and must change. The scope-caveat area is reconciled above.

---

## 3. The areas that need work

### 3.1 Correctness — three touch bugs

**World-panel latch never releases** (`VaCuusWorldInputProcessor.cpp:518`). *Today:* the clearing
predicate cannot be true for touch (mechanism in §1). The other `ClearLatch` sites all require the
panel to have died (`:390, :446, :601`) and `Tick` is empty (`VaCuusWorldInputProcessor.h:220`);
`IInputProcessor` has no touch or capture-lost hook at all (`IInputProcessor.h:26-50`), and
preprocessors run before all Slate routing (`SlateApplication.cpp:5324, 6148`). The comment at
`:509-517` justifying the predicate cites `SlateApplication.cpp:6100-6103`, which is about the
*application's* live set, not the event's value copy — it is wrong about touch and it is the
load-bearing justification. *Takes:* release on `!MouseEvent.IsTouchEvent() ?
GetPressedButtons().IsEmpty() : true`, or key the latch by pointer index. *Proves it:* the world
demo under `-faketouches` — one tap, then assert `bLatched` false and `NumConsumed` (`:386, :397,
:405`) stops climbing; restore-the-bug by reverting the predicate and watching input die.

**Widget touch release** (`SVaCuusWidget.cpp:638`). *Today:* same predicate, but masked —
`FSlateUser::NotifyPointerReleased` force-releases capture for every touch pointer
(`SlateUser.cpp:1284-1290`) and `OnMouseCaptureLost` does our cleanup (`SVaCuusWidget.cpp:713-729`).
The only visible symptom is a doubled `ProcessMouseLeave` per tap. *Takes:* the same fix. *Proves
it:* after a faked touch the release branch did not run while `IsTrackingMouseCapture_Debug()`
(`SVaCuusWidget.h:213`) nonetheless ends false — that separates "we released" from "the engine
rescued us".

**Multi-touch state is scalar.** `FVaCuusMouseCaptureState` is one bool (`SVaCuusWidget.h:35-45`),
no production path reads `GetPointerIndex`, and every finger dispatches RmlUi button 0
(`VaCuusInputMap.cpp:272-277`). *Consequence [inferred from those read facts]:* finger 0 lifting
tears down hover/active while finger 1 is still down, and two fingers produce down,down,up,up on
one button — a second click on whatever is hovered. The snapshot type itself is fine: a pure
coverage union with no pointer identity (`VaCuusInteractiveSnapshot.h:329-342, :495-506`). *Proves
it:* device only — `-faketouches` produces index 0 exclusively.

### 3.2 Missing capability

**Text entry.** Scope is in §1. The good news is that the shadow-state design already fits:
`FVaCuusTextFieldState` publishes value, selection and read-only per frame
(`VaCuusInteractiveSnapshot.h:171-241`) — exactly what `GetText`/`GetSelection`/`IsMultilineEntry`
need (`IVirtualKeyboardEntry.h:51-111`) — and mutations already flow as generation-stamped queue
events (`VaCuusInputEvent.h:56-91`). *Takes:* a game-thread adapter implementing
`IVirtualKeyboardEntry` over that shadow, plus `FSlateApplication::ShowVirtualKeyboard`
(`SlateApplication.cpp:4283-4297`) from the same D14a click site that today calls `PushImeSurface`
(`SVaCuusWidget.cpp:606-614`), keyed off `EVaCuusRectFlags::TextInput`. Epic's own editable text
does exactly this and carries the matching TODO (`SlateEditableTextLayout.cpp:879-894`). Two
platform facts to decide around: Android shows a **modal dialog** by default
(`AndroidPlatformTextField.cpp:112-122`, cvar `Android.NewKeyboard` at `:30-35`), and iOS's
`RequiresVirtualKeyboard()` is dynamic — `!IsAnyPhysicalKeyboardConnected()`
(`IOSPlatformApplicationMisc.cpp:361-369`), so an iPad with a Magic Keyboard takes the
`ITextInputMethodSystem` branch iOS does not implement and gets **no text entry by either route**
unless the hardware-key path is wired too.

**Touch dispatch to RmlUi.** *Takes:* new `FVaCuusInputEvent` kinds carrying a touch id (the struct
has position and key but no pointer identity, `VaCuusInputEvent.h:136-202`) and a dispatcher arm
onto `Context::ProcessTouch*`. Additive, not a rewrite: RmlUi's touch verbs funnel into
`ProcessMouseMove` + `ProcessMouseButtonDown(0)/Up(0)` internally (`Context.cpp:916-919,
1019-1022`). Two mechanics to get right: the click/scroll distance threshold is scaled by
`density_independent_pixel_ratio` (`Context.cpp:978`), which we never set — grep finds
`SetDensityIndependentPixelRatio` only inside a comment in the lobby demo, which since
2026-08-07 lives in the VaCuusDemo project rather than the plugin
(`Source/TP_ThirdPerson/VaCuus/VaCuusLobbyDemo.cpp:634`, bead VaCuus-akj.25) — so on a
high-DPI phone the default ratio makes it several times too tight; and RmlUi's duplicate-touch
guard is `RMLUI_ASSERTMSG`, compiled out in every configuration of this build
(`Source/VaCuusRml/VaCuusRml.Build.cs:40-44`), so a malformed touch stream degrades silently.

**Hardware Back (Android).** `AndroidKeyNames::Android_Back` (`AndroidInputInterface.cpp:176`) is
in neither the D12 pass-through set (`SVaCuusWidget.cpp:144-150`) nor the key map
(`VaCuusInputMap.cpp:31-215`), so it reaches nothing. `EVaCuusInputEventKind::NavigateBack` already
exists for the pad's B button (`VaCuusInputEvent.h:39-54`) — small, high value.

**Glass on Android GLES.** Our discriminator is the desc flag
(`VaCuusSlateElement.cpp:468`). Vulkan's backbuffers carry `ShaderResource`
(`VulkanViewport.cpp:504, :536`) so the direct-SRV route runs (correct, but a tile flush); Metal's
never does (`MetalViewport.cpp:468-473`, and `GMetalSupportsIntermediateBackBuffer` is 0 on iOS),
so iOS takes the same bounded copy macOS takes. Neither GLES backbuffer sets it either
(`AndroidOpenGL.cpp:616-618`, `OpenGLViewport.cpp:294-297`) — but there the **engine itself**
declares backbuffer sampling unsupported unless the project opts in
(`FAndroidMisc::SupportsBackbufferSampling()` = `bAndroidOpenGLSupportsBackbufferSampling ||
ShouldUseVulkan()`, `AndroidPlatformMisc.cpp:3410-3420`, no default in `BaseEngine.ini`), and
`SBackgroundBlur` refuses the real blur on exactly that signal (`SBackgroundBlur.cpp:108-110`). We
have no such gate. *Takes:* mirror `FPlatformMisc::SupportsBackbufferSampling()` into the same
game-thread mirror the HDR check already uses (`SVaCuusWidget.cpp:351-358` →
`SetGlassAllowed_RenderThread`) so glass is refused with a log line rather than attempted.

### 3.3 Claims in our own source and docs that are false on mobile

These are the cheapest items on the list and the codebase treats this class as a defect.

**All seven rows below are FIXED** (commit "the descriptor stops claiming platforms we never built,
and six comments stop lying about mobile"). The table is kept as the record of what was wrong and
why, so the reasoning survives the diff; the `Where` column's line numbers are from before the fix
and the comments have grown, so read them as "this claim, in this file". Two corrections to the
table's own citations are folded in below.

| Claim | Where | Truth |
|---|---|---|
| "on 5.8 only Win64's platform properties answer true to memory mapping" | `Source/VaCuus/Private/VaCuusBundleMount.cpp:84-88` | `AndroidPlatformProperties.h:118-121` and `IOSPlatformProperties.h:71-74` both return **true**; only the Generic false (`GenericPlatformProperties.h:258-261`) that Linux/Mac inherit. |
| Resident mount logs "`SupportsMemoryMappedFiles()` is false on this platform" | same file `:93` | On Android the property is true and the mapping failed for a *different* reason — this is the line you would be reading at 2am. |
| "literally true only on Win64" | `docs/research/m6-api-notes/bundle-cook.md:90` | Same error. |
| "memory-mapped on Win64, resident buffer on Linux/macOS" | `docs/buyer/setup.md:102-105` | Must become per-platform; iOS is the first non-Windows platform where the mapped branch runs for real. |
| "`BelowNormal` keeps us off the game and render threads' backs" | `Source/VaCuus/Private/VaCuusUIThread.cpp:379` | True on iOS (25 vs 31, `ApplePlatformRunnableThread.h:73-88`). On Android `ANDROID_USE_NICE_VALUE_THREADPRIORITY` defaults to **0** (`AndroidPlatform.h:133-134`), so the nice path is compiled out and `pthread_setschedparam` is called with `sched_priority = 5` under SCHED_OTHER — EINVAL on Linux/Android — with the **return value discarded** (`Runtime/Core/Private/HAL/PThreadRunnableThread.h:55, :75-76` — `:53` above was `TPri_AboveNormal`, `:55` is the `BelowNormal: return 5`). Silent no-op. Note the claim *does* hold on all three supported platforms, which is why it stood: Win64 `THREAD_PRIORITY_BELOW_NORMAL` (`WindowsRunnableThread.cpp:22`), Linux `setpriority()` relative to a captured process baseline (`UnixPlatformRunnableThread.cpp:86-108`), Mac 25 vs 31. |
| `PreloadHint` "page-faults IO that would otherwise land on the UI thread" | `Source/VaCuus/Private/VaCuusBundleMount.cpp:206-208` | `IMappedFileRegion::PreloadHint`'s base is an empty body (`MappedFileHandle.h:75-77`); `FAndroidMappedFileRegion` does not override it (`AndroidPlatformFile.cpp:1324-1339`). Android's mechanism is `MAP_POPULATE` at map time, which IoStore never requests (`IoDispatcherFileBackend.cpp:2152`). Apple *does* implement it (`ApplePlatformFile.cpp:415-430`) — and it is synchronous blocking IO on the **game thread** at mount (`VaCuusBundleMount.cpp:208`), pennies on an SSD, a real hitch on a cold phone. |
| "`ClangToolChain.cs:649` turns it into `-isystem`" | `Source/VaCuusRml/VaCuusRml.Build.cs:35` | Mechanism right; the number is imprecise rather than stale, and this row was itself slightly wrong. `:649` in this engine is `Arguments.AddRange(CompileEnvironment.SystemIncludePaths.Select(IncludePath => GetSystemIncludePathArgument(...)))` — the right dispatch, but a reader who opens it does **not** find the flag. The literal `-isystem` is in `GetSystemIncludePathArgument` at `:636-638` (not `:635-638`; `:635` is blank). Both are now cited. |

Two more that are not wrong, just incomplete: the `Atomics` divergence note at
`Source/VaCuusJs/VaCuusJs.Build.cs:54-75` is MSVC-only and does **not** recur on mobile (both
toolchains are clang at `-std=c11`, so `CONFIG_ATOMICS` is on — `quickjs.c:74-78`); and the
libpng-linking fix at `VaCuusRml.Build.cs:112-114` is redundant on mobile, because
`AddEngineThirdPartyPrivateStaticDependencies` is gated on
`!bUsePrecompiled || LinkType == Monolithic` (`ModuleRules.cs:1601-1607`) and mobile is always
monolithic. Keep it — it is load-bearing on the editor target — but say so, or a reader will
assume it is what makes mobile link.

### 3.4 Lifecycle, clock and memory — the plugin subscribes to nothing

A grep for `EnterBackground|EnteredForeground|WillDeactivate|HasReactivated|MemoryWarning|
ApplicationLifetime` over the whole plugin returns **zero hits**. What happens today is benign *by
accident*, and the accident is load-bearing: the UI thread only runs when `UVaCuusSubsystem::Tick`
calls `Trigger()` (`VaCuusSubsystem.cpp:167`), nothing ever blocks on it, and both platforms
suspend by parking the game thread (`LaunchAndroid.cpp:1632-1668`, `IOSAppDelegate.cpp:1859-1875`).
So the UI thread finishes its frame and parks in `WakeEvent->Wait()` (`VaCuusUIThread.cpp:955`) —
outside RmlUi, outside JS, holding no lock. Correct, but nothing enforces it: the day anything else
wakes the UI thread (a timer, an async load callback, a render-thread completion), backgrounding
stops being safe and no delegate is wired to notice.

The clock keeps running while backgrounded — `FPlatformTime::Seconds()` is `CLOCK_MONOTONIC` on
Android and mach on Apple, and neither stops for a mere background
(`VaCuusSystemInterface.cpp:63-66`, fed to both the RmlUi advance and the JS pump at
`VaCuusUIThread.cpp:1125`). CSS animations jumping the whole gap and rAF seeing a huge `dt` are
browser-like and are content's problem; **JS interval timers already handle it correctly**, re-arming
from fire time rather than the old deadline — a design win already banked. The real risk is the JS
watchdog: it is the same wall clock against a deadline armed at JS entry
(`VaCuusJsRuntime.cpp:258, :482`), so a freeze *inside* a JS call resumes to an expired deadline and
an uncatchable `interrupted` InternalError — 50 ms in Shipping. Narrow window, silent, and it will
never reproduce on a desk.

Nothing in the plugin shrinks under memory pressure, and the engine offers little to hook:
`FAndroidStats::OnTrimMemory` broadcasts **no delegate at all** (`AndroidStats.cpp:264-310`), and
both platforms' warning paths call a **single global function pointer** `GMemoryWarningHandler`
that `UEngine::Init` already claims (`UnrealEngine.cpp:2391-2394`). The portable hooks are
`FCoreDelegates::GetMemoryTrimDelegate()` and `ApplicationShouldUnloadResourcesDelegate`
(`CoreDelegates.h:404, :522-523`); we subscribe to neither. On our side, `CollectGarbage` returns
false unless live bytes grew ≥512 KB or an OOM is pending (`VaCuusJsRuntime.cpp:341-348`) — there is
no "collect regardless" entry point — the JS cap is a fixed 16 MB, RmlUi's caches are dropped only
by the live-reload command, and the per-view RTs are persistent by design. One counterweight
[inferred, OS semantics]: on iOS the mapped bundle is clean file-backed pages that largely do not
count toward the jetsam footprint, so the mapped path is a footprint *win* there versus the
resident branch measured on desktop.

### 3.5 PSO stalls and the automation gap

We create pipelines at draw time — `BindPipeline`'s `SetGraphicsPipelineState`
(`VaCuusReplayRenderer.cpp:464`), the glass draw (`VaCuusSlateElement.cpp:593`), one full pipeline
per material (`VaCuusMaterialDraw.h:150-151`), plus `AddDrawScreenPass`'s internal binds. Nothing
forbids it (`PipelineStateCache.cpp:1685-1696`; `PLATFORM_USE_FALLBACK_PSO` is 0 with no mobile
override in the readable tree) and the count is small and fixed by construction — the replay pass
shares one VS, blend, rasterizer, depth-stencil and vertex declaration across both pixel shaders
(`VaCuusReplayRenderer.cpp:416-426`), so UI-vs-Gradient is a cache hit, ~7 pipelines plus one per
material. Each is written to the PSO file cache on first creation
(`PipelineStateCache.cpp:4805`), so a recorded cache from a run that *shows a VaCuus HUD* covers
them — that recording step is the work item, and the buyer perf guide should say so. On GLES a
"PSO" is a program link (`OpenGLCommands.cpp:2591-2593`) and an uncached link is the classic
multi-millisecond Android hitch. **Shape hazard:** the glass and composite pipelines are keyed on
the *backbuffer* format, so a device whose swapchain substitutes a format
(`VulkanSwapChain.cpp:299-336`) mints a PSO a desktop capture never recorded.

The 198-test suite compiles into a mobile Development build and then runs **nothing**. Every
declaration carries `EditorContext | EngineFilter` and nothing else (198 occurrences of
`EAutomationTestFlags::EditorContext`, zero of `ClientContext`); in a packaged game
`GIsEditor` is false so the application mask is `ClientContext|ServerContext`
(`AutomationTest.cpp:805-833`) and the gate at `:863-871` yields 0 — the tests do not fail, they
never enumerate. Meanwhile they are compiled in and paying binary size
(`WITH_DEV_AUTOMATION_TESTS` is 1 outside Test/Shipping on every platform,
`UEBuildTarget.cs:6311, :6326`). The fix is one token per declaration, but not free: the tests must
then pass without editor data, and the bundle-pack test lives in `VaCuusEditor`, which does not
exist in a mobile target — a subset stays editor-only by design. Running them is solved: Android
stages `UECommandLine.txt` into assets (`UEDeployAndroid.cs:4518-4519`), iOS has the equivalent.
**If this is deferred, say plainly that the substitute evidence — three compile legs, a clean cook,
a hand-driven on-device demo run with the device log — is substitute evidence, not the suite.**

---

## 4. The unknowns, and the cheapest experiment for each

Kept unknown on purpose. None of these was smoothed into a verdict.

| Unknown | Cheapest experiment | Needs |
|---|---|---|
| Do our 6 global shaders + the `MD_UI` pair compile for the mobile formats? | Editor Preview Rendering Level → "Android Vulkan Mobile" (`VULKAN_ES3_1_ANDROID`) with a `UVaCuusStyleSet` material registered; watch the compile log for `FVaCuusMaterialVS/PS`. Repeat with "Android OpenGL" for the SPIRV-Cross leg. | this box |
| Same, for Metal | Same via `IOSMetal` preview, or a plain `-platform=IOS` cook | Mac |
| Does anything in the cook reject? | `BuildCookRun -platform=Android_ASTC -cook` (no build/stage). A clean log is the whole answer. | NDK |
| Does the Android GLES backbuffer **copy** succeed at all? | Package the M5 glass demo, run once with `-vulkan` and once forced to GLES; grep for our latched `Exp-GLASS-BACKBUFFER-SRV` line plus any GL error. `RHICopyTexture` is `CopyImageSubData` (`OpenGLTexture.cpp:1866`), which needs a real texture object on both sides — whether the EGL backbuffer has one depends on `AndroidEGL::IsOfflineSurfaceRequired()` (`AndroidEGL.cpp:1138-1146`), itself conditional on a cvar, Android version and vendor id. | device |
| Does the Android bundle mapping engage? | Cook+stage Shipping Android, grep the log for `Memory map bulk data from chunk ... FAILED` (`AsyncLoading2.cpp:3302`) and for our `memory-mapped region` vs `resident buffer` line. Expectation: resident, because `FAndroidPlatformFile::OpenMappedEx2` `open()`s `LocalPath` only and never consults the OBB/APK `AssetPath` (`AndroidPlatformFile.cpp:1719-1746`). | device/emulator |
| Does quickjs build clean under bionic + NDK r27c clang? | The Android compile leg. Note the dev box has the *newest* compiler of the three (clang 20 vs Xcode 26's LLVM 19.1.5 vs NDK r27c) — "it builds here" is the weakest evidence for mobile, not the strongest. | NDK |
| Is `_GNU_SOURCE` inert on bionic/Darwin? | Same leg. The one place it demonstrably changes UE behaviour (`UnixPlatformTLS.h:29-45`) is unreachable on mobile — Android and iOS use their own TLS headers. | NDK/Mac |
| Per-PSO stall in ms | GPU/CPU capture on device, first HUD frame vs steady state | device |
| Does the UI thread inherit the game thread's core pin? | Log `sched_getaffinity` from `FVaCuusUIThread::Init()`, two runs, with and without `android.DefaultThreadAffinity GT 0x0f`. No engine rebuild. [inferred: pthreads inherit the creator's mask] | device |
| Which memory hook actually fires on iOS 18+? | One build with a log in each candidate (`GetMemoryTrimDelegate`, `ApplicationShouldUnloadResourcesDelegate`, the warning handler) | device |
| Does GLES `glGenerateMipmap` match the raster chain visually over our premultiplied, display-encoded RT? | World-panel demo screenshot vs the desktop reference. It is a gamma-unaware driver box filter — which the WS-GAMMA note already accepts. | device |
| Is the EGL/Vulkan surface sRGB-encoded? | The M6 gamma matrix's photograph-a-known-swatch protocol, one run | device |
| Does iOS report a usable cursor for a connected trackpad? | Read `IOSCursor.cpp` behaviour, then confirm on an iPad | Mac + device |
| Correct `density_independent_pixel_ratio` per screen | Read it off the device and compare tap/scroll thresholds by feel | device |
| Is the Android modal-dialog keyboard acceptable over a live document? | Product judgement, on device | device |
| Multi-touch and hover fidelity | Device only — `-faketouches` skips the `OnMouseEnter`/`Leave` synthesis (`SlateApplication.cpp:5467, 5584`) and only ever produces finger 0 | device |
| Platform-extension overrides (`Platform.h`, `.ini`) on a Mac install | Re-run the `PLATFORM_USE_FALLBACK_PSO` and `r.DefaultBackBufferPixelFormat` greps there before treating the iOS verdicts as final | Mac |

---

## 5. What the plugin should declare now

*(Written before the fix; kept because it is the argument for the field. The field is now in
`VaCuus.uplugin` and what follows the code block is the measured result.)*

`VaCuus.uplugin` declared **no** `SupportedTargetPlatforms` and no per-module allow/deny list.
`PluginDescriptor.SupportsTargetPlatform` returns true for every platform when the list is
null/empty and `bHasExplicitPlatforms` is false (`Configuration/Descriptors/PluginDescriptor.cs:753-763`;
runtime twin `Runtime/Projects/Private/PluginDescriptor.cpp:764-774`), so:
UBT admitted every Runtime module to a mobile Game target (`UEBuildTarget.cs:5842-5846` and
`:5886-5890` are the gates that *would* have refused; `:5141-5144` is the same test in the
build-everything path), UAT
stages the `.uplugin` (`CopyBuildToStagingDirectory.Automation.cs:897-904`), and `RunUAT
BuildPlugin -TargetPlatforms=Android+IOS` will attempt Development *and* Shipping compiles for both
(`BuildPluginCommand.Automation.cs:264-275`). With `"EnabledByDefault": true`, any project that
drops this plugin into `Plugins/` and packages for Android gets all four modules compiled with no
gate whatsoever. **The plugin's answer to "do you support mobile" was "yes", and it had never
been built there.**

The honest interim declaration is one field:

```json
"SupportedTargetPlatforms": [ "Win64", "Mac", "Linux" ],
```

Authoring is unaffected: `bIncludePluginsForTargetPlatforms` defaults to `Type == TargetType.Editor`
(`Configuration/Rules/TargetRules.cs:1554-1557`), so the editor still loads the plugin and only a
*game* target for an unlisted platform refuses. It also makes a Fab listing's platform metadata true
by construction rather than by promise.

**What that refusal looks like was afterwards MEASURED, not predicted, and one prediction above was
wrong.** No Android toolchain is needed for this: `LinuxArm64` is a valid UBT platform on the Linux
box, is outside the declared list, and takes the identical code path. `UnrealBuildTool -Mode=JsonExport
VcHost LinuxArm64 Development` runs the whole target setup without compiling. Three cases, all
observed:

| Case | What the buyer sees |
|---|---|
| Plugin enabled by default, no `.uproject` reference (**our own VcHost**) | **Total silence.** Exit 0, a target with **zero** VaCuus modules, and not one mention of the plugin in the UBT log. `AddPlugin` returns null after a `Logger.LogTrace` (`Configuration/UEBuildTarget.cs:5842-5846`) — reached because `:5679-5687` copies the descriptor's list onto the synthesized reference — and `LogTrace` is below UBT's default output level (`GlobalOptions.cs:28` = `LogEventType.Log` = `LogLevel.Debug`, `EpicGames.Core/Logging/Log.cs:52`; filter `logLevel >= OutputLevel` at `:1244`). At `-VeryVerbose` one line appears: `Ignoring plugin 'VaCuus' (referenced via VcHost default plugins) due to unsupported target platform.` |
| Plugin listed in the `.uproject` (**what `docs/buyer/setup.md` step 2 tells buyers to do**) | Hard failure, exit 6, plugin named: `VaCuus.uplugin is referenced via Gate.uproject with a mismatched 'SupportedTargetPlatforms' field…` (`UEBuildTarget.cs:5886-5890`). Loud — but the remedy it suggests ("Launch the editor to update references") addresses a different problem, so the message needs translating in the buyer docs. |
| A buyer module names a VaCuus module in its dependencies | ~~link failure~~ — **this note's original prediction was wrong.** UBT logs the same silent `Ignoring plugin` trace and then **resolves and compiles the modules anyway**: the `LinuxArm64` target for a project whose module depends on `"VaCuus"` builds `VaCuus` and `VaCuusRml` into the target while `VaCuusRender` and `VaCuusJs` stay out. A half-plugin with no renderer and no staged `.uplugin`, and no message at any verbosity. Worse than the link error that was assumed. |

Case 1 is unreachable from our side — the plugin is gone before any `.Build.cs` of ours is
constructed — so it is documented in `docs/buyer/setup.md` and nowhere else. Case 3 **is** reachable,
and is now refused by name from `Source/VaCuusRml/VaCuusRml.Build.cs`, which re-reads the descriptor
and asks `PluginDescriptor.SupportsTargetPlatform` (`Configuration/Descriptors/PluginDescriptor.cs:753-763`)
rather than copying a platform list that could drift. `VaCuusRml` carries it because every other
module in the plugin depends on it, so one guard covers every dependency path.

**`bHasExplicitPlatforms` is deliberately NOT set.** With a non-empty list both branches of every
implementation of the check are identical — `FPluginDescriptor::SupportsTargetPlatform`
(`Runtime/Projects/Private/PluginDescriptor.cpp:764-774`), its UBT twin
(`PluginDescriptor.cs:753-763`), `FPluginReferenceDescriptor::IsEnabledForPlatform`
(`PluginReferenceDescriptor.cpp:50`) and `PluginReferenceDescriptor.IsSupportedTargetPlatform`
(`PluginReferenceDescriptor.cs:427-437`) — and the two places that test the flag test it as an OR
against a non-empty list anyway (`PluginManager.cpp:1017`, `CookOnTheFlyServer.cpp:11176`). It buys
nothing here and adds a trap: the flag's whole purpose is to make an **empty** list mean "no
platforms", so anyone who later clears the array to re-open the plugin would silently refuse every
platform instead. Leave it absent.

Nothing the plugin itself ships trips Apple's dynamic-code rules, and it is worth recording why
rather than assuming it: the vendored quickjs is a pure bytecode interpreter with no JIT and no
dynamic loading (grep evidence in §1), the only file watcher is in `VaCuusEditor`, and in Shipping
the loose DevUI tree — `.js`/`.mjs` included — is not staged at all (`VaCuus.Build.cs:138-148`).
Say that in the buyer docs so a reviewer question has a prepared answer.

---

## 6. Proposed beads — cheapest de-risking first

1. **DONE — Declare `SupportedTargetPlatforms` and document the refusal.** Add
   `["Win64","Mac","Linux"]` to `VaCuus.uplugin` and one paragraph to `docs/buyer/setup.md`
   explaining that a mobile game target refuses at configure time, and that a buyer module
   depending on `VaCuus` will see a link error instead. Minutes of work; it makes every other item
   on this list non-urgent for a buyer.
2. **THE ONE THAT IS A LIVE BUG TODAY (see §0) — fix the touch release predicate in both places, with the first touch tests the suite has
   ever had.** `VaCuusWorldInputProcessor.cpp:518` and `SVaCuusWidget.cpp:638`, plus corrected
   comments that stop citing `SlateApplication.cpp:6100-6103` as if it were about the event copy.
   Reproducible and provable today under `-faketouches` — no device, no toolchain.
3. **DONE — Correct the false claims in source and docs.** The memory-mapping scope (mount comment, mount
   log line, `bundle-cook.md:90`, `setup.md:102-105`), the `TPri_BelowNormal` claim
   (`VaCuusUIThread.cpp:379`), the `PreloadHint` claim (`VaCuusBundleMount.cpp:206-208`), and the
   stale `ClangToolChain.cs:649` citation. Pure documentation of mechanisms already read; no
   behaviour changes.
4. **Compile the mobile shader maps on this host via Preview Rendering Level.** Android Vulkan and
   Android OpenGL previews with a registered `UVaCuusStyleSet` material, watching for our six
   global shaders and the `FVaCuusMaterialVS/PS` pair. The cheapest possible evidence that the
   shader surface survives DXC → SPIR-V → SPIRV-Cross; it needs no NDK and no device.
5. **Stand up the Android toolchain on the Mac and take the iOS Simulator leg.** Run
   `Engine/Extras/Android/SetupAndroid.command` (NDK r27c, SDK 34, build-tools 35.0.1, cmake
   3.22.1, JDK 17+), then build `IOSSimulator` Development, which needs no signing identity and no
   Apple account. First real compile evidence on either platform.
6. **Three compile legs plus one clean cook per platform.** Android arm64, iOS device, iOS
   Simulator; then `-cook` only, reading the log for shader errors. Any construct the mobile
   compilers reject appears here as a cook error, not a build error.
7. **Gate glass on `FPlatformMisc::SupportsBackbufferSampling()`.** Mirror it into the same
   game-thread mirror the HDR check already uses, so Android GLES refuses glass with one log line
   instead of copying from a backbuffer the platform declares unsamplable. Small, and it removes
   the only place a mobile RHI could fail inside our render pass.
8. **Wire the lifecycle delegates and make backgrounding an enforced property.** Subscribe to the
   engine's foreground/background delegates, and on resume clamp the UI clock delta and re-arm the
   JS watchdog deadline so a freeze inside a JS call cannot resurface as a spurious `interrupted`
   InternalError. Today's safety is an accident of "the game thread is the only pulse" that no test
   asserts.
9. **Touch dispatch onto RmlUi's own API.** New input-event kinds carrying a touch identifier,
   dispatch to `Context::ProcessTouchStart/Move/End/Cancel`, and a real
   `SetDensityIndependentPixelRatio`. This is what makes lists scroll, adds inertia and
   click-cancel for free, and gives us per-finger state RmlUi already implements.
10. **Per-pointer capture state in `SVaCuusWidget`.** Replace the single `FVaCuusMouseCaptureState`
    bool with per-index state so finger 0 lifting stops tearing down a gesture finger 1 is still
    driving. Depends on 9 for the identity plumbing; device-only to verify.
11. **`IVirtualKeyboardEntry` adapter over the existing text shadow.** A game-thread adapter
    exposing `FVaCuusTextFieldState` as `GetText`/`GetSelection`/`IsMultilineEntry` plus
    `SetTextFromVirtualKeyboard` into the existing generation-stamped mutation queue, shown from the
    D14a click site. The largest item here, and the one that decides whether mobile is a supported
    platform or a demo — including the iPad-with-hardware-keyboard case, which needs the key path
    too.
12. **Android hardware Back → `NavigateBack`.** Map `AndroidKeyNames::Android_Back` onto the
    existing `EVaCuusInputEventKind::NavigateBack` and add it to the pass-through set when no view
    wants it. One entry in the key map and one line in the pass-through list.
13. **Automation flags: `ClientContext` where the test does not need editor data.** Add the token to
    every declaration that passes without the editor, leave the rest editor-only by design, and add
    the staged `UECommandLine.txt` recipe to the dev-loop docs. Until this lands, on-device evidence
    is hand-driven demo runs, and the note must say so.
14. **Ship a recorded PSO cache and say how.** Record from a run that actually displays a VaCuus
    HUD, and document in `docs/buyer/perf-guide.md` that a capture recorded on a device whose
    swapchain format differs will miss the glass and composite pipelines. Mitigates the GLES
    program-link hitch, which is the loudest first-use cost on Android.
15. **Decide the Android thread-priority and affinity story.** Either define
    `ANDROID_USE_NICE_VALUE_THREADPRIORITY=1` for the target and keep the claim, or drop the claim
    and accept that the UI thread competes with the game thread; then measure whether it inherits
    the game thread's core pin with one `sched_getaffinity` log. Cheap to measure, and it is the
    difference between "off their backs" and "on their core".
16. **A memory-pressure response.** Subscribe `FCoreDelegates::GetMemoryTrimDelegate()` (and
    `ApplicationShouldUnloadResourcesDelegate` once we know which one actually fires on iOS), and
    add a "collect regardless" entry point to the JS runtime plus an asset-cache drop that is not
    the live-reload command. Nothing in the plugin shrinks under pressure today.
