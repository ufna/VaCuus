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

enum class EVaCuusCommandType : uint8
{
	DrawGeometry,
	SetScissor,
	DisableScissor,
	SetTransform
};

/** One recorded RmlUi render call. Fields beyond Type are per-command payload. */
struct FVaCuusCommand
{
	EVaCuusCommandType Type = EVaCuusCommandType::DrawGeometry;

	/** DrawGeometry: geometry created in this or an earlier buffer. */
	FVaCuusGeometryHandle Geometry = 0;

	/** DrawGeometry: texture to sample; 0 = untextured. */
	FVaCuusTextureHandle Texture = 0;

	/** DrawGeometry: pixel-space translation applied to the geometry. */
	FVector2f Translation = FVector2f::ZeroVector;

	/** SetScissor: clip rect in window coordinates (unaffected by SetTransform). */
	FIntRect Scissor = FIntRect(0, 0, 0, 0);

	/** SetTransform: vertex transform in UE row-vector convention (v' = v * M). */
	FMatrix44f Transform = FMatrix44f::Identity;
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
 * bytes of padding at offsets 1..7. It shifts NOTHING -- sizeof stays 112 and every other
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

constexpr bool bCommandHasExactlySixMembers =
	bTakesInitialisers<FVaCuusCommand, FAny, FAny, FAny, FAny, FAny, FAny> &&
	!bTakesInitialisers<FVaCuusCommand, FAny, FAny, FAny, FAny, FAny, FAny, FAny>;
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

	/** Handles to release AFTER this buffer retires (commands above may still use them). */
	TArray<FVaCuusGeometryHandle> ReleasedGeometry;
	TArray<FVaCuusTextureHandle> ReleasedTextures;

	/**
	 * Does this buffer carry resource traffic the replayer must see even when the drawing
	 * did not change? The idle gate's wake condition, and the reason it lives HERE.
	 *
	 * ADD A RESOURCE ARRAY ABOVE AND YOU MUST ADD IT TO THIS PREDICATE. The four arrays
	 * are the only channel by which created and released resources reach the replayer, and
	 * they are cleared with the buffer; a buffer the gate withholds is dropped. So an array
	 * this predicate does not name is a resource that silently never arrives -- or a release
	 * nobody consumes -- on any frame where the commands happened not to change.
	 *
	 * NOT A HYPOTHETICAL GAP. The recorder implements 9 of Rml::RenderInterface's 21
	 * virtuals; the 12 it leaves at their defaults include CompileShader/RenderShader/
	 * ReleaseShader (ThirdParty/RmlUi/Include/RmlUi/Core/RenderInterface.h:131-140, how
	 * RmlUi 6 draws `decorator: linear-gradient`), CompileFilter/ReleaseFilter (:122-125,
	 * `filter:` and `backdrop-filter:`) and EnableClipMask/RenderToClipMask (:78-86,
	 * rounded-corner clipping). Each of those arrives with its own handle map on this
	 * buffer, and a document with an animated gradient whose NewShaders map went unnamed
	 * here would publish frame one and then freeze.
	 *
	 * The compile-time half of that promise is the member-count assert in the definition
	 * below: a new member on this struct fails the build until someone decides whether it
	 * is resource traffic. The comment is what tells them which answer means what.
	 */
	bool HasResourceTraffic() const;
};

namespace VaCuusLayout
{
constexpr bool bBufferHasExactlySevenMembers =
	bTakesInitialisers<FVaCuusCommandBuffer, FAny, FAny, FAny, FAny, FAny, FAny, FAny> &&
	!bTakesInitialisers<FVaCuusCommandBuffer, FAny, FAny, FAny, FAny, FAny, FAny, FAny, FAny>;
} // namespace VaCuusLayout

inline bool FVaCuusCommandBuffer::HasResourceTraffic() const
{
	// THE COMPLETENESS TRIPWIRE for the four arrays this predicate names. Unlike the frame
	// hash's, this one has nothing else to fall back on: a resource array is not read as
	// bytes, so no sizeof or offsetof clause would notice it, and no existing test exercises
	// a resource type that does not exist yet. The member count is the whole guard.
	//
	// It fires on ANY new member, resource or not, which is the point -- the only two
	// answers are "add it to the return below" and "it is not resource traffic, bump the
	// count". Both are deliberate; neither is silent.
	//
	// Verified by restoring the bug rather than argued: adding a fifth resource map to the
	// struct without naming it below fails this assert at compile time, with this message.
	static_assert(VaCuusLayout::bBufferHasExactlySevenMembers,
		"FVaCuusCommandBuffer gained or lost a member. If it is a resource array (a new handle map or "
		"release list), add it to HasResourceTraffic() or the idle gate will withhold frames that carry "
		"it. If it is frame content the replayer draws from, add it to VaCuusHashFrameContent() too. "
		"Then update this count");

	return NewGeometry.Num() > 0 || NewTextures.Num() > 0 || ReleasedGeometry.Num() > 0 ||
		ReleasedTextures.Num() > 0;
}

