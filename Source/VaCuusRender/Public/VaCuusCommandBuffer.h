// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Hash/xxhash.h"

/**
 * Dense 1-based ids minted by the recorder; 0 = invalid/none.
 * Handles are shared with RmlUi verbatim: the recorder returns them as
 * Rml::CompiledGeometryHandle / Rml::TextureHandle (uintptr_t, 64-bit on all
 * target platforms) and gets them back in draw and release calls, so the
 * full 64-bit range round-trips without truncation.
 */
using FVaCuusGeometryHandle = uint64;
using FVaCuusTextureHandle = uint64;

/**
 * Filter handles follow the geometry pattern exactly: minted strictly increasing,
 * never recycled, round-tripped through Rml::CompiledFilterHandle (uintptr_t)
 * unchanged. Filters are cross-frame resources — RmlUi holds a compiled filter
 * until ReleaseFilter (RenderInterface.h:122-125).
 */
using FVaCuusFilterHandle = uint64;

/**
 * Shader handles are the filter pattern again: strictly increasing, never recycled,
 * round-tripped through Rml::CompiledShaderHandle (uintptr_t) unchanged. Cross-frame
 * resources — RmlUi holds a compiled shader until ReleaseShader
 * (RenderInterface.h:131-140), and re-parameterising a gradient has no mutate call:
 * the decorator releases and compiles a fresh handle (DecoratorShader.cpp:27-53 via
 * ElementEffects::ReloadEffectsData, ElementEffects.cpp:133-148). That is what makes
 * the idle-gate story below complete: a changed gradient is ALWAYS new-handle
 * resource traffic AND a changed Shader field in its draw command.
 */
using FVaCuusShaderHandle = uint64;

/**
 * Layer handles are NOT the geometry pattern, and the difference is load-bearing.
 * RmlUi's layer stack is strictly per-frame: asserted empty at every frame start
 * (RmlUi Source/Core/RenderManager.cpp:51) and there is no release call — PopLayer
 * IS the end of a layer's life (RenderInterface.h:106-107). So the recorder restarts
 * the counter at 1 every BeginFrame(): two identical frames then record identical
 * handles and hash equal. A cross-frame monotonic counter would make every frame of
 * a static glass HUD hash-unique and the idle gate would never withhold it again.
 * Handle 0 stays reserved for the base layer (RenderInterface.h:96).
 */
using FVaCuusLayerHandle = uint64;

/** Mirror of Rml::BlendMode; numeric values pinned by static_asserts in the recorder. */
enum class EVaCuusBlendMode : uint8
{
	Blend,
	Replace
};

/** Mirror of Rml::ClipMaskOperation; numeric values pinned by static_asserts in the recorder. */
enum class EVaCuusClipMaskOp : uint8
{
	Set,
	SetInverse,
	Intersect
};

enum class EVaCuusCommandType : uint8
{
	DrawGeometry,
	SetScissor,
	DisableScissor,
	SetTransform,

	/** M5 glass (spec §2(d)): the layer/filter/clip-mask vocabulary backdrop-filter arrives in. */
	PushLayer,
	PopLayer,
	CompositeLayers,
	EnableClipMask,
	RenderToClipMask,

	/**
	 * M5 decorators stage 1 (spec §2(e)): draw Geometry filled by the compiled shader in
	 * Shader instead of by Texture/vertex colour alone — RmlUi's RenderShader
	 * (RenderInterface.h:137), how `decorator: linear/radial/conic-gradient` and
	 * `decorator: shader(<builtin>)` reach the replayer.
	 */
	DrawShader
};

/** One recorded RmlUi render call. Fields beyond Type are per-command payload. */
struct FVaCuusCommand
{
	EVaCuusCommandType Type = EVaCuusCommandType::DrawGeometry;

	/** DrawGeometry, RenderToClipMask: geometry created in this or an earlier buffer. */
	FVaCuusGeometryHandle Geometry = 0;

	/** DrawGeometry: texture to sample; 0 = untextured. */
	FVaCuusTextureHandle Texture = 0;

	/** DrawGeometry, RenderToClipMask: pixel-space translation applied to the geometry. */
	FVector2f Translation = FVector2f::ZeroVector;

	/** SetScissor: clip rect in window coordinates (unaffected by SetTransform). */
	FIntRect Scissor = FIntRect(0, 0, 0, 0);

	/** SetTransform: vertex transform in UE row-vector convention (v' = v * M). */
	FMatrix44f Transform = FMatrix44f::Identity;

	/** PushLayer: the handle minted for the new layer. CompositeLayers: the source layer. */
	FVaCuusLayerHandle SourceLayer = 0;

	/** CompositeLayers: the destination layer; 0 = the base layer (the per-view RT). */
	FVaCuusLayerHandle DestLayer = 0;

	/** DrawShader: the compiled shader (NewShaders desc) the geometry is filled with. */
	FVaCuusShaderHandle Shader = 0;

	/**
	 * CompositeLayers: this command's filter list, THE VARIABLE-LENGTH RECORD, stored as a
	 * slice [FilterOffset, FilterOffset + FilterCount) of the buffer's CompositeFilters
	 * array. Offset+count rather than an inline TArray so FVaCuusCommand stays a
	 * fixed-size aggregate the layout tripwires below can pin; the HASH still covers the
	 * slice's CONTENTS, not just its shape — see VaCuusHashFrameContent.
	 */
	int32 FilterOffset = 0;
	int32 FilterCount = 0;

