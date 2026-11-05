// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

/*
 * MULTITOUCH, SECONDARY FINGERS (VaCuus patch #18, Source/ThirdParty/RmlUi/VENDORED_TAG.txt).
 *
 * Upstream RmlUi has one pointer. Every touch press runs ProcessMouseButtonDown and moves the
 * hover chain to that finger, so a second finger takes over the first one's press, click and
 * drag. These tests drive Content/DevUI/Tests/multitouch.rml through the production input path
 * (EnqueueInput -> DrainInput -> Context::ProcessTouch*) and pin what a secondary finger gets:
 *
 *  - two fingers on two buttons click their own button (TwoButtonsClickIndependently, HoldAndTap);
 *  - the primary lifting first does not take the secondary finger's click (PrimaryLiftsFirst);
 *  - a secondary finger that drags a draggable element is captured by it: drag events, and the
 *    mousemove for the finger outside the element, go to that element; there is no click at the
 *    end (SecondaryDragCaptured);
 *  - a cancelled secondary finger clicks nothing, sends its pressed element exactly one mouseup, and
 *    drops :active (CancelSecondary);
 *  - a secondary finger's :active survives the primary's reset of its own chain
 *    (ActiveSurvivesPrimaryMove);
 *  - a primary whose scroll container was removed while it was down is a primary again
 *    (PrimaryRecoversAfterScrollerRemoval);
 *  - a secondary finger's id that starts again after the primary lifted, with no End in between,
 *    is an ordinary press and release (SecondaryIdRestartedAfterPrimaryLift);
 *  - a cancelled finger that was dragging ends the drag once and sends its pressed element one
 *    mouseup, with no click (CancelWhileDragging);
 *  - a document closed under a held secondary finger gets nothing from that finger, and the finger
 *    id is free again on the next document (UnloadDocumentDropsSecondary);
 *  - a wobble inside a non-draggable button still clicks on release over it (SecondaryWobblyTapClicks);
 *  - a plain drag (drag: drag) sends its source dragstart, drag and dragend, and nothing to a drop target under
 *    the finger, for the primary finger and for a secondary one (DragEventsPrimary, DragEventsSecondary);
 *  - a drag-drop source (drag: drag-drop) sends its drop target dragover, dragout and dragdrop before its dragend, for
 *    the primary finger (DragDropEventsPrimary) and for a secondary one (SecondaryDropTarget, VaCuus patch #19), and a
 *    secondary plain drag still sends the drop target nothing (SecondaryDropTarget's second part);
 *  - the merged trace of #k and #zone keeps a drop's order: the target's dragdrop and dragout come before the
 *    source's dragend (DragDropEventsPrimary, SecondaryDropTarget);
 *  - a cancelled finger over a drop target gets the primary's cancel: dragdrop and dragout, then the source's dragend
 *    (PrimaryCancelOverDropTarget, SecondaryCancelOverDropTarget).
 *
 * The probe state is read the way the DOM suite reads it: JS through globalThis, and :active
 * through Element::IsPseudoClassSet on the UI thread. The facade exposes neither layout nor
 * pseudo-classes, so there is no offsetWidth to read; the fixture's :active width rule is
 * therefore not measured, only the pseudo-class.
 */

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VaCuusJsDomTestRig.h"

#include "RmlUi/Core/Event.h"
#include "RmlUi/Core/EventListener.h"

#include "VaCuusContentPaths.h"
#include "VaCuusInputEvent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchTwoButtonsTest, "VaCuus.Input.MultiTouch.TwoButtonsClickIndependently",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchHoldAndTapTest, "VaCuus.Input.MultiTouch.HoldAndTap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchSecondaryDragTest, "VaCuus.Input.MultiTouch.SecondaryDragCaptured",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchPrimaryLiftsFirstTest, "VaCuus.Input.MultiTouch.PrimaryLiftsFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchCancelSecondaryTest, "VaCuus.Input.MultiTouch.CancelSecondary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchActiveSurvivesPrimaryMoveTest, "VaCuus.Input.MultiTouch.ActiveSurvivesPrimaryMove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchPrimaryRecoversAfterScrollerRemovalTest,
	"VaCuus.Input.MultiTouch.PrimaryRecoversAfterScrollerRemoval", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchRestartedSecondaryTest, "VaCuus.Input.MultiTouch.SecondaryIdRestartedAfterPrimaryLift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchCancelWhileDraggingTest, "VaCuus.Input.MultiTouch.CancelWhileDragging",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchUnloadDocumentTest, "VaCuus.Input.MultiTouch.UnloadDocumentDropsSecondary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchWobblyTapTest, "VaCuus.Input.MultiTouch.SecondaryWobblyTapClicks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchDragEventsPrimaryTest, "VaCuus.Input.MultiTouch.DragEventsPrimary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchDragEventsSecondaryTest, "VaCuus.Input.MultiTouch.DragEventsSecondary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchDragDropEventsPrimaryTest, "VaCuus.Input.MultiTouch.DragDropEventsPrimary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchSecondaryDropTargetTest, "VaCuus.Input.MultiTouch.SecondaryDropTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchPrimaryCancelOverDropTargetTest, "VaCuus.Input.MultiTouch.PrimaryCancelOverDropTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMultiTouchSecondaryCancelOverDropTargetTest, "VaCuus.Input.MultiTouch.SecondaryCancelOverDropTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMouseTouchActiveTest, "VaCuus.Input.MultiTouch.MouseTouchSharedActive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMouseDragTouchTest, "VaCuus.Input.MultiTouch.MouseDragSurvivesTouch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VaCuusMultiTouchTest
{
using namespace VaCuusJsDomTest;

/** VFS path of the fixture. Tests/ is a folder under the DevUI root, as idle_gate_reload.rml is. */
static const TCHAR* GFixturePath = TEXT("Tests/multitouch.rml");

/** Pointer positions in the fixture's 800x480 view; the fixture's header comment gives the boxes. */
static const FIntPoint GPressA(80, 60);
static const FIntPoint GPressB(80, 320);
static const FIntPoint GPressJ(360, 60);
/** 80 px past the right edge of #j (x = 420). */
static const FIntPoint GMoveBeyondJ(500, 60);
/** Centre of #zone, the drop target: 560..720 x 300..420, clear of every other box. */
static const FIntPoint GZoneCentre(640, 360);
/** Centre of #k, the drag-drop source: 300..420 x 140..220. */
static const FIntPoint GPressK(360, 180);
/** Over no box but the panel, between two crossings of the zone. */
static const FIntPoint GLeaveZone(700, 460);

/** Both click counters as one string, "a,b". */
static const char* GClicksAB = "(globalThis.clicksA || 0) + ',' + (globalThis.clicksB || 0)";

/** The production file-load shape, the same as VaCuusJsDragDropTest.cpp's probe host. */
class FMultiTouchProbeHost final : public FVaCuusTestDocumentHost
{
public:
	explicit FMultiTouchProbeHost(const TCHAR* InContextPrefix)
		: FVaCuusTestDocumentHost(InContextPrefix, "vacuus://multitouch_test.rml", Rml::FocusFlag::Document)
	{
	}

	/** Through Rml::GetFileInterface(), so the fixture's VFS path resolves against the DevUI roots. */
	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (Context == nullptr)
		{
			ReportLoadResult(LoadSerial, /*bSuccess=*/false);
			return;
		}
		AdoptDocument(Context->LoadDocument(Rml::String(TCHAR_TO_UTF8(*VfsPath))), LoadSerial);
	}

	virtual void OnDocumentAdopted() override
	{
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentReady(ViewId, RmlDocument);
		}
	}

	virtual void OnDocumentClosing() override
	{
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentClosing(ViewId);
		}
	}

	virtual void SetVisible(bool /*bVisible*/) override {}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());
		Context->Update();
		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}
};

