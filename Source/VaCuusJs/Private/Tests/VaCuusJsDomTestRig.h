// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

/*
 * The DOM-facade tests' shared scaffolding (M4 Tasks 4-5): a real UI thread, a
 * real Rml::Context and document behind the M3b probe-host shape, and the real
 * FVaCuusJsScriptHost wrapped with a closure queue -- the one in-band way to
 * run facade code ON the UI thread before Task 6 lands the command plumbing.
 * Extracted verbatim from VaCuusJsDomTest.cpp when the Task 5 event tests
 * needed the identical rig; everything lives in namespace VaCuusJsDomTest and
 * is header-inline (the FWrappedDomHost statics are C++17 inline members, one
 * definition across the test TUs -- which share them by design: automation
 * tests run sequentially and each rig Boot()/teardown pair resets the state).
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VaCuus.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusJs.h"
#include "VaCuusJsRuntime.h"
#include "VaCuusJsScriptHost.h"
#include "VaCuusJsViewContext.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

#include "quickjs.h"

#include <RmlUi/Core.h>

#include <atomic>
#include <cstring>

namespace VaCuusJsDomTest
{
/**
 * A real Rml::Context + document host, the M3b probe shape
 * (VaCuusModelTestHost.h's FProbeHost) minus the model capture: the facade
 * tests need a real element tree loaded through the production LoadDocument
 * path, not observations of data binding. GetDocument() is the handle the
 * bind-closure hands to FVaCuusJsScriptHost::BindDocumentForTest -- UI-thread
 * reads only, like every other member.
 */
class FDomProbeHost final : public IVaCuusDocumentHost
{
public:
	explicit FDomProbeHost(const TCHAR* InContextPrefix)
		: ContextPrefix(InContextPrefix)
	{
	}

	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		check(FVaCuusUIThread::IsInUIThread());
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

		Rml::ElementDocument* NewDocument =
			Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*RmlSource)), "vacuus://js_dom_test.rml");
		if (NewDocument == nullptr)
		{
			Report(LoadSerial, /*bSuccess=*/false);
			return;
		}

		// Load first, close second -- FVaCuusRmlDocumentHost::AdoptDocument's order.
		CloseDocument();
		RmlDocument = NewDocument;
		RmlDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
		Report(LoadSerial, /*bSuccess=*/true);
	}

	virtual void CloseDocument() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (RmlDocument != nullptr)
		{
			RmlDocument->Close();
			RmlDocument = nullptr;
		}
	}

	virtual void SetVisible(bool /*bVisible*/) override {}

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

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());
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
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* RmlDocument = nullptr;
	FIntPoint ViewSize = FIntPoint::ZeroValue;
};

/**
 * The pump integration test's wrapped-host shape (VaCuusJsPumpTest.cpp): the
 * REAL FVaCuusJsScriptHost, every seam call forwarded, plus a test-fed closure
 * queue drained at the top of PumpFrame -- the one in-band way to run facade
 * code ON the UI thread before Task 6 lands the command plumbing. BootParams
 * lets a test configure the inner host (the cache-hygiene test parks the GC
 * step out of reach so ITS explicit collection is the only one that can run).
 */
class FWrappedDomHost final : public IVaCuusScriptHost
{
public:
	static inline FVaCuusJsScriptHost* Inner = nullptr;
	static inline TQueue<TFunction<void()>, EQueueMode::Spsc> RunQueue;
	static inline std::atomic<uint64> ClosuresRun{0};
	static inline FVaCuusJsScriptHost::FParams BootParams;

	FWrappedDomHost()
		: Real(MakeUnique<FVaCuusJsScriptHost>(BootParams))
	{
		RunQueue.Empty();
		Inner = Real.Get();
	}

	virtual ~FWrappedDomHost() override { Inner = nullptr; }

	virtual void OnViewAdded(uint32 ViewId) override { Real->OnViewAdded(ViewId); }
	virtual void OnViewRemoved(uint32 ViewId) override { Real->OnViewRemoved(ViewId); }
	virtual void OnDocumentReady(uint32 ViewId, Rml::ElementDocument* Document) override
	{
		Real->OnDocumentReady(ViewId, Document);
	}
	virtual void OnDocumentClosing(uint32 ViewId) override { Real->OnDocumentClosing(ViewId); }
	virtual void PumpFrame(double NowSeconds) override
	{
		TFunction<void()> Closure;
		while (RunQueue.Dequeue(Closure))
		{
			Closure();
			ClosuresRun.fetch_add(1, std::memory_order_release);
		}
		Real->PumpFrame(NowSeconds);
	}
	virtual void CollectGarbage(const TCHAR* Reason) override { Real->CollectGarbage(Reason); }
	virtual void ExecuteScript(uint32 ViewId, const FString& Source, const FString& SourceName) override
	{
		Real->ExecuteScript(ViewId, Source, SourceName);
	}
	virtual void OnInlineFrameEntry() override { Real->OnInlineFrameEntry(); }
	virtual void Shutdown() override { Real->Shutdown(); }

private:
	TUniquePtr<FVaCuusJsScriptHost> Real;
};

