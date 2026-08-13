// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VaCuusJsDomTestRig.h"

#include "VaCuusStats.h"

/*
 * `vacuus.textureStats()` — the JS reader of the resident-texture census (VaCuus-dqs.1).
 *
 * WHAT THIS TESTS AND WHAT IT DELIBERATELY DOES NOT. The census itself — that the numbers
 * describe the replayer's actual texture maps — is VaCuus.Render.Texture.Census's job, on the
 * render thread, with real RHI textures. This test owns the OTHER half: that a number published
 * into the shared store crosses the module boundary and arrives in JS with the right shape and
 * the right value.
 *
 * THAT BOUNDARY IS THE WHOLE REASON THIS TEST EXISTS. VaCuusJs does not depend on VaCuusRender
 * (VaCuusJs.Build.cs: Core, InputCore, VaCuus, VaCuusRml), so the facade cannot read the
 * replayer at all; it reads FVaCuusPerfLog, which both modules do depend on. Writing the store
 * directly here is therefore not a mock standing in for the producer — it IS the seam the
 * producer writes through, exercised from the side that consumes it.
 *
 * DELTAS, NEVER ABSOLUTES. The store is process-wide and always on, so whatever else this
 * editor happens to be holding is legitimately part of the reading. What is deterministic is
 * how much a known publication moves it.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsTextureStatsTest, "VaCuus.Js.Host.TextureStats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusJsTextureStatsTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsDomTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; }</style></head>
<body/>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	const uint32 ViewId = Rig.AddViewWithDocument(Probe, TEXT("vacuus_js_texturestats"), GDocument);

	// The bind is what gives the view a JS context to evaluate in -- without it EvalString
	// answers "<no context>" (VaCuusJsDomTestRig.h:155-161). Production wiring is
	// OnDocumentReady; this is the test-only entry every rig-based test goes through.
	bool bBound = false;
	Rig.RunOnUI([&bBound, Probe, ViewId]()
		{
			Rml::ElementDocument* Document = Probe->GetDocument();
			bBound = Document != nullptr;
			FWrappedDomHost::Inner->BindDocumentForTest(ViewId, Document);
		});
	if (!TestTrue(TEXT("the document loaded and bound"), bBound))
	{
		return false;
	}

	// THE SHAPE FIRST, because it is what a JS author writes against and it is checkable
	// without touching the numbers: a function, returning an object carrying exactly `count`
	// and `bytes`, both numbers. `Object.keys().sort()` catches a key that was added without
	// being typed in vacuus.d.ts as surely as it catches a missing one.
	TestEqual(TEXT("textureStats() is a function returning {count, bytes}, both numbers"),
		Rig.Eval(ViewId,
			// AN IIFE, NOT BARE `const`: every Eval on a view shares one global scope, so a
			// second `const s` in a later call is a SyntaxError that reads as a facade failure.
			"(() => { const s = vacuus.textureStats();"
			" return [typeof vacuus.textureStats, typeof s, Object.keys(s).sort().join(','),"
			" typeof s.count, typeof s.bytes].join('|'); })()"),
		FString(TEXT("function|object|bytes,count|number|number")));

	// 213x276 again: the icon the whole epic is about, so the number crossing the bridge here
	// is the same 235,152 the render-side test asserts and the epic's description quotes.
	constexpr int32 IconCount = 7;
	constexpr int64 IconBytes = 7ll * 213ll * 276ll * 4ll;	  // 1,646,064

	const FString Before = Rig.Eval(ViewId,
		"(() => { const s = vacuus.textureStats(); return [s.count, s.bytes].join(','); })()");

	FVaCuusPerfLog::AddResidentTextures(IconCount, IconBytes);
	const FString AfterAdd = Rig.Eval(ViewId,
		"(() => { const s = vacuus.textureStats(); return [s.count, s.bytes].join(','); })()");

	// And back down, which is the half a growing-only counter would pass anyway.
	FVaCuusPerfLog::AddResidentTextures(-IconCount, -IconBytes);
	const FString AfterRetract = Rig.Eval(ViewId,
		"(() => { const s = vacuus.textureStats(); return [s.count, s.bytes].join(','); })()");

	const auto Parse = [](const FString& Pair, int64& OutCount, int64& OutBytes)
	{
		FString Left, Right;
		if (!Pair.Split(TEXT(","), &Left, &Right))
		{
			return false;
		}
		OutCount = FCString::Atoi64(*Left);
		OutBytes = FCString::Atoi64(*Right);
		return true;
	};

	int64 C0 = 0, B0 = 0, C1 = 0, B1 = 0, C2 = 0, B2 = 0;
	if (!TestTrue(TEXT("all three readings parse"),
			Parse(Before, C0, B0) && Parse(AfterAdd, C1, B1) && Parse(AfterRetract, C2, B2)))
	{
		AddError(FString::Printf(TEXT("readings were '%s', '%s', '%s'"), *Before, *AfterAdd, *AfterRetract));
		return false;
	}

	TestEqual(TEXT("a publication moves the count JS sees"), C1 - C0, int64(IconCount));
	TestEqual(TEXT("...and the bytes, exactly"), B1 - B0, IconBytes);
	TestEqual(TEXT("retracting it puts the count back"), C2, C0);
	TestEqual(TEXT("...and the bytes"), B2, B0);

	// THE FLOAT64 IS NOT DECORATION. A byte count passes 2^31 at 2 GiB — a texture budget
	// reaches that — and JS_NewInt32 would have wrapped it silently. Pushing a value beyond the
	// 32-bit range and reading it back is the only way to see that from this side.
	constexpr int64 HugeBytes = 3ll * 1024ll * 1024ll * 1024ll;	   // 3 GiB
	FVaCuusPerfLog::AddResidentTextures(0, HugeBytes);
	const FString Huge = Rig.Eval(ViewId, "String(vacuus.textureStats().bytes)");
	FVaCuusPerfLog::AddResidentTextures(0, -HugeBytes);

	int64 HugeSeen = FCString::Atoi64(*Huge);
	TestEqual(TEXT("a byte count past 2^31 survives the crossing"), HugeSeen - B0, HugeBytes);

	// ---------------------------------------------------------------------------------
	// VaCuus-dqs.2: the release surface, on the same view.
	//
	// WHAT IS CHECKABLE HERE AND WHAT IS NOT. This document loads no images, so there is
	// nothing cached to drop -- which makes it exactly the right rig for the SEMANTICS: that
	// the call reaches the view's real document host and reports honestly, and that the two
	// refusals fire. Whether a release actually retires an RHI texture is a render-thread
	// question, measured in a -game run through vacuus.TextureStats.
	// ---------------------------------------------------------------------------------

	TestEqual(TEXT("releaseTexture on a source this view never cached answers false"),
		Rig.Eval(ViewId, "String(vacuus.releaseTexture('img/never_loaded.png'))"),
		FString(TEXT("false")));

	// AN EMPTY STRING IS THE ONE ARGUMENT THAT MUST NOT SILENTLY WORK: it is how the wire
	// spells "all of them", so a script passing an undefined variable through String() would
	// flush the whole view instead of one image.
	TestEqual(TEXT("releaseTexture('') throws rather than flushing the view"),
		Rig.Eval(ViewId,
			"(() => { try { vacuus.releaseTexture(''); return 'no-throw'; }"
			" catch (e) { return e.constructor.name; } })()"),
		FString(TEXT("TypeError")));

	TestEqual(TEXT("releaseTexture(non-string) throws"),
		Rig.Eval(ViewId,
			"(() => { try { vacuus.releaseTexture(42); return 'no-throw'; }"
			" catch (e) { return e.constructor.name; } })()"),
		FString(TEXT("TypeError")));

	TestEqual(TEXT("releaseTextures() is callable and answers undefined"),
		Rig.Eval(ViewId, "String(vacuus.releaseTextures())"), FString(TEXT("undefined")));

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
