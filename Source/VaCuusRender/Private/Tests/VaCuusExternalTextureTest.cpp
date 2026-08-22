// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusDefines.h"
#include "VaCuusRecordingRenderInterface.h"
#include "VaCuusReplayRenderer.h"
#include "VaCuusTextureRegistry.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>
#include <RmlUi/Core/Vertex.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * ENGINE TEXTURES IN THE DOCUMENT (spec 2026-08-22): `<img src="unreal://<key>">`.
 *
 * WHY MOST OF THESE DRIVE THE RECORDER DIRECTLY, with no Rml::Context: every property here is
 * about the seam's own decisions -- what LoadTexture mints, what the idle gate does with it,
 * what the replayer binds -- and none of them is about RmlUi's layout. The one thing that IS
 * about RmlUi (that a failed texture entry is latched forever, which is why an unregistered
 * key must still mint a handle) is documented at the site it forced, LoadTexture's scheme
 * branch, and is what UnknownKeyMintsHandle exists to hold in place.
 *
 * THE SNAPSHOTS COME FROM THE REAL REGISTRY, NEVER HAND-BUILT, and that is not fussiness.
 * FVaCuusTextureRegistry::InstallSnapshot checkf's that versions never regress, and the
 * registry's counter is PROCESS-WIDE and never resets. A test that minted its own version
 * numbers would either collide with the registry's or, worse, race ahead of them and make a
 * later real registration fire that checkf -- a failure in some other test entirely.
 */
namespace VaCuusExternalTextureTest
{
static const FIntPoint GViewSize(800, 600);

/** Any valid geometry payload; nothing here depends on its contents. */
struct FTriangle
{
	Rml::Vertex Vertices[3] = {};
	int Indices[3] = {0, 1, 2};

	FTriangle()
	{
		Vertices[0].position = Rml::Vector2f(10.f, 20.f);
		Vertices[1].position = Rml::Vector2f(30.f, 40.f);
		Vertices[2].position = Rml::Vector2f(50.f, 60.f);
	}

	Rml::Span<const Rml::Vertex> VertexSpan() const { return Rml::Span<const Rml::Vertex>(Vertices, 3); }
	Rml::Span<const int> IndexSpan() const { return Rml::Span<const int>(Indices, 3); }
};

/** A 4x4 transient colour texture: an ordinary UTexture2D, the "cooked asset" case. */
static UTexture2D* MakeAssetTexture()
{
	UTexture2D* Texture = UTexture2D::CreateTransient(4, 4, PF_B8G8R8A8);
	if (!Texture)
	{
		return nullptr;
	}
	Texture->SRGB = true;
	Texture->UpdateResource();
	return Texture;
}

/** A 64x32 render target: the SceneCapture case, and the one whose Auto modes differ. */
static UTextureRenderTarget2D* MakeRenderTarget()
{
	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>();
	RenderTarget->RenderTargetFormat = RTF_RGBA8;
	RenderTarget->ClearColor = FLinearColor::Black;
	RenderTarget->InitAutoFormat(64, 32);
	RenderTarget->UpdateResourceImmediate(true);
	return RenderTarget;
}

/**
 * The UI-thread install the production drain would do. Called with the registry's OWN
 * snapshot, so versions stay the registry's -- see the file comment.
 */
static void InstallCurrent()
{
	if (TSharedPtr<const FVaCuusTextureSnapshot> Snapshot = FVaCuusTextureRegistry::GetSnapshot_GameThread())
	{
		FVaCuusTextureRegistry::InstallSnapshot(Snapshot);
	}
}

/** One recorded frame that draws Geometry with Texture. Returns the published buffer, or null. */
static TUniquePtr<FVaCuusCommandBuffer> RecordDraw(
	FVaCuusRecordingRenderInterface& Recorder, Rml::CompiledGeometryHandle Geometry, Rml::TextureHandle Texture)
{
	Recorder.BeginFrame(GViewSize);
	Recorder.RenderGeometry(Geometry, Rml::Vector2f(1.f, 2.f), Texture);
	return Recorder.EndFrameAndPublish();
}
} // namespace VaCuusExternalTextureTest