/** One UI frame at a time; the wake event coalesces (the M3/seam test pattern). */
inline bool PumpRealFrames(FVaCuusUIThread& Thread, int32 Count)
{
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const uint64 Before = Thread.GetFrameCount();
		Thread.Trigger();
		if (!Thread.WaitForFrameCount(Before + 1, 5.0))
		{
			return false;
		}
	}
	return true;
}

/**
 * The tests' read-back channel, from the pump tests verbatim: evaluates Source
 * and renders the completion value as a string. UI-thread only here -- the
 * facade thunks it exercises call straight into RmlUi.
 */
inline FString EvalString(FVaCuusJsScriptHost& Host, uint32 ViewId, const char* Source)
{
	FVaCuusJsViewContext* View = Host.FindViewContext(ViewId);
	if (View == nullptr || !View->IsValid())
	{
		return TEXT("<no context>");
	}
	JSContext* Ctx = View->GetContext();

	JSValue Ret = JS_Eval(Ctx, Source, std::strlen(Source), "<dom-test-probe>", JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(Ret))
	{
		JS_FreeValue(Ctx, Ret);
		JSValue Exception = JS_GetException(Ctx);
		FString Text(TEXT("<exception>"));
		if (const char* Utf8 = JS_ToCString(Ctx, Exception))
		{
			Text = FString::Printf(TEXT("<exception: %s>"), UTF8_TO_TCHAR(Utf8));
			JS_FreeCString(Ctx, Utf8);
		}
		JS_FreeValue(Ctx, Exception);
		return Text;
	}

	FString Result(TEXT("<unstringifiable>"));
	if (const char* Utf8 = JS_ToCString(Ctx, Ret))
	{
		Result = FString(UTF8_TO_TCHAR(Utf8));
		JS_FreeCString(Ctx, Utf8);
	}
	else
	{
		JS_FreeValue(Ctx, JS_GetException(Ctx));
	}
	JS_FreeValue(Ctx, Ret);
	return Result;
}

/**
 * Boots the UI thread with the wrapped host and tears everything down at scope
 * end (the pump integration test's scaffolding, shared by the facade tests).
 * RunOnUI is the ordering primitive: enqueue a closure, pump frames until the
 * atomic closure counter shows it ran -- the release there / acquire here is
 * what makes every closure-written plain member readable on the test thread,
 * and it is immune to the coalesced-trigger straggler frame that makes bare
 * WaitForFrameCount insufficient (the M3b SettledFrames lesson).
 */
struct FDomTestRig
{
	FVaCuusModule* Module = nullptr;
	IConsoleVariable* EnableCVar = nullptr;
	int32 SavedEnable = 0;
	FVaCuusUIThread* Thread = nullptr;
	bool bBooted = false;

	enum class EBoot
	{
		Ok,
		Skip,
		Fail
	};

	EBoot Boot(FAutomationTestBase& Test, const FVaCuusJsScriptHost::FParams& Params = FVaCuusJsScriptHost::FParams())
	{
		if (!FPlatformProcess::SupportsMultithreading())
		{
			Test.AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
			return EBoot::Skip;
		}
		if (FVaCuusEngine::Get().IsInitialized())
		{
			Test.AddError(TEXT("RmlUi is already up before the test"));
			return EBoot::Fail;
		}

		Module = &FVaCuusModule::Get();
		EnableCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.Js.Enable"));
		if (EnableCVar == nullptr)
		{
			Test.AddError(TEXT("vacuus.Js.Enable does not exist"));
			return EBoot::Fail;
		}
		SavedEnable = EnableCVar->GetInt();
		EnableCVar->Set(1, ECVF_SetByConsole);

		FWrappedDomHost::BootParams = Params;
		Module->SetScriptHostFactory([]() -> TUniquePtr<IVaCuusScriptHost> { return MakeUnique<FWrappedDomHost>(); });
		bBooted = true;

		Thread = Module->GetOrStartUIThread();
		if (Thread == nullptr || !Thread->HasScriptHost())
		{
			Test.AddError(TEXT("the UI thread or its script host failed to come up"));
			return EBoot::Fail;
		}
		return EBoot::Ok;
	}

