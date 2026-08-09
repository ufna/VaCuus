// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusFontRegistry.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * THE RUNTIME FONT DOOR AND ITS REPLAY (spec 2026-08-09 §3).
 *
 * The door itself is the easy half. The half worth a test is the REPLAY: RmlUi's font provider
 * lives inside Rml::Initialise/Shutdown, so a UI thread that stops and starts again comes back
 * holding only the shipped Latin face. A game that loaded a Cyrillic face at runtime would lose
 * it, and lose it SILENTLY -- missing glyphs are substituted with U+FFFD and nothing is logged
 * (FontFaceHandleDefault.cpp:386-393). So the registry replays, and this test is what says so.
 *
 * RESTORE-THE-BUG: delete the FVaCuusFontRegistry::PublishToUIThread line from
 * FVaCuusModule::GetOrStartUIThread and the post-restart assertion below fails at 0 while the
 * first-boot one still passes. Verified both ways.
 *
 * THE FACE IS THE SHIPPED ONE, registered as a FALLBACK face. It is the only .ttf in the tree,
 * and using it is not a compromise: what is under test is the registry's bookkeeping and replay,
 * and a fallback registration of an already-loaded file is a real, distinct request to RmlUi.
 */
namespace VaCuusFontRegistryTest
{
static const TCHAR* GFacePath = TEXT("fonts/LatoLatin-Regular.ttf");

/** Samples the UI-thread-only counter once per frame; the same rule the model tests follow. */
class FFontProbeHost final : public FVaCuusTestDocumentHost
{
public:
	explicit FFontProbeHost(const TCHAR* InContextPrefix)
		: FVaCuusTestDocumentHost(InContextPrefix, "vacuus://font_registry.rml", Rml::FocusFlag::Document)
	{
	}

	virtual void SetVisible(bool /*bVisible*/) override {}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Context->Update();
		NumFacesLoaded = FVaCuusFontRegistry::GetNumFacesLoaded_UIThread();
		NumReplacementGlyphs = FVaCuusFontRegistry::GetNumReplacementGlyphs_UIThread();
		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	int32 NumFacesLoaded = -1;
	int32 NumReplacementGlyphs = -1;
};

static bool RunFrames(FVaCuusUIThread& UIThread, int32 NumFrames)
{
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const uint64 Before = UIThread.GetFrameCount();
		UIThread.Trigger();
		if (!UIThread.WaitForFrameCount(Before + 1, 5.0))
		{
			return false;
		}
	}
	return true;
}

/**
 * A document is required, not decoration: the per-view record loop is gated on HasView(), which
 * wants both a non-degenerate size AND a document, so a view without one never records a frame
 * and the probe would never sample anything (the sizeless view in VaCuus.Model.Apply is the same
 * gate seen from the other side).
 */
static const TCHAR* GProbeDocument = TEXT(R"(<rml>
<head><style>body { display: block; }</style></head>
<body/>
</rml>)");

/** Boots a view with a document on the given thread, so the record loop will run. */
static FFontProbeHost* AddProbeView(FVaCuusUIThread& UIThread, const TCHAR* Prefix, TSharedRef<FVaCuusViewStatus>& OutStatus)
{
	TUniquePtr<FFontProbeHost> Owned = MakeUnique<FFontProbeHost>(Prefix);
	FFontProbeHost* Probe = Owned.Get();
	const uint32 ViewId = UIThread.AllocateViewId();
	UIThread.EnqueueAddView(ViewId, MoveTemp(Owned), FIntPoint(200, 100), OutStatus);
	UIThread.EnqueueLoadDocumentFromMemory(ViewId, GProbeDocument, /*LoadSerial=*/1);
	return Probe;
}
}	 // namespace VaCuusFontRegistryTest