/**
 * THE LATCH TRAP, held in place (spec 2026-08-22 §1).
 *
 * RmlUi's FileTextureDatabase sets load_texture_failed on a zero handle
 * (TextureDatabase.cpp:106-113) and EnsureLoaded never retries a latched entry (:118-130).
 * So "return 0 until the game registers the key" would mean a document that loaded first is
 * dead FOREVER, with one warning and silence after. The seam must therefore mint a handle for
 * a key nobody has registered, and this is that assertion.
 *
 * RESTORE-THE-BUG: make LoadTexture's scheme branch return Rml::TextureHandle(0) when the
 * installed snapshot has no binding for the key. This test then fails on its first
 * TestNotEqual, and -- the part that matters -- a real document with a late registration
 * never recovers.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusExternalTextureUnknownKeyTest, "VaCuus.Render.ExternalTexture.UnknownKeyMintsHandle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusExternalTextureUnknownKeyTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusExternalTextureTest;

	FVaCuusRecordingRenderInterface Recorder;
	const FString Key = TEXT("vacuus-test-never-registered");

	Recorder.BeginFrame(GViewSize);
	Rml::Vector2i Dimensions(0, 0);
	const Rml::TextureHandle Handle = Recorder.LoadTexture(Dimensions, "unreal://vacuus-test-never-registered");

	TestNotEqual(TEXT("An unregistered key still mints a handle — RmlUi latches a zero forever"),
		uint64(Handle), uint64(0));
	TestEqual(TEXT("...laid out at the 1x1 placeholder, not collapsed to nothing"), Dimensions.x, 1);
	TestEqual(TEXT("...in both axes"), Dimensions.y, 1);

	Recorder.RenderGeometry(Recorder.CompileGeometry(FTriangle().VertexSpan(), FTriangle().IndexSpan()),
		Rml::Vector2f(1.f, 2.f), Handle);
	const TUniquePtr<FVaCuusCommandBuffer> Buffer = Recorder.EndFrameAndPublish();
	if (!TestNotNull(TEXT("The frame carrying it publishes"), Buffer.Get()))
	{
		return false;
	}

	// The desc rides the buffer as an id, with no payload — see FVaCuusExternalTextureDesc.
	const FVaCuusExternalTextureDesc* Desc = Buffer->NewExternalTextures.Find(FVaCuusTextureHandle(Handle));
	if (!TestNotNull(TEXT("The buffer carries an external-texture desc for it"), Desc))
	{
		return false;
	}
	TestEqual(TEXT("The desc names the key's hashed id, computed with no registry round-trip"),
		Desc->StableId, FVaCuusTextureRegistry::IdForKey(Key));

	// The id is a pure function of the key, which is the property that lets the UI thread
	// name something the game thread has never seen.
	TestEqual(TEXT("IdForKey is stable across calls"), FVaCuusTextureRegistry::IdForKey(Key),
		FVaCuusTextureRegistry::IdForKey(Key));
	TestNotEqual(TEXT("...and separates different keys"), FVaCuusTextureRegistry::IdForKey(Key),
		FVaCuusTextureRegistry::IdForKey(Key + TEXT("x")));
	TestNotEqual(TEXT("...and never collides with the no-texture sentinel"), FVaCuusTextureRegistry::IdForKey(FString()),
		uint64(0));

	return true;
}

/**
 * THE HEADLINE COST PROPERTY: a STATIC engine texture costs nothing. The idle gate keeps
 * withholding frames exactly as it would with no texture in the document at all.
 *
 * Without this test the default would silently drift to "everything is live", which is the
 * whole plugin's selling point inverted — 60 published frames a second for a HUD that has not
 * changed. RESTORE-THE-BUG: register with bLive = true and the loop below publishes every
 * frame instead of none.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusExternalTextureStaticTest, "VaCuus.Render.ExternalTexture.StaticCostsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusExternalTextureStaticTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusExternalTextureTest;

	UTexture2D* Texture = MakeAssetTexture();
	if (!TestNotNull(TEXT("Transient texture created"), Texture))
	{
		return false;
	}

	const FString Key = TEXT("vacuus-test-static");
	ON_SCOPE_EXIT
	{
		FVaCuusTextureRegistry::UnregisterTexture(Key);
	};

	if (!TestTrue(TEXT("A static registration is accepted"), FVaCuusTextureRegistry::RegisterTexture(Key, Texture)))
	{
		return false;
	}
	InstallCurrent();

	FVaCuusRecordingRenderInterface Recorder;
	Recorder.BeginFrame(GViewSize);
	Rml::Vector2i Dimensions(0, 0);
	const Rml::TextureHandle Handle = Recorder.LoadTexture(Dimensions, "unreal://vacuus-test-static");
	TestEqual(TEXT("A registered key lays out at the texture's size"), Dimensions.x, 4);
	const Rml::CompiledGeometryHandle Geometry = Recorder.CompileGeometry(FTriangle().VertexSpan(), FTriangle().IndexSpan());
	Recorder.RenderGeometry(Geometry, Rml::Vector2f(1.f, 2.f), Handle);
	if (!TestNotNull(TEXT("The first frame publishes"), Recorder.EndFrameAndPublish().Get()))
	{
		return false;
	}

	// Five engine frames' worth of identical content. Advancing GFrameCounter is what makes
	// this a real test of the LIVENESS term rather than of the clamp: a live texture would
	// publish on every one of these.
	for (int32 Frame = 0; Frame < 5; ++Frame)
	{
		++GFrameCounter;
		TestNull(TEXT("A static engine texture never reopens the idle gate"), RecordDraw(Recorder, Geometry, Handle).Get());
	}
	TestEqual(TEXT("One publish in six frames"), int32(Recorder.GetNumFramesPublished()), 1);
	TestEqual(TEXT("...and five withheld"), int32(Recorder.GetNumFramesSkipped()), 5);

	return true;
}

/**
 * THE FREEZE REMEDY (spec 2026-08-22 §3), and its clamp, and its kill-switch.
 *
 * A live render target's pixels change without changing one byte of the command stream, so
 * neither term of the idle gate can see it. VaCuusMaterialTest asserts the same three things
 * for the material tier; this is the texture tier's twin, deliberately in the same order so
 * the two read alike.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusExternalTextureLiveTest, "VaCuus.Render.ExternalTexture.LiveForcesRepublish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusExternalTextureLiveTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusExternalTextureTest;

	UTextureRenderTarget2D* RenderTarget = MakeRenderTarget();
	if (!TestNotNull(TEXT("Render target created"), RenderTarget))
	{
		return false;
	}

	const FString Key = TEXT("vacuus-test-live");
	IConsoleVariable* Remedy = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.ExternalTextureForcedRepublish"));
	if (!TestNotNull(TEXT("The kill-switch exists"), Remedy))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Remedy->Set(1, ECVF_SetByCode);
		FVaCuusTextureRegistry::UnregisterTexture(Key);
	};

	if (!TestTrue(TEXT("A live registration is accepted"),
			FVaCuusTextureRegistry::RegisterTexture(Key, RenderTarget, /*bLive=*/true)))
	{
		return false;
	}
	InstallCurrent();

	FVaCuusRecordingRenderInterface Recorder;
	Recorder.BeginFrame(GViewSize);
	Rml::Vector2i Dimensions(0, 0);
	const Rml::TextureHandle Handle = Recorder.LoadTexture(Dimensions, "unreal://vacuus-test-live");
	TestEqual(TEXT("The render target's width reaches layout"), Dimensions.x, 64);
	TestEqual(TEXT("...and its height"), Dimensions.y, 32);
	const Rml::CompiledGeometryHandle Geometry = Recorder.CompileGeometry(FTriangle().VertexSpan(), FTriangle().IndexSpan());
	Recorder.RenderGeometry(Geometry, Rml::Vector2f(1.f, 2.f), Handle);
	if (!TestNotNull(TEXT("The first frame publishes"), Recorder.EndFrameAndPublish().Get()))
	{
		return false;
	}

	// THE CLAMP, from its suppressing side: this frame records inside the SAME engine frame
	// as the publish above, and a second replay inside one engine frame buys pixels the
	// composite will never sample.
	TestNull(TEXT("Unchanged frame in the same engine frame: withheld (the clamp)"),
		RecordDraw(Recorder, Geometry, Handle).Get());

	// THE REMEDY: next engine frame, identical content, publishes anyway.
	++GFrameCounter;
	TestNotNull(TEXT("Next engine frame: the unchanged frame publishes (the freeze remedy)"),
		RecordDraw(Recorder, Geometry, Handle).Get());

	++GFrameCounter;
	TestNotNull(TEXT("...and the one after it"), RecordDraw(Recorder, Geometry, Handle).Get());

	// THE KILL-SWITCH, which is the freeze itself made observable: no publish means the
	// replay never re-binds the reference, so the view holds whatever pixels it last had.
	Remedy->Set(0, ECVF_SetByCode);
	++GFrameCounter;
	TestNull(TEXT("Remedy off: withheld — the freeze the remedy exists for"), RecordDraw(Recorder, Geometry, Handle).Get());
	Remedy->Set(1, ECVF_SetByCode);

	// THE FLAG'S OTHER EDGE: releasing the texture empties this recorder's table, and the
	// view can idle again.
	Recorder.ReleaseTexture(Handle);
	const TUniquePtr<FVaCuusCommandBuffer> Releasing = RecordDraw(Recorder, Geometry, Rml::TextureHandle(0));
	if (TestNotNull(TEXT("The releasing frame publishes (release traffic + content change)"), Releasing.Get()))
	{
		TestTrue(TEXT("...and carries the release"), Releasing->ReleasedTextures.Contains(FVaCuusTextureHandle(Handle)));
	}
	++GFrameCounter;
	TestNull(TEXT("No live engine texture drawn: the idle gate is back in charge"),
		RecordDraw(Recorder, Geometry, Rml::TextureHandle(0)).Get());

	return true;
}

