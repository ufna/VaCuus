# Engine textures in the document

**Date:** 2026-08-22
**Status:** implemented and verified 2026-08-22 (UE 5.8.1). Three claims below were corrected
against the source and the running engine during implementation; each correction is marked
**CORRECTED**.
**Beads:** filed with this document

## The problem

A VaCuus document can show an image only if that image is a **file** the VFS can open and
the recorder can decode — PNG, JPEG, UEJPEG, through
`FVaCuusRecordingRenderInterface::LoadTexture` (`VaCuusRecordingRenderInterface.cpp:270`).
Nothing that lives as a `UTexture` can appear in the UI at all: not a cooked texture asset,
not a `UMediaTexture`, and — the one people ask for — not a `UTextureRenderTarget2D` fed by
a `SceneCapture2D`.

That gap has a price already paid. The 2d6 demo drew its "3D scene" as seven pre-rendered
PNG plates, 7.5 MB of a 9.9 MB bundle (`VaCuus-9ak.4`), because live 3D in the document was
not available. Character portraits, minimaps, security-camera feeds, video — the whole
Gameface-class "live view" family — are all the same missing seam.

The reverse direction already exists and works: `FVaCuusWorldSink` copies a view's replayed
pixels *into* a `UTextureRenderTarget2D` for world panels. This spec is the other arrow.

## Non-goals, decided rather than deferred

- **Relayout on texture resize.** RmlUi caches per-entry dimensions in
  `FileTextureDatabase` and refreshes them nowhere; `EnsureLoaded`
  (`Source/ThirdParty/RmlUi/Source/Core/TextureDatabase.cpp:118-130`) re-enters
  `LoadTextureEntry` only while the handle is still 0. So the contract is: **CSS sizes the
  element, the texture's dimensions are a first-layout hint.** Changing this means
  releasing and re-fetching the RmlUi texture entry, which is a separate piece of work.
- **Asset paths in `src`.** `<img src="unreal:///Game/UI/T_X.T_X">` would put asset
  resolution on the UI thread, which is the crash class `VaCuus-akj.6.12` already closed
  once (`LoadModuleChecked` from `LoadTexture`), and gives runtime-created render targets no
  stable name anyway. Registration by key instead.
- **Composite-time draws.** The material spike priced this and chose the forced-republish
  clamp (`VaCuusMaterialDraw.h:34-41`). Same answer here, same reasons.
- **`border-radius` on an image.** **CORRECTED, TWICE.** It does not round an image:
  `ElementImage::GenerateGeometry` builds a plain quad (`ElementImage.cpp:180`) and so does
  the image decorator (`DecoratorTiled.cpp:202`); the radius shapes only background and border
  geometry.

  The first correction stopped there and concluded "a circular portrait is not expressible",
  **which is wrong** — it treated shape as a geometry problem. It is a **coverage** problem,
  and coverage is the alpha channel. Write the mask into the texture's alpha, register with an
  alpha mode that means it (`Straight` or `Premultiplied` — the overrides already exist), and
  the premultiplied composite does the rest with **no plugin support of any kind**. A
  `SceneCapture2D` that writes coverage into its render target's alpha gets a round portrait
  the same way. Proven on screen: the demo's icon tile is a smooth-edged disc produced by
  nothing but alpha.

  RmlUi's own general mechanism for this is `mask-image` — a real RCSS property
  (`StyleSheetSpecification.cpp:404`) applied through `SaveLayerAsMaskImage`
  (`ElementEffects.cpp:296-311`) — and VaCuus refuses it loudly because the replayer has no
  layer render targets and compiles no filter but `blur`. That is already tracked as
  `VaCuus-iuv` and blocked on the same layer capture as `VaCuus-u0q`; it is a different
  feature from anything in this spec. `transform`, `opacity` and `image-color` compose today,
  and the demo shows them.
- **Writing back into an engine texture from the document.** One direction only.
- **Mip generation / streaming policy for registered textures.** The registered object owns
  its own resource; VaCuus binds it and nothing more.

## 1. The naming seam

Markup:

```html
<img src="unreal://portrait"/>
<div style="decorator: image(unreal://minimap)"/>
```

Game side:

```cpp
Subsystem->RegisterTexture(TEXT("portrait"), PortraitRT, /*bLive=*/true);
Subsystem->MarkTextureDirty(TEXT("minimap"));
Subsystem->UnregisterTexture(TEXT("portrait"));
```

### The scheme survives RmlUi's path joining by RmlUi's own rule

