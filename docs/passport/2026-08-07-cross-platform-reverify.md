# Cross-platform re-verification, 2026-08-07 — Linux, Win64, macOS on one commit

**What this is.** Not a new pass. The 2026-08-03 Win64 and macOS passports each explored a
platform for the first time and found things; this re-runs the three *gates* — build, suite,
export surface — on a single current commit so the numbers in those documents stop being the
newest ones anybody can find. It exists because 2026-08-07 changed the plugin's public C++
surface (bead `VaCuus-akj.25`), and a surface change is exactly the class of edit that can
build on one platform and not the others.

**The commit under test:** `0e1f470` (`master`, "tools: the Win64 export gate learns akj.25's
surface, and proves the handle is opaque"). Same tree on all three machines — Linux merged it,
the Windows trees were fast-forwarded to it over a git bundle, the Mac was cloned fresh at it.

**What landed today that this is checking**

| | |
|---|---|
| `VaCuus-9b3` | `FVaCuusWorldSink` starts the async texture upload; `IVaCuusFrameSink::SetPendingBuffer_RenderThread` widened to `FRHICommandListImmediate&`. New test `VaCuus.World.AsyncUpload`. |
| `VaCuus-akj.25` | The lobby demo left the plugin for the VaCuusDemo project. `SVaCuusWidget` became public and **exported**; `VaCuusSlateView.h` added two factories over opaque handles. |

The second one is the risk. Exporting a Slate widget class is the first `VACUUSRENDER_API` on a
non-`UCLASS` type in this plugin, and each platform's linker has a different opinion about what
that means.

---

## The three machines

| | Linux | Win64 | macOS |
|---|---|---|---|
| Host | Arch, kernel 7.1.5, 16 physical / 32 logical | `DESKTOP-590NICV`, Windows 11 10.0.26200, 8/16 cores, 15.9 GB | `va-macbook`, macOS 26.5.2 (25F84), **Apple M1 Pro**, 10/10 cores, 16 GB |
| Engine | **source build** `/w/Unreal/UnrealEngine` | Installed UE 5.8, CL 56057345 | Installed UE 5.8, CL 56057345 |
| Toolchain | clang 20.1.8 (engine-bundled) | MSVC 14.44 / VS 2022 17.14 | Xcode 26.0 (17A324) |
| Target built | `VcHostEditor` + monolithic `VcHost` | `TP_ThirdPersonEditor` | `TP_ThirdPersonEditor` |

The two Installed builds are the same changelist as each other and as the August passes, so the
comparison across the three columns is honest. Linux is the odd one out on purpose — it is the
dev box and it is a source build.

## 1. Build

| | Result | Time | Warnings |
|---|---|---|---|
| Linux editor + monolithic game | Succeeded | 13 s / 73 s (incremental) | none |
| Win64 editor | **Succeeded** | 140.91 s, 166 actions | **zero**, across the whole 200-line log |
| macOS editor | **Succeeded** | 211.45 s, 327 actions | **zero**, across the whole 361-line log |

**The question this was run to answer, and the answer.** `VACUUSRENDER_API` on `SVaCuusWidget`
and on `FVaCuusMouseCaptureState` was expected to be the risky part under MSVC: a dllexported
class exports *every* member, and C4251 fires when an exported class has a member of a type that
is not itself exported — which `SVaCuusWidget` has twice over (`TSharedPtr<FVaCuusSlateElement>`
on an **incomplete** type, and `FVaCuusMouseCaptureState`). Grepping the MSVC log for
`C4251|C4275|dllexport|dllimport|warning` returns **NONE**. Clang on Mac and on Linux is quieter
still. So the façade shape costs nothing at compile time on any of the three.

## 2. Automation suite

`-nullrhi -unattended`, filter `VaCuus`, counts read from the log rather than stdout on every
platform (an interleaved `UnrealTraceServer` fork clobbers the stdout tail — this bit again on
Linux today and was mistaken for a missing test for a minute).

| | Tests | Failures |
|---|---|---|
| Linux | 217 | **0** |
| Win64 | 217 | **0** |
| macOS | 217 | **0** |

Identical counts, which is itself the assertion: no test is being skipped on a platform. The two
tests that disappeared today (`VaCuus.Render.LobbyDemo.RefusesWithoutContent` and `.BackdropRefuses`,
219 → 217) went with the demo; their precondition was "this host serves no `chrome.rml`", which
stops being a statement about the plugin once the command lives elsewhere.

`VaCuus.World.AsyncUpload` — the new `9b3` test — is **Success on all three**.

Linux additionally ran `VaCuus.World` + `VaCuus.Render.Upload` against a **real RHI**
(`-RenderOffscreen`, Vulkan): 12/12, including `MipContentGPU`, which exercises the RDG mip path
whose parameter type `9b3` changed. `Upload.Cost` still reproduces `akj.6.25`'s curve — 137.3 MB
inline 31.6 ms against ASYNC START 0.342 ms.

**A macOS venue fact that cost ten minutes and is not in the August passport:** with an Installed
engine, the project log is **not** at `<Project>/Saved/Logs/VcHost.log`. It is at
`~/Library/Logs/Unreal Engine/VcHostEditor/VcHost.log`. Deleting the former and then reading it
back reports "no log yet" forever while the suite runs to completion in the latter.

## 3. Export surface

The gate asks the only question a header cannot: what does a **buyer's linker** see? Two legs —
every supported class must have at least one exported member, and every internal one must have
**zero**.

**Win64**, `Tools/api_export_check_win64.ps1` against the built DLLs:

```
ok  VaCuusRender/UVaCuusWidget:          27 exported member references
ok  VaCuusRender/UVaCuusWorldComponent:  53
ok  VaCuus/UVaCuusView:                  67
ok  VaCuus/UVaCuusSubsystem:             39
ok  VaCuus/UVaCuusStyleSet:              14
ok  VaCuusRender/SVaCuusWidget:          54          <- new today
ok  VaCuusRender/FVaCuusRmlDocumentHost:  0 exported members
ok  VaCuusRender/FVaCuusSlateElement:     0 exported members   <- new today
ok  exported JS_* symbols: 0
RESULT: clean
```

Module totals moved: `VaCuus` 385 → **389**, `VaCuusRender` 143 → **221**, `VaCuusJs` 2,
`VaCuusRml` 1746, `VaCuusEditor` 2. The +78 on `VaCuusRender` is `SVaCuusWidget` plus the two
factories plus whatever the 38-commit delta since `fc38ced` added.

**macOS** has **no gate script** — `Tools/api_export_check.sh` is ELF-only (`nm -D`,
`libUnrealEditor-<Module>.so`) and macOS modules are `.dylib`. The gate's own rule was therefore
applied by hand (`nm -gU | c++filt | grep -c 'Class::'`) against
`libUnrealEditor-VaCuusRender.dylib` and `-VaCuus.dylib`:

```
SVaCuusWidget            44        UVaCuusView       56
UVaCuusWidget            21        UVaCuusSubsystem  38
UVaCuusWorldComponent    47        UVaCuusStyleSet    7
FVaCuusRmlDocumentHost    0
FVaCuusSlateElement       0
```

Both legs hold on Mach-O too. **Follow-up filed** for the missing script — a hand-run check is
evidence for one commit, not a gate.

### The Mach-O detail that will mislead the next reader

A first attempt counted raw `nm` hits by mangled class name and reported `FVaCuusSlateElement: 3`
and `FVaCuusRmlDocumentHost: 1` — which looks like the façade leaking. It is not. Demangled, the
hits are:

```
SVaCuusWidget::Construct(..., TSharedRef<FVaCuusSlateElement> const&)      <- parameter mention
VaCuusSlateView::MakeDocumentHost(TSharedRef<FVaCuusSlateElement> const&)  <- parameter mention
vtable for FVaCuusSlateElement                                             <- vtable, not a member
vtable for FVaCuusRmlDocumentHost                                          <- vtable, not a member
```

The two parameter mentions are exactly what the `.sh` gate's comment warns about — a class used
as a parameter carries no `::`, which is why the gate matches `Class::` and not the bare name.
The two vtables are a real symbol-table fact and a real non-problem: the class *definitions* stay
in `Private/`, so nothing outside the module can name either type, let alone construct one. A
Mach-O gate should count `Class::` like its siblings and say this out loud, because the naive
grep reads it as a violation.

## What this did NOT cover

1. **`Tools/api_export_check.sh` was not executed on Linux this pass.** It wants a
   `RunUAT BuildPlugin` package with `Binaries/`, and no package was built today. Its list was
   edited (`SVaCuusWidget` in, `FVaCuusSlateElement` in FORBIDDEN); Win64's twin ran with the
   same list and is green, but the ELF leg is unverified on this commit.
2. **The VaCuusDemo project was not built on macOS.** Linux and Win64 both have it (see §4);
   the Mac does not, so the consumer-module compile is unverified there.
3. **No GPU rows.** This is a build/suite/export re-verification. The manual matrix rows (glass,
   world panel, IME, Retina) are the August passports' business and were not re-run.

## 4. The consumer module — VaCuusDemo on Win64

This is the sharpest test of the akj.25 surface and the reason it is worth its own section: the
lobby demo now lives in a **different project, a different module and a different repository**,
so it compiles against `SVaCuusWidget.h` and `VaCuusSlateView.h` and nothing else. If the public
surface were short of anything, this is where it shows up as a compile or link error.

Brought over as a git bundle plus its LFS store (the box has no GitHub credentials that work
non-interactively), given its **own** plugin clone at `C:\VaCuusDemo\Plugins\VaCuus` rather than
being pointed at VcHost's — deliberately, to avoid the shared-intermediate/orphan-binary trap
described in `VaCuus-a1k`.

| | |
|---|---|
| Build | `VaCuusDemoEditor Win64 Development` — **Succeeded**, 649.41 s, 250 actions from scratch |
| Warnings | **0**, across the whole 284-line log |
| Run | `-game -RenderOffscreen`, `vacuus.LobbyDemo` — chrome view 2 over content view 1 at 1280×720, all four PT Sans faces loaded, `lobby.rml` and `chrome.rml` served from the project's own DevUI root |
| RHI | D3D12, **SM6** |

Document roots resolved as designed — `C:/VaCuusDemo/Plugins/VaCuus/Content/DevUI` then
`C:/VaCuusDemo/Content/DevUI` — which is the plugin-first order `VaCuusContentPaths.h` specifies,
with the project root supplying every lobby document.

**`VaCuus-akj.24` did NOT reproduce here.** The async-upload handover ensure
(`IsWellFormedPayload`, `1024x1024, 0 bytes`) fires on **every** lobby run on Linux/Vulkan and
fired **zero** times across this Win64/D3D12 run. That is evidence for the bead's stated
hypothesis rather than against it: the trigger needs two *pending* buffers carrying the same
texture handle in one drain, which is a queue-timing state, not an unconditional defect. Noted
on the bead.