	/** CompositeLayers: how the (filtered) source blends onto the destination. */
	EVaCuusBlendMode Blend = EVaCuusBlendMode::Blend;

	/** RenderToClipMask: how the geometry modifies the mask. */
	EVaCuusClipMaskOp ClipMaskOp = EVaCuusClipMaskOp::Set;

	/** EnableClipMask: 1 = draws are masked from here on, 0 = mask off. */
	uint8 bClipMaskEnable = 0;
};

/**
 * MEMBER COUNTS OF THE TWO TYPES THE IDLE GATE READS, and the only thing that can actually
 * detect a field added to either of them.
 *
 * FVaCuusCommand feeds the tripwire in VaCuusHashFrameContent() below; FVaCuusCommandBuffer
 * feeds the one in FVaCuusCommandBuffer::HasResourceTraffic(). Both guard the same failure:
 * a field the gate cannot see, and therefore a frame that changed being withheld.
 *
 * WHY sizeof AND offsetof ARE NOT ENOUGH, measured rather than reasoned about: declaring a
 * uint8, uint16 or uint32 between FVaCuusCommand's Type and Geometry puts it in the seven
 * bytes of padding at offsets 1..7. It shifts NOTHING -- sizeof stays 160 and every other
 * member keeps its offset -- so a layout tripwire built only from sizeof and offsetof
 * compiles it silently, however many offsets it pins. The field then never reaches
 * FVaCuusCommandHashImage, the hash cannot see it change, and the gate reports "unchanged"
 * for a frame that is not: a UI frozen on stale pixels with no diagnostic anywhere. That is
 * the one failure mode the idle short-circuit must not have, so it is asserted directly.
 *
 * Both types are aggregates (no user-declared constructors, no bases, no private members;
 * default member initialisers do not change that in C++14 and later, and neither do member
 * FUNCTIONS -- which is why HasResourceTraffic() can live on the buffer without disarming
 * the check that guards it). So their member counts are observable: brace initialisation
 * accepts at most one initialiser per member, and one too many is ill-formed in the
 * immediate context of a requires-expression. No brace elision can absorb the extra
 * initialiser either, because no member of either type is itself an aggregate or an array
 * (FIntPoint, FVector2f, FIntRect, FMatrix44f, TArray and TMap all declare constructors).
 *
 * For FVaCuusCommand the offsetof clauses still earn their place -- they catch a REORDER,
 * which keeps both sizeof and the member count intact. The buffer needs no such clause: it
 * is never read as bytes, only by member name.
 */
namespace VaCuusLayout
{
/**
 * Stands in for any member's type: converts to whatever that member happens to be, so the
 * counts below do not have to be kept in step with the member TYPES as well.
 *
 * Declared and never defined, which is all it needs: it is only ever named inside the
 * unevaluated requires-expressions below.
 */
struct FAny
{
	template <typename T>
	constexpr operator T() const;
};

/** Can TAggregate be brace-initialised from exactly this many initialisers? */
template <typename TAggregate, typename... TInitialisers>
constexpr bool bTakesInitialisers = requires { TAggregate{TInitialisers{}...}; };

constexpr bool bCommandHasExactlyFourteenMembers =
	bTakesInitialisers<FVaCuusCommand, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny> &&
	!bTakesInitialisers<FVaCuusCommand, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny>;
} // namespace VaCuusLayout

/**
 * Bit-identical mirror of Rml::Vertex (Vector2f position, ColourbPremultiplied
 * colour, Vector2f tex_coord); the recorder memcpy's the vertex stream.
 *
 * Color keeps RmlUi's in-memory byte order: R,G,B,A with premultiplied alpha.
 * On little-endian platforms FColor's named channels read those bytes as
 * B,G,R,A, so R and B appear swapped through the FColor API — the replayer
 * must upload as RGBA (e.g. PF_R8G8B8A8) or swizzle at upload time.
 */
struct FVaCuusVertex
{
	FVector2f Position;
	FColor Color;
	FVector2f UV;
};

struct FVaCuusGeometryData
{
	TArray<FVaCuusVertex> Vertices;
	TArray<int32> Indices;
};

/**
 * Raw RGBA8 pixels, RmlUi byte order, ALWAYS premultiplied alpha: generated
 * textures (fonts) per the Rml contract, loaded images premultiplied at decode
 * by the recorder. Upload as PF_R8G8B8A8 and blend One/InverseSourceAlpha.
 *
 * The recorder ENFORCES the alpha half of this contract rather than assuming it —
 * formats whose decoder leaves the A byte undefined (JPEG) are stamped opaque at the
 * decode boundary. See FVaCuusRecordingRenderInterface::LoadTexture.
 *
 * A 1x1 (0,0,0,0) payload is the async-load placeholder: LoadTexture returns the
 * real dimensions immediately but the decode runs on a worker, so the handle
 * carries one premultiplied-transparent texel until the payload arrives. That
 * makes an unfinished image draw INVISIBLE rather than missing, which is why the
 * placeholder is a real entry and not an absent one.
 */
struct FVaCuusTextureData
{
	FIntPoint Size = FIntPoint::ZeroValue;
	TArray<uint8> RGBA;
};

