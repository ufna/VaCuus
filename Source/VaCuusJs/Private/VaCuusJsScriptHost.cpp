// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusJsScriptHost.h"

#include "VaCuusJs.h"
#include "VaCuusJsRuntime.h"

FVaCuusJsScriptHost::FVaCuusJsScriptHost() = default;

FVaCuusJsScriptHost::~FVaCuusJsScriptHost()
{
	// Normally already null: the UI thread pairs every creation with a Shutdown()
	// call in Exit(). Destroying a live runtime here is still correct -- same
	// thread, same order -- just unplanned.
	Shutdown();
}

void FVaCuusJsScriptHost::OnViewAdded(uint32 /*ViewId*/)
{
	// (view contexts: M4 Task 3) Contexts are created on demand at the first
	// script, not at view registration -- there is nothing to do here yet.
}

void FVaCuusJsScriptHost::OnViewRemoved(uint32 /*ViewId*/)
{
	// (view contexts: M4 Task 3) Frees the view's JSContext and its listener
	// registry against the still-live Rml tree (the caller guarantees the order).
}

void FVaCuusJsScriptHost::OnDocumentReady(uint32 /*ViewId*/, Rml::ElementDocument* /*Document*/)
{
	// (documents: M4 Task 6) The host-ordered recycle-and-run point (spec 2(f)).
}

void FVaCuusJsScriptHost::OnDocumentClosing(uint32 /*ViewId*/)
{
	// (documents: M4 Task 6) Unload JS dispatch, at Close() time.
}

void FVaCuusJsScriptHost::PumpFrame(double /*NowSeconds*/)
{
	// (pump internals: M4 Task 3) rAF -> due timers -> the bounded job drain, in
	// that order (spec 3.5), against every view context. No runtime, no work.
}

void FVaCuusJsScriptHost::CollectGarbage(const TCHAR* Reason)
{
	if (Runtime.IsValid())
	{
		Runtime->CollectGarbage(Reason);
	}
}

void FVaCuusJsScriptHost::ExecuteScript(uint32 ViewId, const FString& /*Source*/, const FString& SourceName)
{
	// A named refusal rather than a silent drop -- the BindModel lesson
	// (VaCuusUIThread.cpp's drain): losing a script silently is this seam's
	// quietest failure. Nothing enqueues this before Task 6; anything that
	// reaches it earlier deserves a loud answer.
	UE_LOG(LogVaCuusJS, Error,
		TEXT("ExecuteScript('%s') for view %u dropped: per-view JS contexts land in M4 Task 3, the command plumbing in Task 6"),
		*SourceName, ViewId);
}

void FVaCuusJsScriptHost::OnInlineFrameEntry()
{
	if (Runtime.IsValid())
	{
		Runtime->UpdateStackTopOnThisThread();
	}
}

void FVaCuusJsScriptHost::Shutdown()
{
	// (per-view teardown: M4 Task 3) Contexts and listener registries go down
	// here, per view, BEFORE the runtime -- free every context (and every held
	// JSValue) before JS_FreeRuntime (research note quickjs-ng-0151.md section 2).
	// With no contexts yet, destroying the runtime is the whole task; its
	// destructor checks the live-byte counter back to zero.
	Runtime.Reset();
}