/**
 * Padding-free mirror of one FVaCuusCommand, built only to be fed to the frame hash
 * below as raw bytes.
 *
 * WHY IT EXISTS -- FVaCuusCommand ITSELF MUST NEVER BE HASHED AS BYTES. Its first
 * member is a uint8 enum and its last is an FMatrix44f, which is alignas(16)
 * (Math/Matrix.h:42, and the M array again at :49), so the struct is 112 bytes with
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
};

static_assert(sizeof(FVaCuusCommandHashImage) ==
		3 * sizeof(uint64) + 2 * sizeof(float) + 4 * sizeof(int32) + 16 * sizeof(float),
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
 * WHAT IS OUT, and why neither can cause a false "unchanged":
 *  - Generation: identifies the buffer, not its content. Hashing it would make every
 *    frame differ from every other and the gate would never fire.
 *  - NewGeometry / NewTextures / ReleasedGeometry / ReleasedTextures: resource traffic
 *    the replayer must see even when the drawing did not change. The gate tests them
 *    for emptiness DIRECTLY (FVaCuusCommandBuffer::HasResourceTraffic) instead of
 *    hashing their payloads -- cheaper (no megabyte texture walked per frame) and safer,
 *    since "any traffic at all forces a publish" cannot be fooled by a hash collision.
 *
 * Every FVaCuusCommand field is hashed, including the ones a given command type does
 * not use (Scissor on a DrawGeometry, say). Those are default-initialised by the
 * struct's NSDMIs and never written, so they contribute a constant -- hashing them
 * costs nothing and removes the need to reason about which field matters to which
 * type. Floats are hashed BITWISE, so +0.0 and -0.0 read as a change; that direction
 * is the safe one (a redundant publish, never a missed one).
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
	// alone: swap Texture and Translation and sizeof is still 112 with six members, but the
	// nine hand-written scalar copies below would be reading the wrong fields.
	//
	// Either way the failure being prevented is the same one, and it is the worst kind this
	// feature can have: a field that never reaches FVaCuusCommandHashImage cannot make the
	// hash differ, so a frame that changed reads as "unchanged" and is withheld -- a UI
	// frozen on stale pixels, with nothing logged anywhere.
	static_assert(VaCuusLayout::bCommandHasExactlySixMembers,
		"FVaCuusCommand gained or lost a field. Every field that reaches the replayer must be copied into "
		"FVaCuusCommandHashImage below, or an unhashed field silently stops triggering a publish");
	static_assert(sizeof(FVaCuusCommand) == 112 && offsetof(FVaCuusCommand, Type) == 0 &&
			offsetof(FVaCuusCommand, Geometry) == 8 && offsetof(FVaCuusCommand, Texture) == 16 &&
			offsetof(FVaCuusCommand, Translation) == 24 && offsetof(FVaCuusCommand, Scissor) == 32 &&
			offsetof(FVaCuusCommand, Transform) == 48,
		"FVaCuusCommand's layout changed. Every field that reaches the replayer must be copied into "
		"FVaCuusCommandHashImage below, or an unhashed field silently stops triggering a publish");

	// XXH3, streaming (FXxHash64Builder, Hash/xxhash.h:204; Update is CORE_API at :215-216,
	// i.e. out of line).
	//
	// ONE Update PER COMMAND, over the padding-free image below, rather than one per field --
	// and the reason is completeness, not speed. Hashing the image as a single blob is the
	// only form the COMPILER can check: the static_assert above proves the image has no gaps,
	// so every byte between its first and last member is a field that was copied in. A
	// sequence of per-field Update calls has no such check -- drop one call and the hash
	// simply stops seeing that field, which is the exact failure the tripwires above exist to
	// prevent, reintroduced one level down.
	//
	// The call-overhead argument that used to lead here was WRONG IN DIRECTION and is gone:
	// adding the whole hash moved Record from 0.039-0.043 ms to 0.040-0.041 ms, i.e. below
	// the noise floor, so ~500 extra calls a frame certainly are too. Nothing here is
	// justified on measured cost.
	FXxHash64Builder Builder;

	// Frame header. The command count is redundant today (fixed-size records mean
	// equal byte lengths imply equal counts) and is here so it stays correct if a
	// variable-length field is ever added to the image. It follows that NO TEST CAN PIN IT
	// until that day: appending a command lengthens the stream by a whole image either way.
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

		Builder.Update(&Image, sizeof(Image));
	}

	return Builder.Finalize().Hash;
}