/**
 * Payload of one compiled filter. Blur-only in v1 by policy (M5 spec §2(d)): the
 * recorder's CompileFilter returns handle 0 for every other filter type, so an entry
 * in NewFilters is always a blur and Sigma is its whole payload.
 *
 * Sigma is RmlUi's resolved value verbatim: FilterBlur::CompileFilter passes
 * ResolveLength(sigma_value) — the blur() length itself, no 0.5 factor — under the
 * key "sigma" (RmlUi Source/Core/FilterBlur.cpp:16-20). So `blur(12px)` at dp-ratio
 * 1.0 records Sigma == 12.0. A struct rather than a bare float so the shader tier
 * (M5 Track S) and any future filter payload have a place to grow.
 */
struct FVaCuusFilterData
{
	float Sigma = 0.0f;
};

/**
 * One engine texture referenced by this frame: a `<img src="unreal://<key>">` the
 * recorder minted a handle for (spec 2026-08-22).
 *
 * THE PAYLOAD IS AN ID, NOT PIXELS, and that is the whole difference from
 * FVaCuusTextureData. Nothing is uploaded and nothing is copied: the render thread
 * resolves StableId through FVaCuusTextureRegistry's mirror to the registered UTexture's
 * stable RHI reference and binds that. So a 4K render target costs this buffer 8 bytes.
 *
 * ENCODING AND ALPHA ARE DELIBERATELY NOT HERE. They are properties of the REGISTRATION,
 * they live in the render-thread mirror beside the reference, and keeping them there means
 * re-registering a key with a different mode takes effect without any recorder having to
 * notice — the recorder has nothing to re-record.
 *
 * MINTED FOR AN UNREGISTERED KEY TOO. See FVaCuusRecordingRenderInterface::LoadTexture:
 * RmlUi latches a failed texture entry forever, so refusing here would be permanent.
 */
struct FVaCuusExternalTextureDesc
{
	/** FVaCuusTextureRegistry::IdForKey(key). Never 0. */
	uint64 StableId = 0;
};

/**
 * Which of the four CompileShader names a desc records (M5 spec §2(e)). RmlUi 6 sends
 * exactly four: "linear-gradient" (DecoratorGradient.cpp:252-259), "radial-gradient"
 * (:422-428), "conic-gradient" (:619-625) and "shader" (DecoratorShader.cpp:35). The
 * recorder refuses any other name — and any "shader" whose value is neither a builtin
 * nor a registered style-set key — with handle 0, which suppresses exactly ONE
 * decorator on ONE element (Decorator.h:42-44; the per-entry guard in
 * ElementEffects.cpp:196-200), so there is no Unknown kind here: an unknown key never
 * mints a desc at all.
 */
enum class EVaCuusShaderKind : uint8
{
	LinearGradient,
	RadialGradient,
	ConicGradient,

	/** decorator: shader(<key>): a VaCuus builtin pixel shader; BuiltinKey names it. */
	Builtin,

	/**
	 * decorator: shader(<key>) where key resolved in the UVaCuusStyleSet snapshot
	 * (M5 Task 5b): an MD_UI material drawn by the FVaCuusMaterialVS/PS pair.
	 * MaterialId carries the resolved stable id; BuiltinKey carries the key for
	 * diagnostics. Kind is fixed at compile time: builtins win over style keys, so a
	 * key can never mean both.
	 */
	Material
};

/**
 * One RESOLVED gradient color stop. RmlUi resolves every stop before CompileShader:
 * positions arrive normalized to Unit::NUMBER along the gradient line — lengths,
 * percentages and angles are all converted and auto positions distributed by
 * ResolveColorStops (DecoratorGradient.cpp:27-122, asserted all-NUMBER at :119) — and
 * colors arrive as ColourbPremultiplied (DecorationTypes.h:8-11). Recorded as floats so
 * the replayer can hand the array to the gradient PS without a per-draw conversion.
 */
struct FVaCuusColorStop
{
	/** Premultiplied RGBA in 0..1 (ColourbPremultiplied byte / 255). */
	FVector4f Color = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);

	/** Position along the gradient line, ~0..1, strictly non-decreasing across the list. */
	float Position = 0.0f;
};

/**
 * The most stops one compiled gradient carries — the gradient PS's uniform-array size.
 * 16 matches RmlUi's own reference backends (MAX_NUM_STOPS,
 * Backends/RmlUi_Renderer_GL3.cpp:40), whose CompileShader truncates the same way
 * (Math::Min at :1635): RmlUi core does NOT cap the list (ResolveColorStops keeps every
 * stop), so the recorder truncates to the first 16 — the reference behavior — and,
 * unlike the reference, says so once in the log.
 */
inline constexpr int32 VaCuusMaxGradientStops = 16;

/**
 * Payload of one compiled shader: the CompileShader dictionary, recorded verbatim
 * (material-decorators.md §1's table, every field opened at its DecoratorGradient.cpp /
 * DecoratorShader.cpp site). Which fields are meaningful depends on Kind; the rest keep
 * their defaults, exactly like FVaCuusCommand's per-type payload fields.
 *
 * COORDINATE SPACE, the one fact the replayer must not guess: the three gradient
 * decorators build meshes whose tex_coord is the vertex position minus the paint-box
 * offset — LOCAL PIXELS (DecoratorGradient.cpp:268-270, :437-439, :634-636) — and
 * P0/P1/Center/Radius are in that same space. The "shader" decorator instead normalizes
 * tex_coord to 0..1 over Dimensions (DecoratorShader.cpp:45-47), which is why Builtin
 * carries Dimensions: a builtin PS that wants pixel units multiplies them back.
 */
struct FVaCuusShaderDesc
{
	EVaCuusShaderKind Kind = EVaCuusShaderKind::LinearGradient;