`ElementImage::LoadTexture` hands `src` to `RenderManager::LoadTexture(source, document_path)`
(`Elements/ElementImage.cpp:251`), which joins it against the document directory
(`RenderManager.cpp:74-82`) unless the source starts with `?`. The join runs through
`SystemInterface::JoinPath`, and **that function returns the path verbatim when a colon
occurs before the first slash** (`SystemInterface.cpp:62-68`) — its rule for Windows drive
paths, `C:/…`. For `unreal://portrait` the colon is at 6 and the first slash at 7, so
`6 < 7` and the string arrives at `LoadTexture` untouched.

So no `JoinPath` override is needed, and VaCuus deliberately does not have one today. The
`://` form is chosen over the also-working `unreal:portrait` because it reads as a URL, and
over RmlUi's `?` escape hatch because `?` carries no name.

### The trap that dictates the resolution order

`FileTextureDatabase::LoadTextureEntry` latches `load_texture_failed = true` on a zero
handle (`TextureDatabase.cpp:106-113`), and `EnsureLoaded` never retries a latched entry
(`:118-130`). A document that loads before the game registers its key would therefore show
nothing **forever**, with one warning at load and silence after.

Therefore: **`LoadTexture` mints a handle for any syntactically valid `unreal://<key>`,
registered or not, and resolution is deferred to replay.** This is the shape the async image
decode already uses — mint the handle, hand out a placeholder, fill it in later
(`VaCuusCommandBuffer.h:222-227`).

### Stable ids are hashed from the key, not minted by the registry

The id the recorder writes into the command buffer is a 64-bit hash of the key string.
Computed identically on any thread, it lets the UI thread name a key the game has never
mentioned, with no registry round-trip and no lock. A collision is caught where it can be
caught — on the game thread at registration, when an id is already held by a different key —
and is refused by name, the discipline `FVaCuusStyleRegistry::RegisterStyleSet` already
follows (`VaCuusStyleSet.h:98-107`).

## 2. Three threads, the style-registry shape

`FVaCuusTextureRegistry` mirrors `FVaCuusStyleRegistry` (`VaCuusStyleSet.h:80-92`) closely
enough that the same reader can check both, and deliberately so.

- **Game thread (registration rate).** Validate; root the `UTexture` in a
  `TStrongObjectPtr`; keep its `FTextureReference*` (**CORRECTED** — not the RHI reference,
  see below); mirror `{id → binding}` to the render thread with `ENQUEUE_RENDER_COMMAND`,
  which resolves the reference there; publish an
  immutable snapshot `{key → {id, Size, bLive, Encoding, Alpha}}` to the UI thread over the
  existing command queue. Unregistration parks the root behind a render fence begun after
  the mirror replacement — `UnregisterStyleSet`'s deferred-release discipline verbatim.
- **UI thread (per `LoadTexture`).** A map lookup in the installed snapshot, and a handle
  minted whether or not it hits.
- **Render thread (per draw).** `Textures.Find` misses → `ExternalTextures.Find(handle)` →
  id → mirror → bind. One extra branch at the single existing texture-bind site
  (`VaCuusReplayRenderer.cpp:1126-1136`).

### Why `TextureReferenceRHI` and not the resource's texture

`FRHITextureReference : public FRHITexture` (`RHITextureReference.h:7`), so it drops into
the replayer's existing `TMap<FVaCuusTextureHandle, FTextureRHIRef>`
(`VaCuusReplayRenderer.h:482`) with no type change and binds as an ordinary SRV.

`FTextureResource::TextureReferenceRHI` is documented as "a FRHITextureReference to update
whenever the FTexture::TextureRHI changes… It allows to prevent dereferencing the UAsset
pointers when updating a texture resource" (`TextureResource.h:169-171`), and
`SetReferencedTexture` is reachable only from `FDynamicRHI::RHIUpdateTextureReference`
(`RHITextureReference.h:47-51`). So a render target that resizes, or a streamed texture
that swaps mips, keeps the same binding — the property `UVaCuusWorldComponent.h:40` already
leans on for its material.

And the failure mode is bounded: a reference with nothing behind it resolves to a global
black texture (`RHITextureReference.h:60-65`), so a torn-down registration draws black
rather than crashing.

**CORRECTED — where the reference is taken, and what registration actually checks.** The
design said the game thread reads `TextureReferenceRHI` at registration. It cannot:
`FTextureReference::InitRHI` runs on the **render thread**, so immediately after a caller's
`UpdateResource()` that field is still null on the game thread, and a registry that read it
there would refuse every texture registered in the same function that created it. This was
invisible under `-nullrhi` and surfaced on the first real-RHI test run.