/**
 * MarkTextureDirty costs EXACTLY ONE published frame, per view, with nothing to clear.
 *
 * This is the mode that makes an occasionally-updated view — a minimap captured twice a
 * second — cost two frames a second rather than sixty. The counter shape is what makes "per
 * view" true: see FVaCuusExternalTextureUse::LastPublishedDirtyCounter.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusExternalTextureMarkDirtyTest, "VaCuus.Render.ExternalTexture.MarkDirtyPublishesOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusExternalTextureMarkDirtyTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusExternalTextureTest;

	UTextureRenderTarget2D* RenderTarget = MakeRenderTarget();
	if (!TestNotNull(TEXT("Render target created"), RenderTarget))
	{
		return false;
	}

	const FString Key = TEXT("vacuus-test-ondemand");
	ON_SCOPE_EXIT
	{
		FVaCuusTextureRegistry::UnregisterTexture(Key);
	};

	// STATIC, deliberately: on-demand refresh must not need bLive.
	if (!TestTrue(TEXT("Registered static"), FVaCuusTextureRegistry::RegisterTexture(Key, RenderTarget)))
	{
		return false;
	}
	InstallCurrent();

	const uint64 Id = FVaCuusTextureRegistry::IdForKey(Key);

	FVaCuusRecordingRenderInterface Recorder;
	Recorder.BeginFrame(GViewSize);
	Rml::Vector2i Dimensions(0, 0);
	const Rml::TextureHandle Handle = Recorder.LoadTexture(Dimensions, "unreal://vacuus-test-ondemand");
	const Rml::CompiledGeometryHandle Geometry = Recorder.CompileGeometry(FTriangle().VertexSpan(), FTriangle().IndexSpan());
	Recorder.RenderGeometry(Geometry, Rml::Vector2f(1.f, 2.f), Handle);
	if (!TestNotNull(TEXT("The first frame publishes"), Recorder.EndFrameAndPublish().Get()))
	{
		return false;
	}

	++GFrameCounter;
	TestNull(TEXT("Idle before anything is marked"), RecordDraw(Recorder, Geometry, Handle).Get());

	// One mark, one frame. Not two.
	FVaCuusTextureRegistry::MarkDirty_UIThread(Id);
	++GFrameCounter;
	TestNotNull(TEXT("The marked frame publishes"), RecordDraw(Recorder, Geometry, Handle).Get());

	++GFrameCounter;
	TestNull(TEXT("...and exactly one: the next frame is withheld again"), RecordDraw(Recorder, Geometry, Handle).Get());
	++GFrameCounter;
	TestNull(TEXT("...and stays withheld"), RecordDraw(Recorder, Geometry, Handle).Get());

	// A SECOND RECORDER SEES ITS OWN COPY OF THE SAME MARK. This is what a flag could not
	// do: whoever cleared it first would steal the other view's refresh.
	FVaCuusRecordingRenderInterface Second;
	Second.BeginFrame(GViewSize);
	Rml::Vector2i SecondDimensions(0, 0);
	const Rml::TextureHandle SecondHandle = Second.LoadTexture(SecondDimensions, "unreal://vacuus-test-ondemand");
	const Rml::CompiledGeometryHandle SecondGeometry = Second.CompileGeometry(FTriangle().VertexSpan(), FTriangle().IndexSpan());
	Second.RenderGeometry(SecondGeometry, Rml::Vector2f(1.f, 2.f), SecondHandle);
	Second.EndFrameAndPublish();

	FVaCuusTextureRegistry::MarkDirty_UIThread(Id);
	++GFrameCounter;
	TestNotNull(TEXT("View A publishes for the mark"), RecordDraw(Recorder, Geometry, Handle).Get());
	TestNotNull(TEXT("View B publishes for the same mark, independently"),
		RecordDraw(Second, SecondGeometry, SecondHandle).Get());

	return true;
}

/**
 * DRAWN, NOT MERELY LOADED (the divergence from LiveMaterialShaders, argued at
 * ExternalTexturesDrawnThisFrame).
 *
 * A live render target is the expensive case. An <img> inside a hidden panel is loaded but
 * not drawn, and it must not keep the whole view republishing at engine rate for pixels
 * nothing composites. RESTORE-THE-BUG: key the gate's term off ExternalTextures instead of
 * ExternalTexturesDrawnThisFrame and the loop below publishes every frame.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusExternalTextureOffscreenTest, "VaCuus.Render.ExternalTexture.UndrawnCostsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusExternalTextureOffscreenTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusExternalTextureTest;

	UTextureRenderTarget2D* RenderTarget = MakeRenderTarget();
	if (!TestNotNull(TEXT("Render target created"), RenderTarget))
	{
		return false;
	}

	const FString Key = TEXT("vacuus-test-undrawn");
	ON_SCOPE_EXIT
	{
		FVaCuusTextureRegistry::UnregisterTexture(Key);
	};
	if (!TestTrue(TEXT("Registered LIVE"), FVaCuusTextureRegistry::RegisterTexture(Key, RenderTarget, /*bLive=*/true)))
	{
		return false;
	}
	InstallCurrent();

	FVaCuusRecordingRenderInterface Recorder;
	Recorder.BeginFrame(GViewSize);
	Rml::Vector2i Dimensions(0, 0);
	const Rml::TextureHandle Handle = Recorder.LoadTexture(Dimensions, "unreal://vacuus-test-undrawn");
	const Rml::CompiledGeometryHandle Geometry = Recorder.CompileGeometry(FTriangle().VertexSpan(), FTriangle().IndexSpan());

	// LOADED but never drawn: the element exists in the document and is not rendered.
	Recorder.RenderGeometry(Geometry, Rml::Vector2f(1.f, 2.f), Rml::TextureHandle(0));
	if (!TestNotNull(TEXT("The first frame publishes"), Recorder.EndFrameAndPublish().Get()))
	{
		return false;
	}

	for (int32 Frame = 0; Frame < 4; ++Frame)
	{
		++GFrameCounter;
		TestNull(TEXT("A live texture that is not drawn costs nothing"),
			RecordDraw(Recorder, Geometry, Rml::TextureHandle(0)).Get());
	}

	// And the moment it IS drawn, it costs what a live texture costs.
	++GFrameCounter;
	TestNotNull(TEXT("Drawing it reopens the gate"), RecordDraw(Recorder, Geometry, Handle).Get());
	++GFrameCounter;
	TestNotNull(TEXT("...and keeps it open while it stays on screen"), RecordDraw(Recorder, Geometry, Handle).Get());

	return true;
}