	/** LinearGradient: gradient line start/end, local px ("p0"/"p1"). */
	FVector2f P0 = FVector2f::ZeroVector;
	FVector2f P1 = FVector2f::ZeroVector;

	/** RadialGradient + ConicGradient: gradient center, local px ("center"). */
	FVector2f Center = FVector2f::ZeroVector;

	/** RadialGradient: per-axis ellipse radius, px ("radius"). */
	FVector2f Radius = FVector2f::ZeroVector;

	/** ConicGradient: start angle, radians ("angle"). */
	float Angle = 0.0f;

	/** LinearGradient: gradient line length, px ("length"). Stop resolution used it; the PS derives its own from P0/P1. */
	float Length = 0.0f;

	/** Gradients: repeating-* variant ("repeating"). */
	uint8 bRepeating = 0;

	/** Gradients: the resolved stop list ("color_stop_list"), truncated to VaCuusMaxGradientStops. */
	TArray<FVaCuusColorStop> Stops;

	/** Builtin + Material: the shader(<key>) registry key ("value"), known-valid — unknown keys never mint a desc. */
	FString BuiltinKey;

	/** Builtin + Material: paint-box size in px ("dimensions") — the tex_coord normalization factor. */
	FVector2f Dimensions = FVector2f::ZeroVector;

	/**
	 * Material: the style registry's stable id, resolved from the UI-thread snapshot at
	 * compile time and resolved again to an FMaterialRenderProxy on the render thread
	 * (FVaCuusStyleRegistry::ResolveProxy_RenderThread). An id the mirror no longer
	 * carries (the key was unregistered under a live draw) skips with a latched log —
	 * the named refusal, never a stale pointer: the mirror drops the proxy BEFORE the
	 * game-side root can (the fence in FVaCuusStyleRegistry::UnregisterStyleSet).
	 *
	 * DELIBERATELY NOT A TRIPWIRE CHANGE, checked rather than assumed: FVaCuusShaderDesc
	 * is not one of the two layout-guarded types (FVaCuusCommand / FVaCuusCommandBuffer
	 * — see VaCuusLayout), because a desc is resource PAYLOAD: it rides NewShaders,
	 * which HasResourceTraffic() already names, and payloads are covered by the traffic
	 * leg, not the hash (the NewGeometry/NewTextures argument in VaCuusHashFrameContent
	 * applies verbatim — a desc is immutable once compiled, RmlUi has no mutate call, so
	 * a changed material decorator is always a new handle: hash leg AND traffic leg).
	 */
	uint64 MaterialId = 0;
};

/**
 * One recorded UI frame: the command list plus the resource delta since the
 * previous buffer. Produced on the game thread by FVaCuusRecordingRenderInterface,
 * consumed by the render-thread replayer.
 *
 * A handle may appear in both NewGeometry/NewTextures and the Released* arrays
 * of the same buffer (created and released within one frame): the replayer
 * must create the resource, play the commands, then retire it.
 */
struct FVaCuusCommandBuffer
{
	/** Strictly increasing publish counter; newer buffers replace older ones. */
	uint64 Generation = 0;

	/** View size the frame was laid out for, in pixels. */
	FIntPoint ViewSize = FIntPoint::ZeroValue;

	TArray<FVaCuusCommand> Commands;

	/** Geometry first seen this frame, keyed by handle. */
	TMap<FVaCuusGeometryHandle, FVaCuusGeometryData> NewGeometry;

	/**
	 * Textures first seen this frame, keyed by handle.
	 *
	 * NewTextures is also the arrival channel for a finished async image decode,
	 * so ONE texture handle may appear in NewTextures of two different buffers:
	 * the placeholder in the buffer that ran LoadTexture, the real payload in a
	 * later one. The replayer's TMap<Handle, FTextureRHIRef>::Add on an existing
	 * key destroys the old value before relocating the new one in
	 * (Containers/SetUtilities.h:98, MoveByRelocate), so re-adding IS the swap:
	 * the placeholder's RHI ref is released and no handle is orphaned.
	 *
	 * The two buffers are usually consecutive but need not be, and can even be the
	 * same one: see FVaCuusRecordingRenderInterface::DrainCompletedDecodes.
	 */
	TMap<FVaCuusTextureHandle, FVaCuusTextureData> NewTextures;

	/**
	 * Filters first seen this frame, keyed by handle. Blur-only in v1: every entry is a
	 * blur and carries its sigma — see FVaCuusFilterData. Resource traffic exactly like
	 * NewGeometry: a sigma change releases the old filter and compiles a new handle
	 * (RmlUi has no mutate-a-compiled-filter API, RenderInterface.h:118-125), so it is
	 * double-covered at the idle gate — new handle in the command's filter list (hash)
	 * AND an entry here (traffic).
	 */
	TMap<FVaCuusFilterHandle, FVaCuusFilterData> NewFilters;

	/**
	 * Shaders first seen this frame, keyed by handle — the map this header's own warning
	 * below spent two milestones predicting. Resource traffic exactly like NewFilters: a
	 * gradient parameter change releases the old shader and compiles a new handle (no
	 * mutate API exists — see FVaCuusShaderHandle), so an animated gradient is
	 * double-covered at the idle gate: new handle in its DrawShader command (hash) AND an
	 * entry here (traffic).
	 */
	TMap<FVaCuusShaderHandle, FVaCuusShaderDesc> NewShaders;