/**
 * Counts the events one element receives, from the C++ side. A JS handler cannot show that for a closed document,
 * so the close test reads the element's events through this instead.
 */
class FCountingListener final : public Rml::EventListener
{
public:
	virtual void ProcessEvent(Rml::Event& /*Event*/) override
	{
		++Count;
	}

	int32 Count = 0;
};

/**
 * Records what one element receives, in order: the event type, mouse_x and mouse_y, then touch_id and the drag source
 * when the event carries them. JS cannot read the drag source (VaCuusJsEvents.cpp:44-49), so it is read here.
 */
class FTraceListener final : public Rml::EventListener
{
public:
	virtual void ProcessEvent(Rml::Event& Event) override
	{
		FString Line = FString::Printf(TEXT("%s %d,%d"), UTF8_TO_TCHAR(Event.GetType().c_str()),
			Event.GetParameter<int>("mouse_x", -1), Event.GetParameter<int>("mouse_y", -1));
		if (Event.GetParameters().count(Rml::String("touch_id")) > 0)
		{
			Line += FString::Printf(TEXT(" touch %llu"), Event.GetParameter<uint64_t>("touch_id", 0));
		}
		Rml::Element* Source = static_cast<Rml::Element*>(Event.GetParameter<void*>("drag_element", nullptr));
		if (Source != nullptr)
		{
			Line += FString::Printf(TEXT(" source %s"), UTF8_TO_TCHAR(Source->GetId().c_str()));
		}
		Lines.Add(Line);
		if (MergedSink != nullptr)
		{
			MergedSink->Add(FString::Printf(TEXT("%s:%s"), SinkTag, *Line));
		}
	}

	/** Also appends each line to OutMerged, prefixed with InTag, so two listeners' lines keep the order they arrived in. */
	void ShareOrder(TArray<FString>& OutMerged, const TCHAR* InTag)
	{
		MergedSink = &OutMerged;
		SinkTag = InTag;
	}

	TArray<FString> Lines;

private:
	TArray<FString>* MergedSink = nullptr;
	const TCHAR* SinkTag = TEXT("");
};

enum class EOpen
{
	Ready,
	Skipped,
	Failed
};

/**
 * One view on the fixture, loaded through the production LoadDocumentFile command, plus the
 * input helpers. Test-thread object: the closures it runs go through Rig.RunOnUI.
 */
struct FMultiTouchScene
{
	FDomTestRig Rig;
	FMultiTouchProbeHost* Probe = nullptr;
	uint32 ViewId = 0;