	~FDomTestRig()
	{
		// Leave the process as found: thread down, cvar back, and the PRODUCTION
		// factory re-registered (the seam test's teardown shape).
		if (bBooted)
		{
			Module->StopUIThread();
			EnableCVar->Set(SavedEnable, ECVF_SetByConsole);
			Module->SetScriptHostFactory(
				[]() -> TUniquePtr<IVaCuusScriptHost> { return MakeUnique<FVaCuusJsScriptHost>(); });
			FWrappedDomHost::BootParams = FVaCuusJsScriptHost::FParams();
		}
	}

	/** Adds a probe-hosted view and loads Document into it; the probe pointer is UI-thread-use-only. */
	uint32 AddViewWithDocument(FDomProbeHost*& OutProbe, const TCHAR* Prefix, const TCHAR* Document)
	{
		TUniquePtr<FDomProbeHost> Owned = MakeUnique<FDomProbeHost>(Prefix);
		OutProbe = Owned.Get();
		const uint32 ViewId = Thread->AllocateViewId();
		Thread->EnqueueAddView(ViewId, MoveTemp(Owned), FIntPoint(400, 300), MakeShared<FVaCuusViewStatus>());
		Thread->EnqueueLoadDocumentFromMemory(ViewId, Document, /*LoadSerial=*/1);
		return ViewId;
	}

	/**
	 * Runs Fn on the UI thread (at the next PumpFrame's top) and returns once it
	 * HAS run. Commands enqueued before this call drain earlier in the same
	 * frame, so a closure always sees them applied.
	 *
	 * THE TIMEOUT MUST NOT LEAVE Fn ARMED: callers capture their own stack by
	 * reference (&Out, &bBound -- every call site in these tests), and the
	 * static RunQueue outlives the caller's frame, so a closure still queued
	 * after a timeout would run against a DEAD stack at some later drain. The
	 * abandon flag closes that: the wrapper runs Fn only under the shared
	 * state's lock while not abandoned, and every timeout path marks abandoned
	 * UNDER THE SAME LOCK before returning -- if the UI thread is mid-Fn right
	 * then, the mark blocks until Fn completes, and the captured stack is still
	 * alive for that whole wait because this very frame is what it captured.
	 * Smallest safe shape: no call-site changes, one heap-shared flag per call.
	 */
	bool RunOnUI(TFunction<void()> Fn)
	{
		struct FRunState
		{
			FCriticalSection Mutex;
			bool bAbandoned = false;
		};
		TSharedRef<FRunState, ESPMode::ThreadSafe> State = MakeShared<FRunState, ESPMode::ThreadSafe>();

		const uint64 Target = FWrappedDomHost::ClosuresRun.load(std::memory_order_acquire) + 1;
		FWrappedDomHost::RunQueue.Enqueue(
			[State, Inner = MoveTemp(Fn)]()
			{
				FScopeLock Lock(&State->Mutex);
				if (!State->bAbandoned)
				{
					Inner();
				}
			});
		for (int32 Attempt = 0; Attempt < 10; ++Attempt)
		{
			if (!PumpRealFrames(*Thread, 1))
			{
				break;
			}
			if (FWrappedDomHost::ClosuresRun.load(std::memory_order_acquire) >= Target)
			{
				return true;
			}
		}

		// Timed out (or a frame never completed): the closure may still be
		// queued. Neuter it so a later drain skips the body instead of writing
		// through the by-then-dead captures; the skipped wrapper still counts a
		// ClosuresRun tick when it eventually drains, which is harmless -- every
		// later Target is computed from the then-current counter.
		FScopeLock Lock(&State->Mutex);
		State->bAbandoned = true;
		return false;
	}

	/** EvalString via a UI-thread closure, synchronously. */
	FString Eval(uint32 ViewId, const char* Source)
	{
		FString Out(TEXT("<ui-timeout>"));
		RunOnUI([&Out, ViewId, Source]() { Out = EvalString(*FWrappedDomHost::Inner, ViewId, Source); });
		return Out;
	}
};
}	 // namespace VaCuusJsDomTest

#endif	  // WITH_DEV_AUTOMATION_TESTS