So the mirror command reads the reference **on the render thread**, and FIFO is what makes
that sound: `UTexture`'s constructor already enqueued the reference's init
(`Texture.cpp:205-208`), and both commands come from the game thread. The entry carries an
`FTextureReference*`, safe to dereference there because the entry roots the owning `UTexture`
and unregistration replaces the mirror before it fences.

What registration checks instead is `UTexture::GetResource()`, which `UpdateResource()` sets
inline (`Texture.cpp:336-339`) — the synchronous, game-thread-truthful form of the same
question. A caller who never ran `UpdateResource()` is refused by name.

**And the refusal is conditional on `FApp::CanEverRender()`**, because `UpdateResource()`
creates a resource only under it (`Texture.cpp:336`). In a `-nullrhi` process, a dedicated
server or most commandlets, *no* texture has a resource — refusing there would refuse
everything for a reason that has nothing to do with the caller, and nothing draws anyway.

## 3. Liveness, and the idle gate

A live render target changes its pixels without changing one byte of the command stream —
invisible to both terms of the idle gate, the content hash and the resource-traffic
predicate. Left alone, a document showing a portrait would publish once and freeze.

This is exactly the material-decorator problem, already solved once
(`VaCuusRecordingRenderInterface.cpp:1626-1665`), and the solution is reused rather than
reinvented:

- Per-recorder `LiveExternalTextures` — the twin of `LiveMaterialShaders`
  (`VaCuusRecordingRenderInterface.h:389-399`). Non-empty is this view's forced-republish
  flag. **Per view**, so one view's portrait cannot reopen the idle row for every other view.
- **Clamped to engine rate** on `GFrameCounter`, for the reason the material clamp gives: the
  composite samples the RT once per engine frame, so a second replay inside one frame is
  pure cost.
- Kill-switch `vacuus.ExternalTextureForcedRepublish` (default 1), so the freeze is
  observable and the remedy is testable.

Three modes, and the default is the cheap one:

| registration | cost while nothing else moves |
|---|---|
| static (default) | zero publishes — the idle gate is untouched |
| `MarkTextureDirty(key)` | exactly one publish |
| `bLive = true` | one publish per engine frame, clamped |

`bLive` is sugar for "dirty every frame" and is written that way, so there is one mechanism
and one place it can be wrong.

## 4. Colour and alpha, derived rather than asked

The pipeline stores **sRGB-encoded premultiplied** bytes in the view RT and decodes at
composite time (`VaCuusUIShaders.h:52-65`). UI textures are created `PF_R8G8B8A8` with no
sRGB flag (`VaCuusReplayRenderer.cpp:227-232`), so the sampler hands the shader raw bytes and
the contract holds by construction.

An engine texture breaks both halves:

- An sRGB-tagged texture is **decoded by the sampler**, so the shader receives linear values
  where the pipeline expects encoded ones — a visibly washed-out image.
- A `SceneCapture2D` render target typically carries `alpha = 0`. Under the premultiplied
  blend that is a fully transparent image, i.e. the feature appears not to work at all.

So an external binding carries two enums, `Encoding` (`Raw` | `EncodeFromLinear`) and
`Alpha` (`Opaque` | `Premultiplied` | `Straight`), applied by the pixel shader as uniforms
beside the existing `bUseTexture` — no permutation, and no cost on the ordinary path.

Both are **derived on the game thread from the texture itself** (`UTexture::SRGB` and the
pixel format) at registration, with an explicit override on the registration call for the
caller who knows better. Defaults chosen so the common case — a `SceneCapture2D` RT and a
cooked `UTexture2D` — is correct with no arguments.

## 5. Observables

An invariant with no observable cannot be tested and will rot (`CLAUDE.md`), and three of
this design's claims have no natural observable at all:

- `FVaCuusTextureRegistry::GetNumEntries_GameThread()` / `GetNumPendingReleases_GameThread()`
  / `GetVersion_GameThread()` — the style registry's trio, same meanings.
- `NumUnresolvedExternalDraws` on the replayer — a draw whose id resolves to nothing. Without
  it, "unregistered draws black rather than crashing" is unassertable.
- `vacuus.TextureRegistry` — dumps key, id, size, mode, resolved/unresolved.

## 6. The demo

`vacuus.TexDemo`, in the shape of `vacuus.WorldDemo` (`VaCuusWorldDemo.cpp`), loading one
document `Content/DevUI/tex_demo.rml` with three tiles:

| tile | source | mode | what it proves |
|---|---|---|---|
| portrait | `SceneCapture2D` on a rotating mesh, `CaptureEveryFrame` | `bLive` | a live RT under a `transform` (**CORRECTED**: not `border-radius` — see the non-goals) |
| minimap | `SceneCapture2D` overhead, captured at 1 Hz | `MarkTextureDirty` | refresh on demand, not at 60 Hz; tinted through `image-color` |
| icon | a plain `UTexture2D` with a disc in its alpha | static | "any UTexture"; **zero** publishes; and that shape comes from coverage, not geometry |

The icon is a `UTexture2D::CreateTransient` pattern rather than a new `.uasset`: no binaries
added to the repository, no cook-config change, and it proves the claim that matters — an
ordinary texture, not a render target. Swapping it for a real asset later is a one-line
change.

A counter row (published / recorded / skipped) sits under the tiles, because the headline
property — the static tile costs nothing — has to be visible on the same screenshot as the
live one.

## 7. Tests, each with its restore-the-bug

`VaCuus.Render.ExternalTexture.*`:

1. **UnknownKeyMintsHandle** — register *after* the document loads and assert the image
   resolves. Break: return 0 for an unknown key; the image must stay dead after
   registration, which is the RmlUi latch doing its documented thing.
2. **StaticCostsNothing** — a static registered texture drawn in an idle document: the
   publish counter does not advance past the first. Break: mark it live; the counter runs.
3. **LiveForcesRepublish** — publishes track engine frames, one per frame, never two. Break:
   `vacuus.ExternalTextureForcedRepublish 0`; the freeze is observable.
4. **MarkDirtyPublishesOnce** — exactly one additional publish per call, no more.
5. **UnregisterDrawsBlackNotCrash** — a draw naming an unregistered id increments
   `NumUnresolvedExternalDraws`, logs once, and binds the default black.
6. **Registration** — the named refusals, the derived `Auto` modes, and that re-registering a
   key **keeps its id** (without which a document already drawing the key would follow a swap
   nowhere).

**Not covered, said out loud rather than left as a gap:** the hash-collision refusal. A
CityHash64 collision cannot be constructed on demand, so that branch has only its counter
(`GetNumCollisionsRefused_GameThread`) as an observable, asserted to stay zero.

**Venue.** `UnresolvedDrawCounts` needs the replay draw pass, which returns before its first
draw when global shaders are unavailable (`VaCuusReplayRenderer.cpp:932`) — i.e. every
`-nullrhi` run. It self-skips loudly there, the contract
`VaCuus.Render.Composite.LinearOutputGPU` already states, and is exercised in a real-RHI run.

## Implementation order

1. `FVaCuusTextureRegistry` + subsystem doors + observables (game thread only, testable alone).
2. `LoadTexture` scheme interception, handle minting, `NewExternalTextures` on the buffer.
3. Replayer resolution + shader uniforms for encoding/alpha.
4. Liveness: `LiveExternalTextures`, the clamp, the kill-switch, `MarkTextureDirty`.
5. Tests 1-6.
6. `vacuus.TexDemo` + `tex_demo.rml` + the counter row.
7. Editor build, monolithic `-game` build, automation run, headless screenshot.

## Verification record (2026-08-22, UE 5.8.1, Linux)

- Editor target and monolithic `VcHost` game target both build.
- `VaCuus.Render.ExternalTexture.*`: **7/7** under `-nullrhi`, **7/7** against a real RHI.
- Full suite `Automation RunTests VaCuus`: **242 passed, 0 failed** — no regressions.
- Restore-the-bug, one break at a time, each failing exactly its own test and nothing else:
  - `LoadTexture` refusing an unregistered key → only `UnknownKeyMintsHandle` fails.
  - the liveness term keyed off loaded rather than drawn textures → only `UndrawnCostsNothing`
    fails, on the assertion it exists for.
- Headless screenshots of `vacuus.TexDemo`, same document, one switch apart:
  - portrait live: **307 recorded / 306 published / 1 withheld**
  - portrait static (`vacuus.TexDemo.Live 0`): **294 recorded / 6 published / 288 withheld**
- Auto-derived modes observed in the log: an `RTF_RGBA8` capture → `Raw, Opaque`; an sRGB
  `UTexture2D` → `EncodeFromLinear, Straight`, and its ring renders back at exactly the
  `#88C0D0` written into it — the encode round trip is identity.
- Re-registration observed to keep the id (`15815917570512957247`) and add no entry.