	/** Boots the UI thread, loads the fixture and checks the committed boxes. */
	EOpen Open(FAutomationTestBase& Test)
	{
		if (VaCuusContentPaths::ResolveExistingDocument(GFixturePath).IsEmpty())
		{
			Test.AddInfo(TEXT("Skipped: multitouch.rml does not resolve through the DevUI roots"));
			return EOpen::Skipped;
		}

		const FDomTestRig::EBoot Boot = Rig.Boot(Test);
		if (Boot == FDomTestRig::EBoot::Skip)
		{
			return EOpen::Skipped;
		}
		if (Boot == FDomTestRig::EBoot::Fail)
		{
			return EOpen::Failed;
		}

		TUniquePtr<FMultiTouchProbeHost> Owned = MakeUnique<FMultiTouchProbeHost>(TEXT("vacuus_multitouch"));
		Probe = Owned.Get();
		ViewId = Rig.Thread->AllocateViewId();
		Rig.Thread->EnqueueAddView(ViewId, MoveTemp(Owned), FIntPoint(800, 480), MakeShared<FVaCuusViewStatus>());
		Rig.Thread->EnqueueLoadDocumentFile(ViewId, GFixturePath, /*LoadSerial=*/1);
		if (!PumpRealFrames(*Rig.Thread, 2))
		{
			Test.AddError(TEXT("UI frames did not run"));
			return EOpen::Failed;
		}
		const FString Loaded = Eval("String(globalThis.multitouchReady === true && document.getElementById('j') !== null)");
		if (Loaded != TEXT("true"))
		{
			bool bHasDocument = false;
			Rig.RunOnUI([this, &bHasDocument]() { bHasDocument = Probe->GetDocument() != nullptr; });
			Test.AddError(FString::Printf(TEXT("multitouch.rml did not load its elements: JS said '%s', RmlUi document %s"),
				*Loaded, bHasDocument ? TEXT("exists") : TEXT("is missing")));
			return EOpen::Failed;
		}

		// THE COORDINATE SELF-CHECK: the presses above are the fixture's absolute boxes. A re-layout
		// fails here, by name, instead of as a click that silently lands on the wrong element.
		FString Layout;
		Rig.RunOnUI([this, &Layout]()
			{
				Rml::ElementDocument* Document = Probe->GetDocument();
				for (const char* Id : {"a", "j", "b", "k", "zone"})
				{
					Rml::Element* Element = Document != nullptr ? Document->GetElementById(Id) : nullptr;
					if (Element == nullptr)
					{
						Layout += FString::Printf(TEXT("%s:missing "), UTF8_TO_TCHAR(Id));
						continue;
					}
					const Rml::Vector2f Offset = Element->GetAbsoluteOffset(Rml::BoxArea::Border);
					Layout += FString::Printf(TEXT("%s@%d,%d "), UTF8_TO_TCHAR(Id), static_cast<int32>(Offset.x),
						static_cast<int32>(Offset.y));
				}
			});
		if (Layout != TEXT("a@20,20 j@300,20 b@20,280 k@300,140 zone@560,300 "))
		{
			Test.AddError(FString::Printf(TEXT("the committed layout moved: got '%s'"), *Layout));
			return EOpen::Failed;
		}
		return EOpen::Ready;
	}

	static uint64 Id(uint32 Pointer)
	{
		return FVaCuusInputEvent::MakeTouchId(0, Pointer);
	}

	void Down(uint32 Pointer, FIntPoint Position)
	{
		Rig.Thread->EnqueueInput(ViewId,
			FVaCuusInputEvent::Touch(EVaCuusInputEventKind::TouchStart, Id(Pointer), Position, FVaCuusModifierState()));
	}

	void Move(uint32 Pointer, FIntPoint Position)
	{
		Rig.Thread->EnqueueInput(ViewId,
			FVaCuusInputEvent::Touch(EVaCuusInputEventKind::TouchMove, Id(Pointer), Position, FVaCuusModifierState()));
	}

	void Up(uint32 Pointer, FIntPoint Position)
	{
		Rig.Thread->EnqueueInput(ViewId,
			FVaCuusInputEvent::Touch(EVaCuusInputEventKind::TouchEnd, Id(Pointer), Position, FVaCuusModifierState()));
	}

	void Cancel(uint32 Pointer)
	{
		Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::TouchCancel(Id(Pointer)));
	}

	void Mouse(bool bDown, FIntPoint Position)
	{
		Rig.Thread->EnqueueInput(ViewId,
			FVaCuusInputEvent::MouseButton(bDown, Position, EKeys::LeftMouseButton, FVaCuusModifierState()));
	}

	void MouseMove(FIntPoint Position)
	{
		Rig.Thread->EnqueueInput(ViewId, FVaCuusInputEvent::MouseMove(Position, FVaCuusModifierState()));
	}

	/** Runs Source on the view's context; the commands enqueued before it are applied first. */
	FString Eval(const char* Source)
	{
		return Rig.Eval(ViewId, Source);
	}

	/**
	 * Detaches #scroll, the primary's scroll container, while the primary is still down. Destroying the returned
	 * ElementPtr is the detach that Context::OnElementDetach has to survive.
	 */
	void RemoveScroller()
	{
		Rig.RunOnUI([this]()
			{
				Rml::ElementDocument* Document = Probe->GetDocument();
				Rml::Element* Scroller = Document != nullptr ? Document->GetElementById("scroll") : nullptr;
				if (Scroller != nullptr && Scroller->GetParentNode() != nullptr)
				{
					Rml::ElementPtr Removed = Scroller->GetParentNode()->RemoveChild(Scroller);
				}
			});
	}

	/** Adds Listener to the element with this id, for the events a finger sends an element. Returns false if it is missing. */
	bool AttachListener(const char* ElementId, Rml::EventListener* Listener)
	{
		bool bAttached = false;
		Rig.RunOnUI([this, ElementId, Listener, &bAttached]()
			{
				Rml::ElementDocument* Document = Probe->GetDocument();
				Rml::Element* Element = Document != nullptr ? Document->GetElementById(ElementId) : nullptr;
				if (Element == nullptr)
				{
					return;
				}
				for (const Rml::EventId Id : {Rml::EventId::Mousemove, Rml::EventId::Mouseup, Rml::EventId::Click,
						 Rml::EventId::Dragstart, Rml::EventId::Drag, Rml::EventId::Dragend,
						 Rml::EventId::Mousedown, Rml::EventId::Dragover, Rml::EventId::Dragout, Rml::EventId::Dragdrop,
						 Rml::EventId::Dragmove})
				{
					Element->AddEventListener(Id, Listener);
				}
				bAttached = true;
			});
		return bAttached;
	}

	void ResetListener(FCountingListener& Listener)
	{
		Rig.RunOnUI([&Listener]() { Listener.Count = 0; });
	}

	int32 ListenerEvents(const FCountingListener& Listener)
	{
		int32 Count = -1;
		Rig.RunOnUI([&Listener, &Count]() { Count = Listener.Count; });
		return Count;
	}

	/**
	 * The last Count lines (all of them by default), joined with '|' in the order they arrived. Read after an Eval, so the
	 * input is applied.
	 */
	FString Join(const TArray<FString>& Lines, int32 Count = MAX_int32)
	{
		FString Result;
		Rig.RunOnUI([&Lines, &Result, Count]()
			{
				for (int32 Index = FMath::Max(0, Lines.Num() - Count); Index < Lines.Num(); ++Index)
				{
					if (!Result.IsEmpty())
					{
						Result += TEXT("|");
					}
					Result += Lines[Index];
				}
			});
		return Result;
	}

	/** The events a listener recorded, joined with '|' in the order they arrived. */
	FString Trace(const FTraceListener& Listener)
	{
		return Join(Listener.Lines);
	}

	/** Closes the view's document through the production close command, while the fingers are still down. */
	void CloseDocument()
	{
		Rig.Thread->EnqueueCloseDocument(ViewId);
	}

	/** Loads the fixture again into the view, which replaces the closed document. */
	bool Reload(FAutomationTestBase& Test)
	{
		Rig.Thread->EnqueueLoadDocumentFile(ViewId, GFixturePath, /*LoadSerial=*/2);
		if (!PumpRealFrames(*Rig.Thread, 2))
		{
			Test.AddError(TEXT("UI frames did not run after the reload"));
			return false;
		}
		return true;
	}

	/** Element::IsPseudoClassSet("active") on the element with this id, as "true" or "false". */
	FString IsActive(const char* ElementId)
	{
		FString Result(TEXT("<ui-timeout>"));
		Rig.RunOnUI([this, &Result, ElementId]()
			{
				Rml::ElementDocument* Document = Probe->GetDocument();
				Rml::Element* Element = Document != nullptr ? Document->GetElementById(ElementId) : nullptr;
				if (Element == nullptr)
				{
					Result = TEXT("<missing>");
					return;
				}
				Result = Element->IsPseudoClassSet("active") ? TEXT("true") : TEXT("false");
			});
		return Result;
	}
};
}	 // namespace VaCuusMultiTouchTest

