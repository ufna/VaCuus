// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The shared half of every REAL-CONTEXT probe host in the four test suites (bead
 * VaCuus-akj.6.15). See VaCuusTestNullDocumentHost.h for why both files live in Internal/ and
 * for the do-nothing host; this one includes RmlUi, so it is reachable from VaCuus,
 * VaCuusRender and VaCuusJs but NOT from VaCuusEditor, which does not link VaCuusRml.
 *
 * WHAT A PROBE HOST IS FOR, kept here because it was restated in a dozen places. The subject of
 * these tests is real RmlUi state -- hover, focus, a data-bound value in the DOM, a recorded
 * command list -- reached through the real UI thread, and nothing else: no RHI, no viewport, no
 * PIE. IVaCuusDocumentHost is the seam that allows it (VaCuusRender.Build.cs states the same
 * thing as a supported extension point), and the production host FVaCuusRmlDocumentHost is
 * unreachable from three of the four modules anyway, by module direction.
 *
 * WHAT THIS BASE OWNS, and it is exactly the part that had no variance across 20 copies: the
 * context's creation and removal, the AdoptDocument ordering, the load-result stamp into
 * FVaCuusViewStatus, and the five members every copy declared identically. RecordAndPublishFrame
 * and SetVisible are deliberately NOT provided -- they stay pure from the interface, because
 * RecordAndPublishFrame IS the measurement in every one of these tests and SetVisible is a
 * genuine three-way split (no-op / Show-Hide) that only one test drives at all
 * (VaCuusSnapshotTest.cpp:478 is the sole EnqueueSetVisible in the suite).
 *
 * THREAD HAND-OFF, the rule every derived probe inherits: observations are plain members
 * written on the UI thread and read on the test thread with no lock and no atomic. That is
 * sound, not sloppy -- the test only reads them after WaitForFrameCount() has seen the frame
 * counter advance, and the UI thread stores that counter with release ordering AFTER RunFrame()
 * returns (FVaCuusUIThread::Run). The acquire load therefore happens-after every write made
 * during the frame. A host that appends to a TArray per frame needs Reserve() as well, for the
 * straggler a coalesced trigger can still append; see FDataForProbeHost for that argument.
 */
class FVaCuusTestDocumentHost : public IVaCuusDocumentHost
{
public:
	/**
	 * @param InContextPrefix   Context name prefix; Initialize() appends "_<ViewId>" so N views
	 *                          in one test get N distinct RmlUi contexts.
	 * @param InDocumentUrl     The source URL handed to Context::LoadDocumentFromMemory. RmlUi
	 *                          resolves the document's relative links against it.
	 * @param InFocusFlag       REQUIRED, no default, because the two values are not
	 *                          interchangeable and a default would silently pick one. RmlUi's own
	 *                          default is FocusFlag::Auto (ElementDocument.h:81), which focuses
	 *                          the first tabbable element; FocusFlag::Document focuses the
	 *                          document itself and nothing inside it
	 *                          (ElementDocument.cpp:347-359 is the switch). Both are load-bearing
	 *                          somewhere in this suite: VaCuus.Input.SpatialNav needs focus
	 *                          INSIDE a document or every arrow key is dropped, and the text
	 *                          entry suite's controller decision D9 needs a document-only focus
	 *                          NOT to count as "the UI wants the keyboard".
	 */
	FVaCuusTestDocumentHost(const TCHAR* InContextPrefix, const char* InDocumentUrl, Rml::FocusFlag InFocusFlag)
		: ContextPrefix(InContextPrefix)
		, DocumentUrl(InDocumentUrl)
		, DocumentFocusFlag(InFocusFlag)
	{
	}

	//~ Begin IVaCuusDocumentHost

	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		ViewId = InViewId;
		Status = InStatus;
		ContextName = FString::Printf(TEXT("%s_%u"), *ContextPrefix, InViewId);

		// 1x1 rather than the view's real size: AddView applies that immediately afterwards
		// through SetViewSize (VaCuusUIThread.cpp:1492-1494), and a probe that is never given a
		// positive size must NOT look laid out. A null render interface is the normal case --
		// FVaCuusEngine installs one globally when nobody supplied a real one, and
		// Rml::CreateContext falls back to it; CreateRenderInterface() is the hook for the two
		// probes that record real commands.
		Context = Rml::CreateContext(Rml::String(TCHAR_TO_UTF8(*ContextName)), Rml::Vector2i(1, 1), CreateRenderInterface());
		if (Context == nullptr)
		{
			return false;
		}

		return OnInitialized();
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