	/**
	 * Engine textures first named this frame, keyed by the same handle space as
	 * NewTextures — the recorder mints both from NextTextureHandle, because RmlUi cannot
	 * tell them apart and must not have to.
	 *
	 * RELEASES RIDE ReleasedTextures, NOT A LIST OF THEIR OWN, for that same reason:
	 * RmlUi calls ReleaseTexture(handle) with no idea which kind it is, so one release
	 * list is the honest shape. The replayer removes the handle from both of its maps.
	 *
	 * Resource traffic like every map above it — an entry here means a handle the replayer
	 * has never seen, so the buffer carrying it must publish.
	 */
	TMap<FVaCuusTextureHandle, FVaCuusExternalTextureDesc> NewExternalTextures;

	/** Handles to release AFTER this buffer retires (commands above may still use them). */
	TArray<FVaCuusGeometryHandle> ReleasedGeometry;
	TArray<FVaCuusTextureHandle> ReleasedTextures;
	TArray<FVaCuusFilterHandle> ReleasedFilters;
	TArray<FVaCuusShaderHandle> ReleasedShaders;

	/**
	 * The concatenated filter lists of every CompositeLayers command in this buffer, in
	 * record order; each command slices it by FilterOffset/FilterCount. FRAME CONTENT,
	 * not resource traffic: this is what the composite draws WITH, so it is covered by
	 * VaCuusHashFrameContent (the slice bytes are hashed per command), not by
	 * HasResourceTraffic(). Cleared with the buffer like Commands.
	 */
	TArray<FVaCuusFilterHandle> CompositeFilters;

	/**
	 * Does this buffer carry resource traffic the replayer must see even when the drawing
	 * did not change? The idle gate's wake condition, and the reason it lives HERE.
	 *
	 * ADD A RESOURCE ARRAY ABOVE AND YOU MUST ADD IT TO THIS PREDICATE. The eight arrays
	 * are the only channel by which created and released resources reach the replayer, and
	 * they are cleared with the buffer; a buffer the gate withholds is dropped. So an array
	 * this predicate does not name is a resource that silently never arrives -- or a release
	 * nobody consumes -- on any frame where the commands happened not to change.
	 *
	 * NOT A HYPOTHETICAL GAP — this predicate has now been caught up TWICE, each time by
	 * its own earlier warning: the M5 filter pair, and then the M5 shader pair whose
	 * absence the previous revision of this comment predicted by name ("a document with an
	 * animated gradient whose NewShaders map went unnamed here would publish frame one and
	 * then freeze"). The recorder now overrides ALL 21 of Rml::RenderInterface's virtuals, so
	 * nothing is left at a silent default. The last two — SaveLayerAsTexture and
	 * SaveLayerAsMaskImage (ThirdParty/RmlUi/Include/RmlUi/Core/RenderInterface.h:112-116) —
	 * are REFUSALS, not recordings: each returns 0 with one latched warning and adds no
	 * resource array, so neither touches this predicate. They land here the day layer capture
	 * is implemented, each with its own handle map. CompositeFilters is deliberately NOT here:
	 * it is frame content (see its declaration), covered by the hash.
	 *
	 * A PREVIOUS REVISION OF THIS COMMENT described those two as "element `filter:` output and
	 * mask-image", which was wrong about the first half and is corrected here (bead
	 * VaCuus-u0q): SaveLayerAsTexture's only caller in the whole tree is the box-shadow texture
	 * callback (ThirdParty/RmlUi/Source/Core/GeometryBoxShadow.cpp:235). Element `filter:` goes
	 * through CompositeLayers (ElementEffects.cpp:283-315), which IS implemented. Naming the
	 * wrong property is how box-shadow's white-rectangle failure stayed invisible for two
	 * milestones — nothing under Source/VaCuus* mentioned it at all.
	 *
	 * The compile-time half of that promise is the member-count assert in the definition
	 * below: a new member on this struct fails the build until someone decides whether it
	 * is resource traffic. The comment is what tells them which answer means what.
	 */
	bool HasResourceTraffic() const;
};

namespace VaCuusLayout
{
constexpr bool bBufferHasExactlyThirteenMembers =
	bTakesInitialisers<FVaCuusCommandBuffer, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny> &&
	!bTakesInitialisers<FVaCuusCommandBuffer, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny>;
} // namespace VaCuusLayout

inline bool FVaCuusCommandBuffer::HasResourceTraffic() const
{
	// THE COMPLETENESS TRIPWIRE for the eight arrays this predicate names. Unlike the frame
	// hash's, this one has nothing else to fall back on: a resource array is not read as
	// bytes, so no sizeof or offsetof clause would notice it, and no existing test exercises
	// a resource type that does not exist yet. The member count is the whole guard.
	//
	// It fires on ANY new member, resource or not, which is the point -- the only two
	// answers are "add it to the return below" and "it is not resource traffic, bump the
	// count". Both are deliberate; neither is silent.
	//
	// Verified by restoring the bug rather than argued: adding a resource map to the
	// struct without naming it below fails this assert at compile time, with this message —
	// re-verified when the M5 filter pair landed (7 -> 10) and again when the M5 shader
	// pair landed (10 -> 12), the count bumped in the same change that extended the return
	// each time, and again when NewExternalTextures landed (12 -> 13) — exactly the ceremony
	// this assert exists to force.
	static_assert(VaCuusLayout::bBufferHasExactlyThirteenMembers,
		"FVaCuusCommandBuffer gained or lost a member. If it is a resource array (a new handle map or "
		"release list), add it to HasResourceTraffic() or the idle gate will withhold frames that carry "
		"it. If it is frame content the replayer draws from, add it to VaCuusHashFrameContent() too. "
		"Then update this count");

	return NewGeometry.Num() > 0 || NewTextures.Num() > 0 || NewFilters.Num() > 0 || NewShaders.Num() > 0 ||
		NewExternalTextures.Num() > 0 || ReleasedGeometry.Num() > 0 || ReleasedTextures.Num() > 0 ||
		ReleasedFilters.Num() > 0 || ReleasedShaders.Num() > 0;
}

