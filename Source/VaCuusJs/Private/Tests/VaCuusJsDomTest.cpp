// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

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
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeExit.h"

#include "quickjs.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Factory.h>

#include <atomic>
#include <cstring>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * THE DOM FACADE, END TO END (M4 Task 4 spec 3.9 / plan 4.2): every RmlUi call
 * the facade makes is UI-thread-only, so unlike the pump's library-level tests
 * these all boot the REAL UI thread with a REAL Rml::Context and document --
 * the M3b probe-host pattern (VaCuusModelTestHost.h) -- and drive JS through
 * closures executed at the top of the script host's PumpFrame, ON that thread.
 * The test thread only enqueues closures and reads results after the
 * closure-serial handshake (RunOnUI below) has ordered them.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDomFacadeTest, "VaCuus.Js.Dom.Facade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDomCreateElementLowercaseTest, "VaCuus.Js.Dom.CreateElementLowercase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDomTwoViewIsolationTest, "VaCuus.Js.Dom.TwoViewIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsDomCacheHygieneTest, "VaCuus.Js.Dom.CacheHygiene",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

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
bool PumpRealFrames(FVaCuusUIThread& Thread, int32 Count)
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
FString EvalString(FVaCuusJsScriptHost& Host, uint32 ViewId, const char* Source)
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
 * end (the pump integration test's scaffolding, shared by four tests). RunOnUI
 * is the ordering primitive: enqueue a closure, pump frames until the atomic
 * closure counter shows it ran -- the release there / acquire here is what
 * makes every closure-written plain member readable on the test thread, and
 * it is immune to the coalesced-trigger straggler frame that makes bare
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
	 */
	bool RunOnUI(TFunction<void()> Fn)
	{
		const uint64 Target = FWrappedDomHost::ClosuresRun.load(std::memory_order_acquire) + 1;
		FWrappedDomHost::RunQueue.Enqueue(MoveTemp(Fn));
		for (int32 Attempt = 0; Attempt < 10; ++Attempt)
		{
			if (!PumpRealFrames(*Thread, 1))
			{
				return false;
			}
			if (FWrappedDomHost::ClosuresRun.load(std::memory_order_acquire) >= Target)
			{
				return true;
			}
		}
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

/**
 * The facade surface on one real document: identity (`===` across lookups),
 * traversal, attributes, the classList staleness trap observed from BOTH
 * sides, the style proxy's copy-at-once get and parse-success set, the two
 * querySelector deviations, the full detached lifecycle
 * (createElement -> configure -> append -> removeChild -> re-append), and
 * dead handles reading null/false/undefined -- never throwing -- after
 * remove() and after an innerRML subtree replacement.
 */
bool FVaCuusJsDomFacadeTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsDomTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// No text nodes anywhere, so no font is needed and no layout warning can
	// fire; single-line body so the tree is exactly the elements named.
	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body><div id="root" class="rootclass"><div id="kid" class="a"><div id="grandkid" class="deep"/></div><div id="sib"/></div></body>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	const uint32 ViewId = Rig.AddViewWithDocument(Probe, TEXT("vacuus_jsdom_facade"), GDocument);

	// Bind the real document to the view's context through the test-only host
	// entry (production wiring is Task 6's OnDocumentReady).
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

	// A: identity and traversal. getElementById twice ==='s, querySelector of
	// the same node is the same wrapper, parentNode of a top-level element IS
	// the document wrapper, and document.body === document (the RmlUi collapse).
	TestEqual(TEXT("identity + traversal"),
		Rig.Eval(ViewId,
			"const root = document.getElementById('root');"
			"const again = document.getElementById('root');"
			"const viaQ = document.querySelector('#root');"
			"[root === again, root === viaQ, root.tagName, root.id,"
			" root.parentNode === document, document.body === document,"
			" root.children.length, root.children[0].id,"
			" root.children[0] === document.getElementById('kid'),"
			" document.getElementById('nope')].map(String).join('|')"),
		FString(TEXT("true|true|div|root|true|true|2|kid|true|null")));

	// B: classList over SetClass/IsClassSet -- and the class ATTRIBUTE going
	// stale, observed from JS. The C++ half below observes the same divergence
	// through GetClassNames() vs GetAttribute("class") (Element.cpp:258-276).
	TestEqual(TEXT("classList vs the stale class attribute"),
		Rig.Eval(ViewId,
			"const kid = document.getElementById('kid');"
			"kid.classList.add('marked');"
			"const r = [kid.classList.contains('marked'), kid.getAttribute('class')];"
			"r.push(kid.classList.toggle('marked'));"	 // off -> false
			"r.push(kid.classList.toggle('marked'));"	 // on -> true
			"kid.classList.remove('a');"
			"r.push(kid.classList.contains('a'), kid.getAttribute('class'));"
			"r.map(String).join('|')"),
		FString(TEXT("true|a|false|true|false|a")));

	FString ClassNamesSeen, ClassAttributeSeen;
	Rig.RunOnUI([&ClassNamesSeen, &ClassAttributeSeen, Probe]()
		{
			Rml::Element* Kid = Probe->GetDocument()->GetElementById("kid");
			ClassNamesSeen = UTF8_TO_TCHAR(Kid->GetClassNames().c_str());
			ClassAttributeSeen = UTF8_TO_TCHAR(Kid->GetAttribute<Rml::String>("class", "<none>").c_str());
		});
	TestEqual(TEXT("GetClassNames tells the truth (a removed, marked on)"), ClassNamesSeen, FString(TEXT("marked")));
	TestEqual(TEXT("the class attribute is stale -- BOTH sides of the trap observed"), ClassAttributeSeen,
		FString(TEXT("a")));

	// C: the style proxy. The first read is copied at once (Element.h:187-193's
	// invalidation contract): a later write -- an RmlUi call that invalidates
	// the Property* -- must not reach back into the already-returned string.
	// setProperty returns parse success; the failed parse also logs RmlUi's
	// syntax-error warning, expected exactly once.
	AddExpectedMessagePlain(TEXT("Syntax error parsing inline property declaration"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("style: copy-at-once get, parse-success set"),
		Rig.Eval(ViewId,
			"kid.style.width = '123px';"
			"const w = kid.style.width;"
			"kid.style.width = '55px';"
			"const ok = kid.style.setProperty('height', '10px');"
			"const bad = kid.style.setProperty('width', 'garbage-value');"
			"kid.style.removeProperty('height');"
			"[w, kid.style.width, ok, bad].map(String).join('|')"),
		FString(TEXT("123px|55px|true|false")));

	FString WidthViaDom;
	Rig.RunOnUI([&WidthViaDom, Probe]()
		{
			// The DOM-side read of the same property -- copied at once, same rule.
			Rml::Element* Kid = Probe->GetDocument()->GetElementById("kid");
			const Rml::Property* Width = Kid->GetProperty("width");
			WidthViaDom = Width != nullptr ? UTF8_TO_TCHAR(Width->ToString().c_str()) : TEXT("<null>");
		});
	TestEqual(TEXT("the DOM probe agrees with the proxy"), WidthViaDom, FString(TEXT("55px")));

	// D: the two documented deviations, pinned. querySelector never matches the
	// element itself (Element.cpp:1540-1546 recurses over children only);
	// closest starts at the PARENT (Element.cpp:1077-1090), so an element never
	// closest-matches its own selector the way DOM's closest() would.
	TestEqual(TEXT("querySelector/closest deviations"),
		Rig.Eval(ViewId,
			"[root.querySelector('#root'),"			// self: never matches -> null
			" root.querySelector('#kid') === kid,"
			" kid.closest('#kid'),"					// self: excluded -> null
			" kid.closest('#root') === root,"
			" kid.closest('div') === root,"			// first div ANCESTOR, not kid itself
			" root.querySelectorAll('div').length].map(String).join('|')"),
		FString(TEXT("null|true|null|true|true|3")));

	// E: the detached lifecycle. createElement owns; configuring a detached
	// node works; appendChild moves ownership into the parent and returns the
	// SAME wrapper; removeChild returns it OWNING again (still fully alive);
	// re-appending elsewhere works; insertBefore orders; and the cycle guard
	// refuses ancestor-into-descendant as null.
	TestEqual(TEXT("detached lifecycle"),
		Rig.Eval(ViewId,
			"const fresh = document.createElement('div');"
			"fresh.id = 'fresh';"
			"fresh.setAttribute('data-x', '1');"
			"fresh.style.width = '10px';"
			"const r0 = [fresh.parentNode, document.getElementById('fresh')];"
			"const ret = root.appendChild(fresh);"
			"const r1 = [ret === fresh, fresh.parentNode === root,"
			"            document.getElementById('fresh') === fresh, root.children.length];"
			"const back = root.removeChild(fresh);"
			"const r2 = [back === fresh, fresh.parentNode, document.getElementById('fresh'),"
			"            fresh.id, fresh.getAttribute('data-x'), fresh.style.width];"
			"const sib = document.getElementById('sib');"
			"sib.appendChild(fresh);"
			"const r3 = [fresh.parentNode === sib, sib.children[0] === fresh];"
			"const first = document.createElement('div'); first.id = 'first';"
			"root.insertBefore(first, kid);"
			"const r4 = [root.children[0] === first, root.children.length];"
			"const r5 = [root.appendChild(root.parentNode), kid.appendChild(root)];"	 // cycles: refused as null
			"r0.concat(r1, r2, r3, r4, r5).map(String).join('|')"),
		FString(TEXT("null|null|true|true|true|3|true|null|null|fresh|1|10px|true|true|true|3|null|null")));

	// F: innerRML set destroys the replaced children synchronously
	// (Element.cpp:1167-1177 discards each RemoveChild return;
	// rmlui-scripting.md section 6): the grandkid wrapper reads dead through
	// its ENTIRE surface before the setter even returns, and nothing throws.
	TestEqual(TEXT("dead handles after innerRML replacement -- null everywhere, no throw"),
		Rig.Eval(ViewId,
			"const g = document.getElementById('grandkid');"
			"kid.innerRML = '<div id=\"newkid\"/>';"
			"const dead = [g.id, g.tagName, g.parentNode, g.innerRML, g.getAttribute('class'),"
			" g.classList.contains('deep'), g.style.width, g.querySelector('div'),"
			" g.querySelectorAll('div').length, g.closest('div'),"
			" g.appendChild(document.createElement('div')), g.removeChild(kid), g.children.length];"
			"let threw = 'no-throw';"
			"try { g.remove(); g.setAttribute('a','b'); g.removeAttribute('a'); g.style.width = '5px';"
			"      g.classList.add('x'); g.id = 'zz'; g.innerRML = 'x'; }"
			"catch (e) { threw = 'threw:' + e; }"
			"const after = [kid.children.length, kid.children[0] === document.getElementById('newkid'),"
			"               kid.innerRML.includes('newkid')];"
			"dead.concat([threw], after).map(String).join('|')"),
		FString(TEXT("null|null|null|null|null|false|null|null|0|null|null|null|0|no-throw|1|true|true")));

	// G: remove() kills -- the element, its subtree (fresh lives inside sib),
	// and every lookup path to them.
	TestEqual(TEXT("remove() kills the subtree and its handles"),
		Rig.Eval(ViewId,
			"sib.remove();"
			"[sib.id, document.getElementById('sib'), fresh.id, root.children.length].map(String).join('|')"),
		FString(TEXT("null|null|null|2")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

/**
 * E1, RESTORE-THE-BUG (spec 3.9 / rmlui-scripting.md section 2): instancer
 * names are registered lowercased but looked up verbatim (Factory.cpp:297 vs
 * :302), and the Element ctor's lowercase assert (Element.cpp:76) is compiled
 * out under NDEBUG -- so an uppercase tag silently instances a generic element
 * that no RCSS rule ever matches. Green half: the facade's createElement('DIV')
 * lowercases, and the `div { width: 123px; }` rule applies -- observed through
 * a property the rule sets, read back through GetProperty on both sides of the
 * seam. Red half: the SAME element instanced with the lowercase dropped --
 * Factory::InstanceElement("DIV") directly, exactly what createElement would do
 * without its ToLower line -- and the style does NOT apply.
 */
bool FVaCuusJsDomCreateElementLowercaseTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsDomTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; width: 123px; }</style></head>
<body><div id="mount"/></body>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	const uint32 ViewId = Rig.AddViewWithDocument(Probe, TEXT("vacuus_jsdom_e1"), GDocument);

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

	// GREEN: through the facade, with the deliberately-wrong case.
	TestEqual(TEXT("the facade lowercased the tag"),
		Rig.Eval(ViewId,
			"const el = document.createElement('DIV');"
			"el.id = 'green';"
			"document.getElementById('mount').appendChild(el);"
			"el.tagName"),
		FString(TEXT("div")));

	// RED: the lowercase dropped -- the raw Factory call createElement wraps
	// (ElementDocument.cpp:427-430), fed the tag verbatim. No assert fires
	// (NDEBUG); the element simply exists with tag "DIV".
	Rig.RunOnUI([Probe]()
		{
			Rml::ElementPtr Raw = Rml::Factory::InstanceElement(nullptr, "DIV", "DIV", Rml::XMLAttributes());
			Raw->SetId("red");
			Probe->GetDocument()->GetElementById("mount")->AppendChild(std::move(Raw));
		});

	// One recorded frame has passed per closure (the record loop's
	// Context::Update follows the pump in the same frame), so both elements'
	// style definitions are resolved by now. Probe both sides.
	FString GreenTag, RedTag, GreenWidth, RedWidth;
	Rig.RunOnUI([&, Probe]()
		{
			Rml::ElementDocument* Document = Probe->GetDocument();
			Rml::Element* Green = Document->GetElementById("green");
			Rml::Element* Red = Document->GetElementById("red");
			if (Green != nullptr)
			{
				GreenTag = UTF8_TO_TCHAR(Green->GetTagName().c_str());
				const Rml::Property* Width = Green->GetProperty("width");
				GreenWidth = Width != nullptr ? UTF8_TO_TCHAR(Width->ToString().c_str()) : TEXT("<null>");
			}
			if (Red != nullptr)
			{
				RedTag = UTF8_TO_TCHAR(Red->GetTagName().c_str());
				const Rml::Property* Width = Red->GetProperty("width");
				RedWidth = Width != nullptr ? UTF8_TO_TCHAR(Width->ToString().c_str()) : TEXT("<null>");
			}
		});

	TestEqual(TEXT("green: tag is lowercase"), GreenTag, FString(TEXT("div")));
	TestEqual(TEXT("green: the div rule applied"), GreenWidth, FString(TEXT("123px")));
	TestEqual(TEXT("green via the proxy too"), Rig.Eval(ViewId, "document.getElementById('green').style.width"),
		FString(TEXT("123px")));

	TestEqual(TEXT("red: the uppercase tag survived instancing verbatim"), RedTag, FString(TEXT("DIV")));
	TestNotEqual(TEXT("red: the SAME rule does NOT apply -- the E1 silent miss, observed"), RedWidth,
		FString(TEXT("123px")));
	AddInfo(FString::Printf(TEXT("red element's width resolved to '%s' (the rule's value is 123px)"), *RedWidth));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

/**
 * Two JS views on one UI thread, two documents: each context's `document`
 * global sees only its own tree -- lookups cross neither the id namespace nor
 * the wrapper caches.
 */
bool FVaCuusJsDomTwoViewIsolationTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsDomTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	static const TCHAR* GDocumentAlpha = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body><div id="whoami" v="alpha"/><div id="only-alpha"/></body>
</rml>)");
	static const TCHAR* GDocumentBeta = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body><div id="whoami" v="beta"/><div id="only-beta"/></body>
</rml>)");

	FDomProbeHost* ProbeAlpha = nullptr;
	FDomProbeHost* ProbeBeta = nullptr;
	const uint32 ViewAlpha = Rig.AddViewWithDocument(ProbeAlpha, TEXT("vacuus_jsdom_alpha"), GDocumentAlpha);
	const uint32 ViewBeta = Rig.AddViewWithDocument(ProbeBeta, TEXT("vacuus_jsdom_beta"), GDocumentBeta);

	bool bBoundAlpha = false;
	bool bBoundBeta = false;
	Rig.RunOnUI([&]()
		{
			bBoundAlpha = ProbeAlpha->GetDocument() != nullptr;
			bBoundBeta = ProbeBeta->GetDocument() != nullptr;
			FWrappedDomHost::Inner->BindDocumentForTest(ViewAlpha, ProbeAlpha->GetDocument());
			FWrappedDomHost::Inner->BindDocumentForTest(ViewBeta, ProbeBeta->GetDocument());
		});
	if (!TestTrue(TEXT("both documents loaded and bound"), bBoundAlpha && bBoundBeta))
	{
		return false;
	}

	TestEqual(TEXT("alpha sees only its own document"),
		Rig.Eval(ViewAlpha,
			"[document.getElementById('whoami').getAttribute('v'),"
			" document.getElementById('only-beta'),"
			" document.getElementById('only-alpha') !== null].map(String).join('|')"),
		FString(TEXT("alpha|null|true")));

	TestEqual(TEXT("beta sees only its own document"),
		Rig.Eval(ViewBeta,
			"[document.getElementById('whoami').getAttribute('v'),"
			" document.getElementById('only-alpha'),"
			" document.getElementById('only-beta') !== null].map(String).join('|')"),
		FString(TEXT("beta|null|true")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

/**
 * The wrapper cache does not grow monotonically (plan 4.3's hygiene row), via
 * the context's cache-size observable -- the invariant given an observable, per
 * the house rule. Both exits proven: the DESTROY path (an innerRML clear
 * erases the dead children's entries synchronously, before any GC) and the
 * FINALIZER path (wrappers trapped in a reference cycle survive the refcount
 * and leave only when the cycle collector runs -- an explicit collection,
 * because the rig parks the frame GC trigger out of reach).
 */
bool FVaCuusJsDomCacheHygieneTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsDomTest;

	// The frame GC point must never fire on its own here, or the
	// "still-cached-before-GC" assertion races it: park the growth step at the
	// int32 ceiling (~2 TB) so the ONLY collection is the explicit one below.
	FVaCuusJsScriptHost::FParams Params;
	Params.RuntimeParams.GCStepKB = MAX_int32;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this, Params);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	static const TCHAR* GDocument = TEXT(R"(<rml>
<head><style>body { display: block; } div { display: block; }</style></head>
<body><div id="pool"/></body>
</rml>)");

	FDomProbeHost* Probe = nullptr;
	const uint32 ViewId = Rig.AddViewWithDocument(Probe, TEXT("vacuus_jsdom_cache"), GDocument);

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

	const auto CacheSize = [&Rig, ViewId]() -> int32
	{
		int32 Size = -1;
		Rig.RunOnUI([&Size, ViewId]()
			{ Size = FWrappedDomHost::Inner->FindViewContext(ViewId)->GetWrapperCacheSize(); });
		return Size;
	};

	// Baseline: the document wrapper (BindDocumentForTest) + pool.
	Rig.Eval(ViewId, "globalThis.poolRef = document.getElementById('pool'); 'ok'");
	TestEqual(TEXT("baseline: document + pool"), CacheSize(), 2);

	// THE DESTROY PATH. Eight children wrapped and referenced; clearing the
	// subtree kills the elements, and the OnElementDestroy probe erases their
	// entries INSIDE the innerRML call -- the cache is back at baseline while
	// the eight dead wrappers still sit referenced in globalThis.kids.
	Rig.Eval(ViewId,
		"poolRef.innerRML = '<div id=\"c0\"/><div id=\"c1\"/><div id=\"c2\"/><div id=\"c3\"/>'"
		"                 + '<div id=\"c4\"/><div id=\"c5\"/><div id=\"c6\"/><div id=\"c7\"/>';"
		"globalThis.kids = [];"
		"for (let i = 0; i < 8; i++) kids.push(document.getElementById('c' + i));"
		"'ok'");
	TestEqual(TEXT("eight children wrapped"), CacheSize(), 10);

	TestEqual(TEXT("the subtree clear left dead-but-referenced wrappers"),
		Rig.Eval(ViewId, "poolRef.innerRML = ''; [kids.length, kids[0].id].map(String).join('|')"),
		FString(TEXT("8|null")));
	TestEqual(TEXT("destroy path: erased at death, no GC needed"), CacheSize(), 2);

	// THE FINALIZER PATH. New children wrapped into a reference CYCLE, then
	// unrooted: refcounting alone cannot reclaim a cycle, so the entries stay
	// -- proving the cache holds no strong refs but also frees none early --
	// until the cycle collector runs.
	Rig.Eval(ViewId,
		"globalThis.kids = null;"
		"poolRef.innerRML = '<div id=\"d0\"/><div id=\"d1\"/><div id=\"d2\"/><div id=\"d3\"/>'"
		"                 + '<div id=\"d4\"/><div id=\"d5\"/><div id=\"d6\"/><div id=\"d7\"/>';"
		"globalThis.cyc = { kids: [], self: null };"
		"cyc.self = cyc;"
		"for (let i = 0; i < 8; i++) cyc.kids.push(document.getElementById('d' + i));"
		"'ok'");
	TestEqual(TEXT("eight live wrappers in the cycle"), CacheSize(), 10);

	Rig.Eval(ViewId, "globalThis.cyc = null; 'ok'");
	TestEqual(TEXT("unrooted but uncollected: the cycle pins the wrappers, the cache keeps their entries"),
		CacheSize(), 10);

	// The explicit collection (plan 4.3: CollectGarbage called explicitly).
	// JS_RunGC directly: the host's CollectGarbage would decline -- its growth
	// gate is parked at the ceiling, by this test's own design.
	Rig.RunOnUI([]() { JS_RunGC(FWrappedDomHost::Inner->GetRuntime()->GetRuntime()); });
	TestEqual(TEXT("finalizer path: the collection returned the cache to baseline"), CacheSize(), 2);

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