		// AFTER the context is gone, which is an ordering constraint and not a preference: a
		// data-bound probe frees its model shadow here, and RmlUi holds a raw void* into that
		// shadow with no unbind API, so the context has to die first.
		OnShutdown();
	}

	virtual void SetViewSize(FIntPoint InViewSize) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		// The interface promises idempotence (VaCuusDocumentHost.h:68). RmlUi already guards the
		// dimension write itself (Context.cpp:131), so the clause that earns its keep here is the
		// non-positive one: it keeps ViewSize -- which HasView() reads -- from ever holding a
		// degenerate size. Every probe in this suite receives exactly one SetViewSize call, from
		// AddView, which filters non-positive sizes before it calls at all
		// (VaCuusUIThread.cpp:1492-1494); the only EnqueueResize in the corpus drives the
		// production host, not a probe (VaCuusUnsizedDrainTest.cpp:225).
		if (InViewSize == ViewSize || InViewSize.X <= 0 || InViewSize.Y <= 0)
		{
			return;
		}

		ViewSize = InViewSize;
		if (Context != nullptr)
		{
			Context->SetDimensions(Rml::Vector2i(ViewSize.X, ViewSize.Y));
		}
	}

	/** Not exercised by most probes: they load from memory. The two that need a real file load
	 *  override this and hand the result to AdoptDocument(). */
	virtual void LoadDocumentFromFile(const FString& /*VfsPath*/, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		ReportLoadResult(LoadSerial, /*bSuccess=*/false);
	}

	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (Context == nullptr)
		{
			ReportLoadResult(LoadSerial, /*bSuccess=*/false);
			return;
		}

		AdoptDocument(Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), DocumentUrl), LoadSerial);
	}

	virtual void CloseDocument() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (RmlDocument != nullptr)
		{
			OnDocumentClosing();
			RmlDocument->Close();
			RmlDocument = nullptr;
		}
	}

	/**
	 * The production host's shape, and the size clause is the one that matters:
	 * FVaCuusRmlDocumentHost::HasView() also requires a positive ViewSize, so a view that has not
	 * been laid out yet is not recorded. VaCuus.Model.Apply's sizeless view never satisfies this,
	 * which is exactly the case the data apply must not be gated on.
	 */
	virtual bool HasView() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context != nullptr && RmlDocument != nullptr && ViewSize.X > 0 && ViewSize.Y > 0;
	}

	virtual Rml::Context* GetContext() const override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Context;
	}

	//~ End IVaCuusDocumentHost -- SetVisible() and RecordAndPublishFrame() stay pure on purpose;
	//~ see the class comment.

	/** UI-thread only, for phase actions and bind closures that need the tree. */
	Rml::ElementDocument* GetDocument() const
	{
		check(FVaCuusUIThread::IsInUIThread());
		return RmlDocument;
	}

protected:
	/**
	 * The per-view render interface, created before Rml::CreateContext and owned by the derived
	 * host. Null (the default) means "use the engine's global null interface", which is what a
	 * probe that never calls Context::Render() wants.
	 *
	 * A HOST THAT RETURNS ONE MUST OUTLIVE Rml::Shutdown(): the production host's rule, because
	 * Rml::Shutdown() destroys the RenderManager keyed on that pointer and releases this view's
	 * font textures THROUGH it. Keep it in a member that Shutdown() does not reset.
	 */
	virtual Rml::RenderInterface* CreateRenderInterface() { return nullptr; }

	/** Runs at the end of a successful Initialize(), with the context live: the place to bind a
	 *  data model (which `data-model` requires to happen before any document parses -- it is read
	 *  exactly once, in Element::SetParent) or to Reserve() a per-frame log. */
	virtual bool OnInitialized() { return true; }

	/** Runs at the end of Shutdown(), after the context is gone. See Shutdown() for why the order
	 *  is a constraint. */
	virtual void OnShutdown() {}

	/** Runs after the new document is parented and shown, before the load result is stamped: the
	 *  spec 2(f) script-host seam, and the place to count loads. */
	virtual void OnDocumentAdopted() {}

	/** Runs while the outgoing document is still current, before Close(): the production
	 *  CloseDocument's order, which is what lets unload scripts see a live tree. */
	virtual void OnDocumentClosing() {}

	/**
	 * FVaCuusRmlDocumentHost::AdoptDocument's ordering, and it is load-bearing: the new document
	 * is LOADED FIRST and CLOSED SECOND, so both are alive for an instant and the new one is
	 * parented -- and therefore resolves `data-model` -- while the model is still fully live.
	 * That is what makes "do nothing to the model on reload" safe.
	 *
	 * Takes an already-loaded document precisely so the ordering cannot be got wrong by a caller:
	 * whatever produced it ran before this function did.
	 */
	void AdoptDocument(Rml::ElementDocument* NewDocument, uint64 LoadSerial)
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (NewDocument == nullptr)
		{
			ReportLoadResult(LoadSerial, /*bSuccess=*/false);
			return;
		}

		CloseDocument();
		RmlDocument = NewDocument;
		RmlDocument->Show(Rml::ModalFlag::None, DocumentFocusFlag);
		OnDocumentAdopted();
		ReportLoadResult(LoadSerial, /*bSuccess=*/true);
	}

	/** How a load result reaches the game thread. Serial 0 means "nobody is waiting". */
	void ReportLoadResult(uint64 LoadSerial, bool bSuccess)
	{
		if (Status.IsValid() && LoadSerial != 0)
		{
			Status->LoadResult.store(
				static_cast<uint8>(bSuccess ? EVaCuusLoadResult::Succeeded : EVaCuusLoadResult::Failed), std::memory_order_relaxed);
			Status->LoadCompletedSerial.store(LoadSerial, std::memory_order_release);
		}
	}

	TSharedPtr<FVaCuusViewStatus> Status;
	FString ContextName;
	uint32 ViewId = 0;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* RmlDocument = nullptr;
	FIntPoint ViewSize = FIntPoint::ZeroValue;

private:
	const FString ContextPrefix;
	const char* const DocumentUrl;
	const Rml::FocusFlag DocumentFocusFlag;
};

#endif	  // WITH_DEV_AUTOMATION_TESTS