/**
 * THE GAME-THREAD HALF: what registration accepts, what it refuses by name, what it derives,
 * and that re-registration keeps the id.
 *
 * THE ID MUST SURVIVE RE-REGISTRATION or a document already drawing the key would follow the
 * swap nowhere: the recorder minted its handle once, and only the id connects that handle to
 * whatever the mirror now holds.
 *
 * NOT COVERED HERE, and said out loud rather than left as a gap: the hash-collision refusal.
 * A CityHash64 collision cannot be constructed on demand, so that branch has only its counter
 * (GetNumCollisionsRefused_GameThread) as an observable. It is asserted to be zero below,
 * which at least holds "nothing collides by accident" in place.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusExternalTextureRegistrationTest, "VaCuus.Render.ExternalTexture.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusExternalTextureRegistrationTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusExternalTextureTest;

	const FString AssetKey = TEXT("vacuus-test-asset");
	const FString RtKey = TEXT("vacuus-test-rt");
	ON_SCOPE_EXIT
	{
		FVaCuusTextureRegistry::UnregisterTexture(AssetKey);
		FVaCuusTextureRegistry::UnregisterTexture(RtKey);
	};

	// Deltas, not absolutes: the registry is process-wide and never resets, so another test
	// in the same run may already have entries. Same discipline as the style registry's test.
	const int32 EntriesBefore = FVaCuusTextureRegistry::GetNumEntries_GameThread();
	const uint64 VersionBefore = FVaCuusTextureRegistry::GetVersion_GameThread();
	const int32 CollisionsBefore = FVaCuusTextureRegistry::GetNumCollisionsRefused_GameThread();

	// --- the named refusals ---
	// AddExpectedError, not AddExpectedMessagePlain: every refusal below deliberately lands
	// at Error severity (a silent refusal is the failure mode this whole registry avoids),
	// and the automation framework fails a test on any unclaimed Error.
	AddExpectedError(TEXT("empty key refused"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("null texture refused"), EAutomationExpectedErrorFlags::Contains, 1);
	if (FApp::CanEverRender())
	{
		AddExpectedError(TEXT("has no render resource"), EAutomationExpectedErrorFlags::Contains, 1);
	}

	TestFalse(TEXT("Empty key refused"), FVaCuusTextureRegistry::RegisterTexture(FString(), MakeAssetTexture()));
	TestFalse(TEXT("Null texture refused"), FVaCuusTextureRegistry::RegisterTexture(AssetKey, nullptr));

	// A texture whose UpdateResource() was never called has no render resource. Registering it
	// would be a black rectangle with nothing in any log, which is the failure mode the
	// refusal exists to replace. AddToRoot so GC cannot take it mid-test.
	//
	// VENUE-CONDITIONAL, because the REFUSAL is: UpdateResource() creates a resource only
	// under FApp::CanEverRender() (Texture.cpp:336), so in a process that cannot render NO
	// texture has one, the registry accepts by design, and this assertion would be asserting
	// the opposite of the intended behaviour. See RegisterTexture's own note.
	if (FApp::CanEverRender())
	{
		UTexture2D* Uninitialised = UTexture2D::CreateTransient(4, 4, PF_B8G8R8A8);
		if (TestNotNull(TEXT("Uninitialised texture created"), Uninitialised))
		{
			Uninitialised->AddToRoot();
			TestFalse(TEXT("A texture with no render resource is refused by name"),
				FVaCuusTextureRegistry::RegisterTexture(TEXT("vacuus-test-uninitialised"), Uninitialised));
			Uninitialised->RemoveFromRoot();
		}
	}
	else
	{
		AddInfo(TEXT("The missing-resource refusal is not asserted here: this process cannot render, so no texture "
					 "has a resource and the registry accepts by design."));
	}

	// --- acceptance, and the derived modes ---
	UTexture2D* Asset = MakeAssetTexture();
	UTextureRenderTarget2D* RenderTarget = MakeRenderTarget();
	if (!TestNotNull(TEXT("Asset texture created"), Asset) || !TestNotNull(TEXT("Render target created"), RenderTarget))
	{
		return false;
	}

	TestTrue(TEXT("An sRGB asset texture registers"), FVaCuusTextureRegistry::RegisterTexture(AssetKey, Asset));
	TestTrue(TEXT("A render target registers"), FVaCuusTextureRegistry::RegisterTexture(RtKey, RenderTarget));

	TestEqual(TEXT("Two entries added"), FVaCuusTextureRegistry::GetNumEntries_GameThread(), EntriesBefore + 2);
	TestTrue(TEXT("The version moved forward"), FVaCuusTextureRegistry::GetVersion_GameThread() > VersionBefore);
	TestEqual(TEXT("Nothing collided"), FVaCuusTextureRegistry::GetNumCollisionsRefused_GameThread(), CollisionsBefore);

	TSharedPtr<const FVaCuusTextureSnapshot> Snapshot = FVaCuusTextureRegistry::GetSnapshot_GameThread();
	if (!TestTrue(TEXT("A snapshot was published"), Snapshot.IsValid()))
	{
		return false;
	}

	const FVaCuusTextureBinding* AssetBinding = Snapshot->KeyToBinding.Find(AssetKey);
	const FVaCuusTextureBinding* RtBinding = Snapshot->KeyToBinding.Find(RtKey);
	if (!TestNotNull(TEXT("The asset is in the snapshot"), AssetBinding) ||
		!TestNotNull(TEXT("The render target is in the snapshot"), RtBinding))
	{
		return false;
	}

	TestEqual(TEXT("The snapshot's id is the key's hash"), AssetBinding->StableId, FVaCuusTextureRegistry::IdForKey(AssetKey));
	TestEqual(TEXT("The render target's size reached the snapshot"), RtBinding->Size, FIntPoint(64, 32));
	TestFalse(TEXT("Static by default"), RtBinding->bLive);

	// --- re-registration keeps the id, which is what lets a live document follow a swap ---
	const uint64 IdBefore = RtBinding->StableId;
	UTextureRenderTarget2D* Replacement = MakeRenderTarget();
	TestTrue(TEXT("Re-registering the same key is accepted"),
		FVaCuusTextureRegistry::RegisterTexture(RtKey, Replacement, /*bLive=*/true));
	TestEqual(TEXT("...and adds no entry"), FVaCuusTextureRegistry::GetNumEntries_GameThread(), EntriesBefore + 2);

	Snapshot = FVaCuusTextureRegistry::GetSnapshot_GameThread();
	RtBinding = Snapshot->KeyToBinding.Find(RtKey);
	if (TestNotNull(TEXT("Still in the snapshot"), RtBinding))
	{
		TestEqual(TEXT("...with the SAME id, so a drawn handle follows the swap"), RtBinding->StableId, IdBefore);
		TestTrue(TEXT("...and the new liveness"), RtBinding->bLive);
	}

	// --- unregistration parks the root behind a fence ---
	FVaCuusTextureRegistry::UnregisterTexture(RtKey);
	TestEqual(TEXT("One entry removed"), FVaCuusTextureRegistry::GetNumEntries_GameThread(), EntriesBefore + 1);
	TestTrue(TEXT("The root is parked behind a render fence, not dropped"),
		FVaCuusTextureRegistry::GetNumPendingReleases_GameThread() > 0);

	FlushRenderingCommands();
	FVaCuusTextureRegistry::TickDeferredReleases_GameThread();
	TestEqual(TEXT("...and released once the fence completes"),
		FVaCuusTextureRegistry::GetNumPendingReleases_GameThread(), 0);

	return true;
}