/**
 * Padding-free mirror of one FVaCuusCommand, built only to be fed to the frame hash
 * below as raw bytes.
 *
 * WHY IT EXISTS -- FVaCuusCommand ITSELF MUST NEVER BE HASHED AS BYTES. Its first
 * member is a uint8 enum and it carries an FMatrix44f, which is alignas(16)
 * (Math/Matrix.h:42, and the M array again at :49), so the struct is 160 bytes with
 * SEVEN bytes of padding sitting between Type and Geometry. No constructor writes
 * padding, so those bytes hold whatever the allocator left behind: hashing the object
 * would give a different hash for two identical frames, and a different one on every
 * run. That is not a theoretical concern here -- the hash decides whether a frame is
 * published, so nondeterministic padding means spurious "changed" frames forever.
 *
 * This mirror gives every hashed field its own 8-byte-aligned slot and lets the
 * compiler CHECK that the result is gap-free (the static_assert below), which is
 * stronger than a comment promising it. Field order here is arbitrary; it only has to
 * be stable within a process, since a hash is never compared across runs.
 */
struct FVaCuusCommandHashImage
{
	uint64 Type = 0;
	uint64 Geometry = 0;
	uint64 Texture = 0;
	float Translation[2] = {0.0f, 0.0f};
	int32 Scissor[4] = {0, 0, 0, 0};
	float Transform[16] = {};
	uint64 SourceLayer = 0;
	uint64 DestLayer = 0;
	uint64 Shader = 0;

	// FilterOffset and FilterCount are both hashed even though only the count is content
	// in its own right (the offset is bookkeeping into CompositeFilters, deterministic
	// given the command list) — uniformity is the discipline here: every FVaCuusCommand
	// field gets a slot, so nobody has to re-derive per field whether skipping it is safe.
	// The count doubles as the length prefix that keeps the variable-length stream
	// unambiguous; the slice's CONTENTS are hashed separately in VaCuusHashFrameContent.
	int32 FilterOffset = 0;
	int32 FilterCount = 0;

	// The three byte-sized command fields widen to uint64 slots: a no-padding image is the
	// property the static_assert below checks, and three u8s would need five explicit pad
	// bytes someone could forget to zero.
	uint64 Blend = 0;
	uint64 ClipMaskOp = 0;
	uint64 ClipMaskEnable = 0;
};

static_assert(sizeof(FVaCuusCommandHashImage) ==
		9 * sizeof(uint64) + 2 * sizeof(float) + 6 * sizeof(int32) + 16 * sizeof(float),
	"FVaCuusCommandHashImage must have no padding: VaCuusHashFrameContent hashes it as raw bytes");

