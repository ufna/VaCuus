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

namespace VaCuusJsDocumentTest
{
/**
 * Mirrors FVaCuusRmlDocumentHost::AdoptDocument/CloseDocument line for line
 * where the seam is concerned (load first, close second, Show(), then
 * OnDocumentReady; OnDocumentClosing before Close()). The production host
 * itself is unreachable from VaCuusJs -- module direction -- and its own
 * wiring is proven from VaCuusRender's test suite.
 */
class FJsDocProbeHost final : public IVaCuusDocumentHost
{
public:
	explicit FJsDocProbeHost(const TCHAR* InContextPrefix)
		: ContextPrefix(InContextPrefix)
	{
	}

	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		ViewId = InViewId;
		Status = InStatus;
		ContextName = FString::Printf(TEXT("%s_%u"), *ContextPrefix, InViewId);
		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1));
		return Context != nullptr;
	}

	virtual void Shutdown() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		CloseDocument();
		if (Context != nullptr)
		{
			Rml::RemoveContext(Rml::String(TCHAR_TO_UTF8(*ContextName)));
			Context = nullptr;
		}
	}

	virtual void SetViewSize(FIntPoint InViewSize) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		ViewSize = InViewSize;
		if (Context != nullptr)
		{
			Context->SetDimensions(Rml::Vector2i(ViewSize.X, ViewSize.Y));
		}
	}

	virtual void LoadDocumentFromFile(const FString& /*VfsPath*/, uint64 LoadSerial) override
	{
		Report(LoadSerial, /*bSuccess=*/false);
	}

	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (Context == nullptr)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		// Load FIRST, close second -- AdoptDocument's order (two documents alive
		// for an instant; the data-binding reload rule rests on it).
		Rml::ElementDocument* NewDocument =
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://js_doc_test.rml");
		if (NewDocument == nullptr)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		CloseDocument();
		RmlDocument = NewDocument;
		RmlDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);

		// The spec 2(f) seam call, exactly where the production host makes it:
		// after old-close (unload JS ran in the old context) and after Show().
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentReady(ViewId, RmlDocument);
		}

		Report(LoadSerial, /*bSuccess=*/true);
	}

	virtual void CloseDocument() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (RmlDocument != nullptr)
		{
			// Unload JS BEFORE Close(), while the document is still current --
			// the production CloseDocument's order.
			if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
			{
				ScriptHost->OnDocumentClosing(ViewId);
			}
			RmlDocument->Close();
			RmlDocument = nullptr;

			// The production host's one-more-frame rule
			// (IVaCuusDocumentHost::CloseDocument), mirrored for its SIDE
			// EFFECT: that frame's Context::Update() is what actually frees the
			// closed tree (Close only queues it), and the unload test's
			// dead-wrapper assertion is about the state AFTER that free -- the
			// state a production view reaches one frame after every close.
			bOwesUpdateFrame = true;
		}
	}

	virtual void SetVisible(bool /*bVisible*/) override {}

	virtual bool HasView() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context != nullptr && (RmlDocument != nullptr || bOwesUpdateFrame) && ViewSize.X > 0 && ViewSize.Y > 0;
	}

	virtual Rml::Context* GetContext() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context;
	}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		bOwesUpdateFrame = false;
		Context->Update();
		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	/** UI-thread only (closures). */
	Rml::ElementDocument* GetDocument() const
	{
		check(FVaCuusUIThread::IsInUIThread());
		return RmlDocument;
	}

private:
	void Report(uint64 LoadSerial, bool bSuccess)
	{
		if (Status.IsValid() && LoadSerial != 0)
		{
			Status->LoadResult.store(
				static_cast<uint8>(bSuccess ? EVaCuusLoadResult::Succeeded : EVaCuusLoadResult::Failed),
				std::memory_order_relaxed);
			Status->LoadCompletedSerial.store(LoadSerial, std::memory_order_release);
		}
	}

	TSharedPtr<FVaCuusViewStatus> Status;
	FString ContextPrefix;
	FString ContextName;
	uint32 ViewId = 0;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* RmlDocument = nullptr;
	FIntPoint ViewSize = FIntPoint::ZeroValue;

	/** See CloseDocument: one post-close Update, the production host's debt. */
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