/**
 * AN UNRESOLVED DRAW BINDS NOTHING AND COUNTS ITSELF — it does not crash, and it does not
 * ensure.
 *
 * Both ways to get here are ordinary: a key nobody registered (LoadTexture mints handles for
 * those on purpose) and a key unregistered while a buffer that draws it is still in flight.
 * The counter is the only observable either has; without it this invariant could not be
 * asserted at all (CLAUDE.md).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusExternalTextureUnresolvedTest, "VaCuus.Render.ExternalTexture.UnresolvedDrawCounts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusExternalTextureUnresolvedTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusExternalTextureTest;

	// VENUE, the contract VaCuus.Render.Composite.LinearOutputGPU already states: this one
	// needs the replay DRAW PASS, and ReplayCommands returns before its first draw when the
	// global shaders are unavailable (VaCuusReplayRenderer.cpp:932) -- which is every
	// -nullrhi run. A silent pass there would read as coverage it is not.
	if (GUsingNullRHI)
	{
		AddInfo(TEXT("Skipped under -nullrhi: the replay pass has no shaders, so no draw reaches the resolve branch."));
		return true;
	}

	// A buffer that draws an engine texture no registration will ever resolve.
	FVaCuusCommandBuffer Buffer;
	Buffer.Generation = 1;
	Buffer.ViewSize = FIntPoint(64, 64);

	const FVaCuusTextureHandle Handle = 1;
	Buffer.NewExternalTextures.Add(Handle).StableId = FVaCuusTextureRegistry::IdForKey(TEXT("vacuus-test-unresolvable"));

	FVaCuusGeometryData& Geometry = Buffer.NewGeometry.Add(FVaCuusGeometryHandle(1));
	Geometry.Vertices.SetNum(3);
	Geometry.Indices = {0, 1, 2};

	FVaCuusCommand& Command = Buffer.Commands.AddDefaulted_GetRef();
	Command.Type = EVaCuusCommandType::DrawGeometry;
	Command.Geometry = FVaCuusGeometryHandle(1);
	Command.Texture = Handle;

	FVaCuusReplayRenderer Replayer;
	ENQUEUE_RENDER_COMMAND(VaCuusExternalTextureUnresolved)
	([&Replayer, &Buffer](FRHICommandListImmediate& RHICmdList) { Replayer.Replay(RHICmdList, Buffer); });
	FlushRenderingCommands();

	TestEqual(TEXT("The unresolved draw is counted, not ensured"), Replayer.GetNumUnresolvedExternalDraws(), uint64(1));

	ENQUEUE_RENDER_COMMAND(VaCuusExternalTextureUnresolvedTeardown)
	([&Replayer](FRHICommandListImmediate&) { Replayer.ReleaseResources(); });
	FlushRenderingCommands();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