/**
 * Content hash of one recorded frame: everything in the buffer that decides what the
 * replayer DRAWS, and nothing else.
 *
 * WHAT IS IN: the command list, field by field, plus ViewSize. ViewSize is hashed
 * rather than compared separately because it is content in exactly the same sense as
 * a command -- it sizes the render target the commands are replayed into, so a resize
 * that happens to produce a byte-identical command list (a full-screen fill, a view
 * that shrinks with everything clipped) has to read as a CHANGED frame or the RT never
 * resizes and the composite stretches stale pixels forever. One number to compare, and
 * no second condition to forget at the gate.
 *
 * WHAT IS OUT, and why none of it can cause a false "unchanged":
 *  - Generation: identifies the buffer, not its content. Hashing it would make every
 *    frame differ from every other and the gate would never fire.
 *  - NewGeometry / NewTextures / NewFilters / NewShaders / ReleasedGeometry /
 *    ReleasedTextures / ReleasedFilters / ReleasedShaders: resource traffic the replayer
 *    must see even when the drawing did not change. The gate tests them for emptiness DIRECTLY
 *    (FVaCuusCommandBuffer::HasResourceTraffic) instead of hashing their payloads --
 *    cheaper (no megabyte texture walked per frame) and safer, since "any traffic at
 *    all forces a publish" cannot be fooled by a hash collision.
 *  - VERTEX COLOURS, which are the interesting case, because they are the one thing a
 *    frame can change with no command difference at all. They live inside NewGeometry's
 *    payloads, so the bullet above is what covers them -- see the argument below, which is
 *    the only claim in this header that rests on code we do not own.
 *
 * WHY A COLOUR-ONLY RESTYLE STILL PUBLISHES -- `div:hover { background-color: ... }`, which
 * moves nothing this function reads.
 *
 * THE STRUCTURAL REASON, and it is stronger than "RmlUi happens to re-compile": Rml's render
 * interface has no operation that mutates an already-compiled geometry. Its whole geometry
 * vocabulary is CompileGeometry / RenderGeometry / ReleaseGeometry
 * (ThirdParty/RmlUi/Include/RmlUi/Core/RenderInterface.h:40-48), plus RenderToClipMask (:86)
 * and RenderShader (:137), which only CONSUME a handle. New vertex colours therefore cannot
 * reach the replayer at all except as a fresh CompileGeometry -- which lands in NewGeometry,
 * which is resource traffic, which forces the publish. The division of labour is complete by
 * construction rather than by luck: this hash covers what the replayer draws WITH (the
 * command list), HasResourceTraffic() covers what it draws (the payloads), and there is no
 * third channel into the replayer.
 *
 * The recorder holds up its end of it: CompileGeometry mints a strictly increasing handle and
 * ReleaseGeometry never recycles one (VaCuusRecordingRenderInterface.cpp:140, :173-181), so a
 * re-compile also changes the Geometry field of every command that draws with it and this
 * hash sees the change on its own. Double-covered; either leg alone would suffice.
 *
 * TEXTURES ARE THE ONE PLACE A HANDLE'S CONTENT REALLY DOES CHANGE UNDER A STABLE HANDLE --
 * the async image decode swaps the real payload in for the 1x1 placeholder without minting a
 * new handle (see FVaCuusCommandBuffer::NewTextures). It arrives through NewTextures, i.e.
 * through the traffic predicate. That case is what the traffic leg is FOR, and it is why
 * "resource traffic" is not merely a bookkeeping concern.
 *
 * WHAT RMLUI 6.x ACTUALLY DOES, opened and checked so nobody has to re-derive it:
 *  - backgrounds and borders: ElementBackgroundBorder::GenerateGeometry does
 *    geometry.Release(ClearMesh) and then render_manager->MakeGeometry(...) unconditionally
 *    (ThirdParty/RmlUi/Source/Core/ElementBackgroundBorder.cpp:131-137). Release reaches
 *    render_interface->ReleaseGeometry via RenderManager::ReleaseResource
 *    (Source/Core/RenderManager.cpp:345-354), and the compile is deferred to the first draw
 *    (:205-206), i.e. to Context::Render().
 *  - text: a colour change sets geometry_dirty (Source/Core/ElementText.cpp:425-428), and the
 *    regeneration REUSES existing geometry when the mesh compares equal
 *    (ElementText.cpp:530-539) -- so it is the mesh comparison that decides this case, and it
 *    is colour-sensitive: Mesh::operator== compares vertices and Vertex::operator== compares
 *    `colour` (Include/RmlUi/Core/Mesh.h:14, Include/RmlUi/Core/Vertex.h:20-23). A recoloured
 *    string never matches, and the assignment that replaces it move-assigns onto a live
 *    UniqueRenderResource, which releases the old handle first
 *    (Include/RmlUi/Core/UniqueRenderResource.h:30-35).
 *
 * RmlUi also contains something that LOOKS like the counter-example, named here so it is not
 * mistaken for one later: ElementText::OnPropertyChange re-colours the text-decoration mesh
 * IN PLACE -- Release(), overwrite every vertex.colour, put it back (ElementText.cpp:430-439).
 * The "in place" is in RmlUi's CPU-side Rml::Mesh; the put-back is MakeGeometry, so the render
 * interface still sees a release and a new compile. It could not be otherwise, for want of an
 * API to do anything else.
 *
 * NOTHING HERE CAN BE A static_assert. The two below guard OUR structs; no compile-time check
 * can speak for a vendored library's next version. The substitute is one end-to-end test that
 * drives a real :hover colour change through a real Rml::Context and requires the publish --
 * VaCuus.Render.IdleGate.HoverRecolour. Every other idle-gate test drives the recorder
 * directly (and says so at the top of that file), so none of them would notice either leg
 * above breaking.
 *
 * Every FVaCuusCommand field is hashed, including the ones a given command type does
 * not use (Scissor on a DrawGeometry, say). Those are default-initialised by the
 * struct's NSDMIs and never written, so they contribute a constant -- hashing them
 * costs nothing and removes the need to reason about which field matters to which
 * type. Floats are hashed BITWISE, so +0.0 and -0.0 read as a change; that direction
 * is the safe one (a redundant publish, never a missed one).
 *
 * THE SHADER FIELD IS THE HASH'S OWN LEG FOR DECORATORS, not redundancy with the
 * traffic leg: two shaders compiled in an EARLIER buffer and swapped between draws
 * later (a restyle toggling between two live decorators) is a frame with zero resource
 * traffic whose only difference is the Shader handle in a DrawShader command. Drop
 * Image.Shader below and that frame hashes "unchanged" and is withheld — the
 * restore-the-bug for exactly that is VaCuus.Render.Decorator.ShaderHash.
 */