/**
 * A registered face is loaded, and survives a UI-thread restart.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFontRegistryReplayTest, "VaCuus.Font.RegistryReplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFontRegistryReplayTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusFontRegistryTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to restart"));
		return true;
	}

	FVaCuusModule& Module = FVaCuusModule::Get();

	// REGISTERED BEFORE ANY THREAD EXISTS -- the register-before-boot half. This is also what
	// makes the first assertion meaningful: nothing but PublishToUIThread could have loaded it.
	const int32 RequestsBefore = FVaCuusFontRegistry::GetRequests_GameThread().Num();
	const bool bRegistered = FVaCuusFontRegistry::RegisterFace(GFacePath, /*bFallbackFace=*/true);
	TestTrue(TEXT("the face registered"), bRegistered);
	TestEqual(TEXT("...and is in the ordered request list"),
		FVaCuusFontRegistry::GetRequests_GameThread().Num(), RequestsBefore + 1);

	// DEDUPLICATION, because a game that re-registers from a language-change handler must not
	// accumulate: the same (path, flag) pair is refused, the list does not grow.
	TestFalse(TEXT("the same request is refused"), FVaCuusFontRegistry::RegisterFace(GFacePath, /*bFallbackFace=*/true));
	TestEqual(TEXT("...and the list did not grow"),
		FVaCuusFontRegistry::GetRequests_GameThread().Num(), RequestsBefore + 1);

	// ---- First boot. ----

	FVaCuusUIThread* First = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), First))
	{
		return false;
	}

	TSharedRef<FVaCuusViewStatus> FirstStatus = MakeShared<FVaCuusViewStatus>();
	FFontProbeHost* FirstProbe = AddProbeView(*First, TEXT("vacuus_font_first"), FirstStatus);
	if (!TestTrue(TEXT("frames ran on the first thread"), RunFrames(*First, 2)))
	{
		Module.StopUIThread();
		return false;
	}

	TestEqual(TEXT("the registered face was loaded on the first boot"), FirstProbe->NumFacesLoaded, RequestsBefore + 1);

	// ---- Restart, which is what the replay exists for. ----

	Module.StopUIThread();

	FVaCuusUIThread* Second = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("the UI thread restarted"), Second))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	TSharedRef<FVaCuusViewStatus> SecondStatus = MakeShared<FVaCuusViewStatus>();
	FFontProbeHost* SecondProbe = AddProbeView(*Second, TEXT("vacuus_font_second"), SecondStatus);
	if (!TestTrue(TEXT("frames ran on the restarted thread"), RunFrames(*Second, 2)))
	{
		return false;
	}

	// THE CLAIM. A fresh RmlUi has only the default face; this count can only be non-zero because
	// the registry replayed its requests into the new thread's queue ahead of anything else.
	TestEqual(TEXT("the face was RELOADED after the restart, not silently lost"),
		SecondProbe->NumFacesLoaded, RequestsBefore + 1);

	return true;
}

/**
 * The silent substitution, made loud (VaCuus patch #5, bead VaCuus-sg2.6).
 *
 * RmlUi answers a character no loaded face covers by swapping in U+FFFD and returning, with no
 * log at any level (FontFaceHandleDefault.cpp, "If we still have not found a glyph"). The shipped
 * face is Latin-only -- measured 20-7e, a0-17f plus symbols -- so a game that switches to Russian
 * without bringing a face renders a screen of replacement characters and its log says nothing was
 * wrong. The patch counts the substitutions and warns once; this test is the observable that
 * keeps the patch alive across a re-vendor.
 *
 * DELTAS, NEVER ABSOLUTES: the counter belongs to the RmlUi library instance, which this suite
 * shares, so what is asserted is what THIS document did to it.
 *
 * WHAT IS NOT ASSERTED, and why: that the count stays flat once a covering face is registered.
 * The repository ships exactly one .ttf and it has no Cyrillic, so there is no face to register
 * -- proving that half needs a font asset, which is a licensing decision and not this bead's.
 * The half that IS proven is the one that matters for a re-vendor: the instrument distinguishes
 * covered text from uncovered text.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusFontMissingGlyphTest, "VaCuus.Font.MissingGlyph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusFontMissingGlyphTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusFontRegistryTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	// A FONT-FAMILY AND A SIZE ARE REQUIRED, not decoration: without them RmlUi logs "No font face
	// defined" and never lays the text out, so no glyph is ever asked for and the counter cannot
	// move whether the patch is present or not.
	static const TCHAR* GLatinDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 16px; } div { display: block; }</style></head>
<body><div id="t">Health and ammo</div></body>
</rml>)");

	// Cyrillic: every one of these code points is outside the shipped face's coverage.
	static const TCHAR* GCyrillicDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 16px; } div { display: block; }</style></head>
<body><div id="t">Здоровье</div></body>
</rml>)");

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	TUniquePtr<FFontProbeHost> Owned = MakeUnique<FFontProbeHost>(TEXT("vacuus_font_glyphs"));
	FFontProbeHost* Probe = Owned.Get();
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	const uint32 ViewId = UIThread->AllocateViewId();

	UIThread->EnqueueAddView(ViewId, MoveTemp(Owned), FIntPoint(400, 200), Status);
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GLatinDocument, /*LoadSerial=*/1);
	if (!TestTrue(TEXT("frames ran over the Latin document"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	// THE CONTROL. Text the face covers must not move the counter -- without this the assertion
	// below would pass just as well on a counter that incremented on every glyph.
	const int32 AfterLatin = Probe->NumReplacementGlyphs;
	if (!TestTrue(TEXT("the counter is readable (the patch is present)"), AfterLatin >= 0))
	{
		return false;
	}

	if (!TestTrue(TEXT("more frames over the same Latin text"), RunFrames(*UIThread, 3)))
	{
		return false;
	}
	TestEqual(TEXT("covered text substitutes NOTHING, however many frames it is laid out for"),
		Probe->NumReplacementGlyphs, AfterLatin);

	// THE CLAIM.
	UIThread->EnqueueLoadDocumentFromMemory(ViewId, GCyrillicDocument, /*LoadSerial=*/2);
	if (!TestTrue(TEXT("frames ran over the Cyrillic document"), RunFrames(*UIThread, 3)))
	{
		return false;
	}

	TestTrue(TEXT("uncovered text is COUNTED instead of silently becoming U+FFFD"),
		Probe->NumReplacementGlyphs > AfterLatin);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
