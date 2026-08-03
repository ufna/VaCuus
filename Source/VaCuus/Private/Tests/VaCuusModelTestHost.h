// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusDocumentHost.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "HAL/PlatformProcess.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A real Rml::Context on the real UI thread, updated once per recorded frame, with the
 * document's data-bound values read back out of the DOM.
 *
 * WHY A PROBE AND NOT FVaCuusRmlDocumentHost: that host lives in VaCuusRender together with
 * the recorder it needs, and VaCuusRender depends on VaCuus rather than the other way round --
 * FVaCuusBoundModel and UVaCuusView's model map are Private to THIS module, so a test that
 * reaches them cannot live over there. The seam is the one IVaCuusDocumentHost exists for, and
 * everything below the host (the layout, both shadows, the channel, the bind, the apply, the
 * UI thread and the Rml::Context) is production code.
 *
 * The render-side half of the same pipeline -- that an unchanging model costs zero PUBLISHED
 * frames through the real idle gate -- is VaCuus.Model.View.Idle, in VaCuusRender, driving
 * FVaCuusRmlDocumentHost.
 *
 * THREAD HAND-OFF: plain members written on the UI thread, read on the test thread only after
 * WaitForFrameCount() saw the frame counter advance -- which the UI thread stores with release
 * ordering after RunFrame() returns. Same rule as VaCuus.Model.Binding's probe.
 */
namespace VaCuusModelTest
{
/** What the document is showing, as of the last Context::Update(). */
struct FObserved
{
	FString Title;
	FString Health;

	bool operator==(const FObserved& Other) const { return Title == Other.Title && Health == Other.Health; }
	bool operator!=(const FObserved& Other) const { return !(*this == Other); }
};

class FProbeHost final : public FVaCuusTestDocumentHost
{
public:
	explicit FProbeHost(const TCHAR* InContextPrefix)
		: FVaCuusTestDocumentHost(InContextPrefix, "vacuus://model_test.rml", Rml::FocusFlag::Document)
	{
	}

	virtual void SetVisible(bool bVisible) override {}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		Context->Update();

		const FObserved Now = Capture();
		if (NumRecordedFrames == 0 || Now != Latest)
		{
			++NumDomChanges;
		}
		Latest = Now;
		++NumRecordedFrames;

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Post-frame observations; see the file comment for why plain members are safe.

	/** What the document is showing right now. */
	FObserved Latest;

	/**
	 * Frames whose DOM differed from the previous frame's (the first counts as one).
	 *
	 * THE IDLE ASSERTION'S OBSERVABLE ON THIS SIDE OF THE RECORDER. What makes an idle UI
	 * frame free is that nothing writes the DOM, so the command list -- and therefore the frame
	 * hash the real gate compares -- does not move. A model that dirtied a variable every frame
	 * would make this climb while nothing on screen changed.
	 */
	int32 NumDomChanges = 0;
	int32 NumRecordedFrames = 0;
	int32 NumDocumentsLoaded = 0;

protected:
	virtual void OnDocumentAdopted() override { ++NumDocumentsLoaded; }

private:
	FObserved Capture() const
	{
		FObserved Out;
		if (RmlDocument == nullptr)
		{
			return Out;
		}

		// InnerRML, because that is where a `{{Field}}` substitution lands: DataViewText::Update
		// calls ElementText::SetText (DataViewDefault.cpp), and ElementText::GetRML appends the
		// CURRENT text (ElementText.cpp), so the div's inner RML is the resolved value rather
		// than the `{{Title}}` source. It needs no font and no laid-out text run.
		if (Rml::Element* TitleElement = RmlDocument->GetElementById("title"))
		{
			Out.Title = FString(UTF8_TO_TCHAR(TitleElement->GetInnerRML().c_str()));
		}
		if (Rml::Element* HealthElement = RmlDocument->GetElementById("health"))
		{
			Out.Health = FString(UTF8_TO_TCHAR(HealthElement->GetAttribute<Rml::String>("p", Rml::String()).c_str()));
		}
		return Out;
	}
};

/**
 * `{{Title}}` is the end-to-end evidence -- a data expression in element text, resolved by
 * RmlUi's own DataViewText and read back out of the DOM. Health rides along through
 * `data-attr-p` because an attribute is readable without a laid-out text run, so a failure
 * tells the two paths apart.
 */
static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body data-model="hud">
	<div id="title">{{Title}}</div>
	<div id="health" data-attr-p="Health"/>
</body>
</rml>)");

/** One UI frame at a time; the wake event coalesces, so N triggers are not N frames. */
inline bool RunFrames(FVaCuusUIThread& UIThread, int32 NumFrames)
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
}	 // namespace VaCuusModelTest

#endif	  // WITH_DEV_AUTOMATION_TESTS