inline uint64 VaCuusHashFrameContent(const FVaCuusCommandBuffer& Buffer)
{
	// THE COMPLETENESS TRIPWIRE, in two halves because neither is sufficient on its own.
	//
	// The member count catches an ADDED or REMOVED field wherever it is declared, including
	// the case sizeof and offsetof provably cannot see -- a small scalar tucked into the
	// seven bytes of padding at offsets 1..7, which shifts nothing at all. See VaCuusLayout
	// above; that hole was verified by building with such a field present, not argued.
	//
	// The offsets catch a REORDER, which leaves both the total size and the member count
	// alone: swap Texture and Translation and sizeof is still 160 with the same member
	// count, but the hand-written scalar copies below would be reading the wrong fields.
	//
	// Either way the failure being prevented is the same one, and it is the worst kind this
	// feature can have: a field that never reaches FVaCuusCommandHashImage cannot make the
	// hash differ, so a frame that changed reads as "unchanged" and is withheld -- a UI
	// frozen on stale pixels, with nothing logged anywhere.
	static_assert(VaCuusLayout::bCommandHasExactlyFourteenMembers,
		"FVaCuusCommand gained or lost a field. Every field that reaches the replayer must be copied into "
		"FVaCuusCommandHashImage below, or an unhashed field silently stops triggering a publish");
	static_assert(sizeof(FVaCuusCommand) == 160 && offsetof(FVaCuusCommand, Type) == 0 &&
			offsetof(FVaCuusCommand, Geometry) == 8 && offsetof(FVaCuusCommand, Texture) == 16 &&
			offsetof(FVaCuusCommand, Translation) == 24 && offsetof(FVaCuusCommand, Scissor) == 32 &&
			offsetof(FVaCuusCommand, Transform) == 48 && offsetof(FVaCuusCommand, SourceLayer) == 112 &&
			offsetof(FVaCuusCommand, DestLayer) == 120 && offsetof(FVaCuusCommand, Shader) == 128 &&
			offsetof(FVaCuusCommand, FilterOffset) == 136 && offsetof(FVaCuusCommand, FilterCount) == 140 &&
			offsetof(FVaCuusCommand, Blend) == 144 && offsetof(FVaCuusCommand, ClipMaskOp) == 145 &&
			offsetof(FVaCuusCommand, bClipMaskEnable) == 146,
		"FVaCuusCommand's layout changed. Every field that reaches the replayer must be copied into "
		"FVaCuusCommandHashImage below, or an unhashed field silently stops triggering a publish");

	// XXH3, streaming (FXxHash64Builder, Hash/xxhash.h:204; Update is CORE_API at :215-216,
	// i.e. out of line).
	//
	// ONE Update PER COMMAND over the padding-free image below (plus one for a command's
	// variable-length filter slice, which by nature cannot live in a fixed image), rather
	// than one per field -- and the reason is completeness, not speed. Hashing the image as
	// a single blob is the only form the COMPILER can check: the static_assert above proves
	// the image has no gaps, so every byte between its first and last member is a field
	// that was copied in. A sequence of per-field Update calls has no such check -- drop
	// one call and the hash simply stops seeing that field, which is the exact failure the
	// tripwires above exist to prevent, reintroduced one level down.
	//
	// The call-overhead argument that used to lead here was WRONG IN DIRECTION and is gone:
	// adding the whole hash moved Record from 0.039-0.043 ms to 0.040-0.041 ms, i.e. below
	// the noise floor, so ~500 extra calls a frame certainly are too. Nothing here is
	// justified on measured cost.
	FXxHash64Builder Builder;

	// Frame header. The command count stopped being redundant when the M5 filter lists
	// made records variable-length — the day the old comment here said would come. Each
	// record is now one image plus FilterCount trailing handles, so equal stream lengths
	// no longer imply equal command counts (one command with 20 filter handles is as long
	// as two with none), and the count is what keeps two such streams from ever needing a
	// byte-level coincidence argument.
	const uint64 Header[3] = {uint64(uint32(Buffer.ViewSize.X)), uint64(uint32(Buffer.ViewSize.Y)), uint64(Buffer.Commands.Num())};
	Builder.Update(Header, sizeof(Header));

	for (const FVaCuusCommand& Command : Buffer.Commands)
	{
		FVaCuusCommandHashImage Image;
		Image.Type = uint64(Command.Type);
		Image.Geometry = Command.Geometry;
		Image.Texture = Command.Texture;
		Image.Translation[0] = Command.Translation.X;
		Image.Translation[1] = Command.Translation.Y;
		Image.Scissor[0] = Command.Scissor.Min.X;
		Image.Scissor[1] = Command.Scissor.Min.Y;
		Image.Scissor[2] = Command.Scissor.Max.X;
		Image.Scissor[3] = Command.Scissor.Max.Y;
		FMemory::Memcpy(Image.Transform, Command.Transform.M, sizeof(Image.Transform));
		Image.SourceLayer = Command.SourceLayer;
		Image.DestLayer = Command.DestLayer;
		Image.Shader = Command.Shader;
		Image.FilterOffset = Command.FilterOffset;
		Image.FilterCount = Command.FilterCount;
		Image.Blend = uint64(Command.Blend);
		Image.ClipMaskOp = uint64(Command.ClipMaskOp);
		Image.ClipMaskEnable = uint64(Command.bClipMaskEnable);

		Builder.Update(&Image, sizeof(Image));

		// THE VARIABLE-LENGTH HALF OF THE RECORD: a CompositeLayers command's filter list
		// CONTENTS, not just its offset/count shape. This is what lets the idle gate see a
		// sigma change on its command-list leg — a new sigma is a new filter handle (the
		// recorder never recycles them), so the slice bytes differ even when the list's
		// length does not. Drop this Update and a composite that swapped one blur for
		// another would hash "unchanged"; the restore-the-bug for exactly that is
		// VaCuus.Render.Glass.FilterListHash. Unambiguous by construction: FilterCount
		// rides inside the image just hashed, so every handle byte in the stream is
		// length-prefixed.
		if (Command.FilterCount > 0)
		{
			check(Command.FilterOffset >= 0 &&
				Command.FilterOffset + Command.FilterCount <= Buffer.CompositeFilters.Num());
			Builder.Update(Buffer.CompositeFilters.GetData() + Command.FilterOffset,
				Command.FilterCount * sizeof(FVaCuusFilterHandle));
		}
	}

	return Builder.Finalize().Hash;
}
