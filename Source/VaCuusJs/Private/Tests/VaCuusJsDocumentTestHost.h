// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

/*
 * The M4 Task 6 document tests' probe host: FDomProbeHost's shape (a real
 * Rml::Context on the real UI thread) PLUS the two script-host seam calls the
 * production FVaCuusRmlDocumentHost makes -- OnDocumentClosing at Close() time,
 * OnDocumentReady after old-close and Show(). A separate class rather than an
 * edit to FDomProbeHost, deliberately: the Task 4/5 suites built their
 * assertions on a host that does NOT recycle contexts on load (their globals
 * and wrapper caches survive BindDocumentForTest re-binds), and retrofitting
 * the seam there would change what those tests test.
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VaCuusJsDomTestRig.h"
#include "VaCuusTestDocumentHost.h"

namespace VaCuusJsDocumentTest
{
/**
 * Mirrors FVaCuusRmlDocumentHost::AdoptDocument/CloseDocument line for line
 * where the seam is concerned (load first, close second, Show(), then
 * OnDocumentReady; OnDocumentClosing before Close()). The production host
 * itself is unreachable from VaCuusJs -- module direction -- and its own
 * wiring is proven from VaCuusRender's test suite.
 */
class FJsDocProbeHost final : public FVaCuusTestDocumentHost
{
public:
	explicit FJsDocProbeHost(const TCHAR* InContextPrefix)
		: FVaCuusTestDocumentHost(InContextPrefix, "vacuus://js_doc_test.rml", Rml::FocusFlag::Document)
	{
	}

	/** The spec 2(f) seam call, exactly where the production host makes it: after old-close
	 *  (unload JS ran in the old context) and after Show(). The base guarantees both. */
	virtual void OnDocumentAdopted() override
	{
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentReady(ViewId, RmlDocument);
		}
	}

	virtual void OnDocumentClosing() override
	{
		// Unload JS while the document is still current -- the production CloseDocument's order,
		// which the base runs this hook to preserve.
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentClosing(ViewId);
		}

		// The production host's one-more-frame rule (IVaCuusDocumentHost::CloseDocument),
		// mirrored for its SIDE EFFECT: that frame's Context::Update() is what actually frees
		// the closed tree (Close only queues it), and the unload test's dead-wrapper assertion
		// is about the state AFTER that free -- the state a production view reaches one frame
		// after every close. Set here rather than after Close() because nothing between the two
		// reads it: this host runs entirely on the UI thread, one command at a time.
		bOwesUpdateFrame = true;
	}

	virtual void SetVisible(bool /*bVisible*/) override {}

	/** The base's, plus the post-close frame this host owes. */
	virtual bool HasView() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context != nullptr && (RmlDocument != nullptr || bOwesUpdateFrame) && ViewSize.X > 0 && ViewSize.Y > 0;
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		bOwesUpdateFrame = false;
		Context->Update();
		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

private:
	/** See OnDocumentClosing: one post-close Update, the production host's debt. */
	bool bOwesUpdateFrame = false;
};

/** AddView with a seam-calling probe; no document. */
inline uint32 AddSeamView(VaCuusJsDomTest::FDomTestRig& Rig, FJsDocProbeHost*& OutProbe, const TCHAR* Prefix)
{
	TUniquePtr<FJsDocProbeHost> Owned = MakeUnique<FJsDocProbeHost>(Prefix);
	OutProbe = Owned.Get();
	const uint32 ViewId = Rig.Thread->AllocateViewId();
	Rig.Thread->EnqueueAddView(ViewId, MoveTemp(Owned), FIntPoint(400, 300), MakeShared<FVaCuusViewStatus>());
	return ViewId;
}

/** AddView + a production LoadDocumentMemory command in one burst. */
inline uint32 AddSeamViewWithDocument(
	VaCuusJsDomTest::FDomTestRig& Rig, FJsDocProbeHost*& OutProbe, const TCHAR* Prefix, const TCHAR* Document)
{
	const uint32 ViewId = AddSeamView(Rig, OutProbe, Prefix);
	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, Document, /*LoadSerial=*/1);
	return ViewId;
}
}	 // namespace VaCuusJsDocumentTest

#endif	  // WITH_DEV_AUTOMATION_TESTS