bool FVaCuusMultiTouchTwoButtonsTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}

	Scene.Down(0, GPressA);
	Scene.Down(1, GPressB);
	Scene.Up(1, GPressB);
	Scene.Up(0, GPressA);

	TestEqual(TEXT("each finger clicks the button it pressed"), Scene.Eval(GClicksAB), FString(TEXT("1,1")));
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchHoldAndTapTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}

	Scene.Down(0, GPressA);
	Scene.Down(1, GPressB);
	Scene.Up(1, GPressB);
	Scene.Down(2, GPressB);
	Scene.Up(2, GPressB);

	TestEqual(TEXT("two taps on B while A is held: B clicked twice, A not at all"), Scene.Eval(GClicksAB),
		FString(TEXT("0,2")));

	Scene.Up(0, GPressA);
	TestEqual(TEXT("releasing A clicks A once and leaves B's count alone"), Scene.Eval(GClicksAB), FString(TEXT("1,2")));
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchSecondaryDragTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}

	Scene.Down(0, GPressA);
	Scene.Down(1, GPressJ);
	// The press must not count: only the move's mousemove is under test.
	Scene.Eval("globalThis.moveOnJ = 0; 'ok'");
	Scene.Move(1, GMoveBeyondJ);

	TestEqual(TEXT("the move past the click radius starts the drag: dragstart, then drag"),
		Scene.Eval("globalThis.dragEvents.join('|')"), FString(TEXT("dragstart|drag")));
	TestEqual(TEXT("the drag is captured: its last event sits outside the element that was pressed"),
		Scene.Eval("String(globalThis.lastDragX)"), FString(TEXT("500")));
	TestEqual(TEXT("the drag events carry the secondary finger's touch_id"), Scene.Eval("String(globalThis.lastDragTouch)"),
		FString(TEXT("1")));
	TestEqual(TEXT("the mousemove for the finger outside J went to J"), Scene.Eval("String(globalThis.moveOnJ || 0)"),
		FString(TEXT("1")));

	Scene.Up(1, GMoveBeyondJ);
	TestEqual(TEXT("the release ends the drag and clicks nothing"),
		Scene.Eval("globalThis.dragEvents.join('|') + '~' + (globalThis.clicksJ || 0)"),
		FString(TEXT("dragstart|drag|dragend~0")));
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchPrimaryLiftsFirstTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}

	Scene.Down(0, GPressA);
	Scene.Down(1, GPressB);
	Scene.Up(0, GPressA);
	TestEqual(TEXT("the primary lifts first: its own click lands, the secondary has not clicked"),
		Scene.Eval(GClicksAB), FString(TEXT("1,0")));

	Scene.Up(1, GPressB);
	TestEqual(TEXT("the secondary then clicks its own button"), Scene.Eval(GClicksAB), FString(TEXT("1,1")));
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchCancelSecondaryTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}

	Scene.Down(0, GPressA);
	Scene.Down(1, GPressB);
	Scene.Cancel(1);

	TestEqual(TEXT("a cancelled secondary finger clicks nothing"), Scene.Eval("String(globalThis.clicksB || 0)"),
		FString(TEXT("0")));
	TestEqual(TEXT("a cancelled secondary finger ends its press with one mouseup on its button"),
		Scene.Eval("String(globalThis.upsB || 0)"), FString(TEXT("1")));
	TestEqual(TEXT("a cancelled secondary finger drops :active from its button"), Scene.IsActive("b"), FString(TEXT("false")));

	Scene.Up(0, GPressA);
	TestEqual(TEXT("the primary is unaffected by the cancel and still clicks"), Scene.Eval(GClicksAB),
		FString(TEXT("1,0")));
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchPrimaryRecoversAfterScrollerRemovalTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}

	Scene.Down(0, GPressA);
	Scene.Eval("globalThis.moveOnJ = 0; 'ok'");
	Scene.RemoveScroller();
	Scene.Up(0, GPressA);

	// A primary press also sends mousemove to the element under it; a secondary press does not. Only a
	// primary can move the hover to J here.
	Scene.Down(2, GPressJ);
	TestEqual(TEXT("after the scroller is gone the next finger is a primary: its press moves the hover to J"),
		Scene.Eval("String(globalThis.moveOnJ || 0)"), FString(TEXT("1")));

	Scene.Up(2, GPressJ);
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchRestartedSecondaryTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}

	Scene.Down(0, GPressA);
	Scene.Down(1, GPressB);
	Scene.Up(0, GPressA);
	// Finger 1 starts again with no End in between. The start has to end its old secondary press first.
	Scene.Down(1, GPressB);
	Scene.Up(1, GPressB);

	TestEqual(TEXT("the restarted finger is an ordinary press and release: nothing stays :active on its button"),
		Scene.IsActive("b"), FString(TEXT("false")));
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchCancelWhileDraggingTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}

	Scene.Down(0, GPressA);
	Scene.Down(1, GPressJ);
	Scene.Eval("globalThis.upsJ = 0; globalThis.dragEvents = []; globalThis.seqJ = []; 'ok'");
	Scene.Move(1, GMoveBeyondJ);
	Scene.Cancel(1);

	TestEqual(TEXT("the cancel ends the drag exactly once: dragstart, drag, dragend"),
		Scene.Eval("(globalThis.dragEvents || []).join('|')"), FString(TEXT("dragstart|drag|dragend")));
	TestEqual(TEXT("the cancel sends J one mouseup"), Scene.Eval("String(globalThis.upsJ || 0)"), FString(TEXT("1")));
	TestEqual(TEXT("the cancel ends the drag before it releases the press: dragend, then mouseup"),
		Scene.Eval("(globalThis.seqJ || []).join('|')"), FString(TEXT("dragend|mouseup")));
	TestEqual(TEXT("a cancelled drag clicks nothing"), Scene.Eval("String(globalThis.clicksJ || 0)"), FString(TEXT("0")));
	TestEqual(TEXT("J is not :active after the cancel"), Scene.IsActive("j"), FString(TEXT("false")));

	Scene.Up(0, GPressA);
	TestEqual(TEXT("the primary is unaffected by the cancel and still clicks"), Scene.Eval(GClicksAB), FString(TEXT("1,0")));
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchUnloadDocumentTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	// Declared before the scene, so the element that holds a pointer to it is torn down before it dies.
	FCountingListener Listener;
	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}

	Scene.Down(0, GPressA);
	Scene.Down(1, GPressJ);
	TestTrue(TEXT("#j is in the document before the close"), Scene.AttachListener("j", &Listener));

	// The document goes away while the second finger is still down on its element. The element outlives the close
	// until its release, and a finger that still reaches it would send it events.
	//
	// This test relies on the test host's HasView() going false after the close. The frame loop then skips
	// RecordAndPublishFrame, and with it Context::Update, so the closed document stays unreleased: the limbo window
	// UnloadDocument's secondary-finger reset guards. If the host ever updates after the close, this test stops
	// covering that reset.
	Scene.CloseDocument();
	Scene.Eval("'closed'");
	Scene.ResetListener(Listener);
	Scene.Move(1, GMoveBeyondJ);
	Scene.Up(1, GMoveBeyondJ);
	Scene.Eval("'released'");

	TestEqual(TEXT("after the close the finger sends the closed document's #j no event (mousemove, drag, mouseup, click)"),
		Scene.ListenerEvents(Listener), int32(0));

	// Finger 2 presses first on the reloaded document, so it becomes that document's primary. The old document's scroller
	// took the primary's TouchState with it when the old document was released, so without this press the next finger
	// would become the primary. Finger 1 then presses B after it, so it is a true secondary, and its id is free again.
	if (!Scene.Reload(*this))
	{
		return false;
	}
	Scene.Down(2, GPressA);
	Scene.Down(1, GPressB);
	Scene.Up(1, GPressB);
	TestEqual(TEXT("the same finger id, as a secondary on the reloaded document, clicks B once"),
		Scene.Eval("String(globalThis.clicksB || 0)"), FString(TEXT("1")));

	Scene.Up(2, GPressA);
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchWobblyTapTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}

	Scene.Down(0, GPressA);
	Scene.Down(1, GPressB);
	// Ten pixels: past the 3 dp click radius, still inside B. B is not draggable, so the radius does not apply to it.
	Scene.Move(1, FIntPoint(90, 320));
	Scene.Up(1, FIntPoint(90, 320));

	TestEqual(TEXT("a wobbly tap on a non-draggable button clicks on a release over it"),
		Scene.Eval("String(globalThis.clicksB || 0)"), FString(TEXT("1")));

	Scene.Up(0, GPressA);
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchActiveSurvivesPrimaryMoveTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}

	Scene.Down(0, GPressA);
	Scene.Down(1, GPressB);
	// 30 px: past the click radius, so the primary resets its :active chain (Context.cpp ProcessTouchMove).
	Scene.Move(0, FIntPoint(110, 60));

	TestEqual(TEXT("the secondary finger keeps :active on its button"), Scene.IsActive("b"), FString(TEXT("true")));
	TestEqual(TEXT("...and on the ancestor it shares with the primary, which the reset would otherwise clear"),
		Scene.IsActive("panel"), FString(TEXT("true")));
	TestEqual(TEXT("the primary's reset did run: its own element lost :active"), Scene.IsActive("a"),
		FString(TEXT("false")));
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchDragEventsPrimaryTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	// Declared before the scene, so the elements that hold pointers to them are torn down first.
	FTraceListener JTrace;
	FTraceListener ZoneTrace;
	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}
	TestTrue(TEXT("#j is in the document before the drag"), Scene.AttachListener("j", &JTrace));
	TestTrue(TEXT("#zone is in the document before the drag"), Scene.AttachListener("zone", &ZoneTrace));

	// One finger presses J, leaves it, crosses the zone and lifts over it.
	Scene.Down(0, GPressJ);
	Scene.Move(0, GMoveBeyondJ);
	Scene.Move(0, GZoneCentre);
	Scene.Up(0, GZoneCentre);
	Scene.Eval("'released'");

	const FString JSequence = Scene.Trace(JTrace);
	AddInfo(FString::Printf(TEXT("primary #j: %s"), *JSequence));
	AddInfo(FString::Printf(TEXT("primary #zone: %s"), *Scene.Trace(ZoneTrace)));
	TestEqual(TEXT("#j, primary: the press, the first move starts the drag, each move drags, the lift ends it; touch_id 0"),
		JSequence,
		FString(TEXT("mousemove 360,60 touch 0|mousedown 360,60 touch 0|dragstart 360,60 touch 0 source j|drag 500,60 touch 0 source j|drag 640,360 touch 0 source j|dragend 640,360 touch 0 source j")));
	// The zone is the hover element under the finger, so it gets the hover mousemove and the release's mouseup. A plain
	// drag still sends it none of the drop-target events.
	TestEqual(TEXT("#zone, primary: the hover events reach it, but a plain drag sends no dragover, dragout or dragdrop"),
		Scene.Trace(ZoneTrace), FString(TEXT("mousemove 640,360 touch 0|mouseup 640,360 touch 0")));
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchDragEventsSecondaryTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	FTraceListener JTrace;
	FTraceListener ZoneTrace;
	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}
	TestTrue(TEXT("#j is in the document before the drag"), Scene.AttachListener("j", &JTrace));
	TestTrue(TEXT("#zone is in the document before the drag"), Scene.AttachListener("zone", &ZoneTrace));

	// The primary holds A, so the press on J is a secondary finger, with touch_id 1. Same path as the primary test.
	Scene.Down(0, GPressA);
	Scene.Down(1, GPressJ);
	Scene.Move(1, GMoveBeyondJ);
	Scene.Move(1, GZoneCentre);
	Scene.Up(1, GZoneCentre);
	Scene.Eval("'released'");

	const FString JSequence = Scene.Trace(JTrace);
	AddInfo(FString::Printf(TEXT("secondary #j: %s"), *JSequence));
	AddInfo(FString::Printf(TEXT("secondary #zone: %s"), *Scene.Trace(ZoneTrace)));
	TestEqual(TEXT("#j, secondary: the press has no mousemove; the drag matches the primary's; each captured move also sends a mousemove; mouseup comes before dragend; no click"),
		JSequence,
		FString(TEXT("mousedown 360,60 touch 1|dragstart 360,60 touch 1 source j|drag 500,60 touch 1 source j|mousemove 500,60 touch 1|drag 640,360 touch 1 source j|mousemove 640,360 touch 1|mouseup 640,360 touch 1|dragend 640,360 touch 1 source j")));
	TestEqual(TEXT("#zone, secondary: a plain drag gives the drop target under the finger no dragover, dragout or dragdrop"),
		Scene.Trace(ZoneTrace), FString());

	Scene.Up(0, GPressA);
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchDragDropEventsPrimaryTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	// Merged is declared before the scene: the listeners write to it until the elements holding them are torn down.
	FTraceListener KTrace;
	FTraceListener ZoneTrace;
	TArray<FString> Merged;
	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}
	TestTrue(TEXT("#k is in the document before the drag"), Scene.AttachListener("k", &KTrace));
	TestTrue(TEXT("#zone is in the document before the drag"), Scene.AttachListener("zone", &ZoneTrace));
	KTrace.ShareOrder(Merged, TEXT("k"));
	ZoneTrace.ShareOrder(Merged, TEXT("zone"));

	// One finger presses K, crosses the zone, leaves it, comes back and lifts over it.
	Scene.Down(0, GPressK);
	Scene.Move(0, GZoneCentre);
	Scene.Move(0, GLeaveZone);
	Scene.Move(0, GZoneCentre);
	Scene.Up(0, GZoneCentre);
	Scene.Eval("'released'");

	const FString KSequence = Scene.Trace(KTrace);
	const FString ZoneSequence = Scene.Trace(ZoneTrace);
	AddInfo(FString::Printf(TEXT("primary #k: %s"), *KSequence));
	AddInfo(FString::Printf(TEXT("primary #zone: %s"), *ZoneSequence));
	TestEqual(TEXT("#k, primary: a drag-drop source sends its drag events as a plain drag does, with touch_id 0"), KSequence,
		FString(TEXT("mousemove 360,180 touch 0|mousedown 360,180 touch 0|dragstart 360,180 touch 0 source k|drag 640,360 touch 0 source k|drag 700,460 touch 0 source k|drag 640,360 touch 0 source k|dragend 640,360 touch 0 source k")));
	// The zone is the hover element under the finger, so it also gets mousemove and mouseup.
	TestEqual(TEXT("#zone, primary: dragover on entering, dragout on leaving, and on the lift dragdrop then dragout before the source's dragend"),
		ZoneSequence,
		FString(TEXT("dragover 640,360 touch 0 source k|mousemove 640,360 touch 0|dragmove 640,360 touch 0 source k|dragout 700,460 touch 0 source k|dragover 640,360 touch 0 source k|mousemove 640,360 touch 0|dragmove 640,360 touch 0 source k|mouseup 640,360 touch 0|dragdrop 640,360 touch 0 source k|dragout 640,360 touch 0 source k")));
	TestEqual(TEXT("merged order: the zone's mouseup, dragdrop and dragout come before #k's dragend"), Scene.Join(Merged, 4),
		FString(TEXT("zone:mouseup 640,360 touch 0|zone:dragdrop 640,360 touch 0 source k|zone:dragout 640,360 touch 0 source k|k:dragend 640,360 touch 0 source k")));
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchSecondaryDropTargetTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	// Merged is declared before the scene: the listeners write to it until the elements holding them are torn down.
	FTraceListener KTrace;
	FTraceListener ZoneTrace;
	TArray<FString> Merged;
	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}
	TestTrue(TEXT("#k is in the document before the drag"), Scene.AttachListener("k", &KTrace));
	TestTrue(TEXT("#zone is in the document before the drag"), Scene.AttachListener("zone", &ZoneTrace));
	KTrace.ShareOrder(Merged, TEXT("k"));
	ZoneTrace.ShareOrder(Merged, TEXT("zone"));

	// The primary holds A. A secondary finger (touch_id 1) presses K, crosses the zone, leaves it, comes back and lifts over it.
	Scene.Down(0, GPressA);
	Scene.Down(1, GPressK);
	Scene.Move(1, GZoneCentre);
	Scene.Move(1, GLeaveZone);
	Scene.Move(1, GZoneCentre);
	Scene.Up(1, GZoneCentre);
	Scene.Eval("'released'");

	const FString KSequence = Scene.Trace(KTrace);
	AddInfo(FString::Printf(TEXT("secondary #k: %s"), *KSequence));
	TestEqual(TEXT("#k, secondary: its drag events carry touch_id 1; each captured move also sends a mousemove; the release ends with mouseup, then dragend"),
		KSequence,
		FString(TEXT("mousedown 360,180 touch 1|dragstart 360,180 touch 1 source k|drag 640,360 touch 1 source k|mousemove 640,360 touch 1|drag 700,460 touch 1 source k|mousemove 700,460 touch 1|drag 640,360 touch 1 source k|mousemove 640,360 touch 1|mouseup 640,360 touch 1|dragend 640,360 touch 1 source k")));

	// Secondary has no hover, so the zone gets only the drop-target events. The same events as the primary, minus its hover.
	const FString ZoneSequence = Scene.Trace(ZoneTrace);
	AddInfo(FString::Printf(TEXT("secondary #zone: %s"), *ZoneSequence));
	TestEqual(TEXT("#zone, secondary: dragover on entering, dragout on leaving, and on the lift dragdrop then dragout, with touch_id 1"),
		ZoneSequence,
		FString(TEXT("dragover 640,360 touch 1 source k|dragmove 640,360 touch 1 source k|dragout 700,460 touch 1 source k|dragover 640,360 touch 1 source k|dragmove 640,360 touch 1 source k|dragdrop 640,360 touch 1 source k|dragout 640,360 touch 1 source k")));

	TestEqual(TEXT("merged order, secondary: the release sends #k's mouseup, then the zone's dragdrop and dragout, then #k's dragend"),
		Scene.Join(Merged, 4),
		FString(TEXT("k:mouseup 640,360 touch 1|zone:dragdrop 640,360 touch 1 source k|zone:dragout 640,360 touch 1 source k|k:dragend 640,360 touch 1 source k")));

	// The gate: a secondary plain drag (drag: drag, #j) over the same zone sends it nothing.
	Scene.Down(2, GPressJ);
	Scene.Move(2, GMoveBeyondJ);
	Scene.Move(2, GZoneCentre);
	Scene.Up(2, GZoneCentre);
	Scene.Eval("'released'");
	TestEqual(TEXT("#zone: a secondary plain drag (drag: drag) over the zone adds no drop event"), Scene.Trace(ZoneTrace), ZoneSequence);

	Scene.Up(0, GPressA);
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchPrimaryCancelOverDropTargetTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	// Merged is declared before the scene: the listeners write to it until the elements holding them are torn down.
	FTraceListener KTrace;
	FTraceListener ZoneTrace;
	TArray<FString> Merged;
	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}
	TestTrue(TEXT("#k is in the document before the drag"), Scene.AttachListener("k", &KTrace));
	TestTrue(TEXT("#zone is in the document before the drag"), Scene.AttachListener("zone", &ZoneTrace));
	KTrace.ShareOrder(Merged, TEXT("k"));
	ZoneTrace.ShareOrder(Merged, TEXT("zone"));

	// The primary presses K, crosses the zone and is cancelled over it. The cancel takes the release's path (ProcessTouchCancel).
	Scene.Down(0, GPressK);
	Scene.Move(0, GZoneCentre);
	Scene.Cancel(0);
	Scene.Eval("'cancelled'");

	TestEqual(TEXT("#zone, primary cancel: the hover mouseup, then dragdrop and dragout, as on a release"), Scene.Trace(ZoneTrace),
		FString(TEXT("dragover 640,360 touch 0 source k|mousemove 640,360 touch 0|dragmove 640,360 touch 0 source k|mouseup 640,360 touch 0|dragdrop 640,360 touch 0 source k|dragout 640,360 touch 0 source k")));
	TestEqual(TEXT("merged order, primary cancel: the zone's dragdrop and dragout come before #k's dragend"), Scene.Join(Merged, 4),
		FString(TEXT("zone:mouseup 640,360 touch 0|zone:dragdrop 640,360 touch 0 source k|zone:dragout 640,360 touch 0 source k|k:dragend 640,360 touch 0 source k")));
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMultiTouchSecondaryCancelOverDropTargetTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;

	// Merged is declared before the scene: the listeners write to it until the elements holding them are torn down.
	FTraceListener KTrace;
	FTraceListener ZoneTrace;
	TArray<FString> Merged;
	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}
	TestTrue(TEXT("#k is in the document before the drag"), Scene.AttachListener("k", &KTrace));
	TestTrue(TEXT("#zone is in the document before the drag"), Scene.AttachListener("zone", &ZoneTrace));
	KTrace.ShareOrder(Merged, TEXT("k"));
	ZoneTrace.ShareOrder(Merged, TEXT("zone"));

	// The primary holds A. A secondary finger (touch_id 1) presses K, crosses the zone and is cancelled over it.
	Scene.Down(0, GPressA);
	Scene.Down(1, GPressK);
	Scene.Move(1, GZoneCentre);
	Scene.Cancel(1);
	Scene.Eval("'cancelled'");

	TestEqual(TEXT("#zone, secondary cancel: dragover and dragmove on entering, then dragdrop and dragout; no mouseup, as the secondary has no hover"),
		Scene.Trace(ZoneTrace),
		FString(TEXT("dragover 640,360 touch 1 source k|dragmove 640,360 touch 1 source k|dragdrop 640,360 touch 1 source k|dragout 640,360 touch 1 source k")));
	TestEqual(TEXT("#k, secondary cancel: the drag ends, then the pressed element gets the mouseup, and no click"), Scene.Trace(KTrace),
		FString(TEXT("mousedown 360,180 touch 1|dragstart 360,180 touch 1 source k|drag 640,360 touch 1 source k|mousemove 640,360 touch 1|dragend 640,360 touch 1 source k|mouseup 640,360 touch 1")));
	TestEqual(TEXT("merged order, secondary cancel: the zone's dragdrop and dragout come before #k's dragend, and dragend before mouseup"),
		Scene.Join(Merged, 4),
		FString(TEXT("zone:dragdrop 640,360 touch 1 source k|zone:dragout 640,360 touch 1 source k|k:dragend 640,360 touch 1 source k|k:mouseup 640,360 touch 1")));

	Scene.Up(0, GPressA);
	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

bool FVaCuusMouseTouchActiveTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;
	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}
	Scene.Down(0, GPressB);
	Scene.Mouse(true, GPressB);
	Scene.Mouse(false, GPressB);
	Scene.Eval("'mouse released'");
	TestEqual(TEXT("a finger keeps shared :active after the mouse releases"), Scene.IsActive("b"), FString(TEXT("true")));
	Scene.Up(0, GPressB);
	TestEqual(TEXT("both devices click the shared button"), Scene.Eval("String(globalThis.clicksB)"), FString(TEXT("2")));
	TestEqual(TEXT("both released means inactive"), Scene.IsActive("b"), FString(TEXT("false")));

	Scene.Mouse(true, GPressB);
	Scene.Down(0, GPressB);
	Scene.Up(0, GPressB);
	Scene.Eval("'touch released'");
	TestEqual(TEXT("the mouse keeps shared :active after the finger releases"), Scene.IsActive("b"), FString(TEXT("true")));
	Scene.Mouse(false, GPressB);
	TestEqual(TEXT("both devices click in the reverse order too"), Scene.Eval("String(globalThis.clicksB)"), FString(TEXT("4")));
	TestEqual(TEXT("the last mouse release clears :active"), Scene.IsActive("b"), FString(TEXT("false")));
	return true;
}

bool FVaCuusMouseDragTouchTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusMultiTouchTest;
	FMultiTouchScene Scene;
	if (const EOpen Open = Scene.Open(*this); Open != EOpen::Ready)
	{
		return Open == EOpen::Skipped;
	}
	Scene.Mouse(true, GPressJ);
	Scene.Down(0, GPressA);
	Scene.MouseMove(GMoveBeyondJ);
	TestEqual(TEXT("a mouse drag still starts while the first touch is down"),
		Scene.Eval("globalThis.dragEvents.join('|')"), FString(TEXT("dragstart|drag")));
	Scene.Up(0, GPressA);
	Scene.Mouse(false, GMoveBeyondJ);
	TestEqual(TEXT("the touch release does not finish or replace the mouse drag"),
		Scene.Eval("globalThis.dragEvents.join('|')"), FString(TEXT("dragstart|drag|dragend")));
	TestEqual(TEXT("the touch still clicks its own button"), Scene.Eval(GClicksAB), FString(TEXT("1,0")));

	Scene.Mouse(true, GPressB);
	Scene.Down(0, GPressA);
	Scene.Rig.Thread->EnqueueInput(Scene.ViewId, FVaCuusInputEvent::MouseCancel());
	Scene.Eval("'mouse cancelled'");
	TestEqual(TEXT("mouse cancel clears only the mouse's :active"), Scene.IsActive("b"), FString(TEXT("false")));
	TestEqual(TEXT("mouse cancel keeps the finger's :active"), Scene.IsActive("a"), FString(TEXT("true")));
	Scene.Up(0, GPressA);
	TestEqual(TEXT("mouse cancellation does not click; the finger still does"), Scene.Eval(GClicksAB), FString(TEXT("2,0")));
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
