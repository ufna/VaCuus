// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusUIThread.h"

#include "VaCuusBoundModel.h"
#include "VaCuusDataVariable.h"
#include "VaCuusDefines.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusInputMap.h"
#include "VaCuusStats.h"
#include "VaCuusTextInput.h"
#include "VaCuusUIQueues.h"
#include "VaCuusWriteRouter.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>

// Rml::GetSystemInterface + the complete SystemInterface, the pump's clock source
// (see the JsPump phase in RunFrame); Core.h only forward-declares the class.
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/SystemInterface.h>

// Rml::Factory::ClearStyleSheetCache/ClearTemplateCache, the live-reload path's
// cache drop (see DrainCommands).
#include <RmlUi/Core/Factory.h>

// ElementDocument, complete rather than forward-declared: the Back handler asks
// "is the focused element a document element?" by comparing GetOwnerDocument()
// against the element itself, and that derived-to-base pointer conversion needs the
// full type.
#include <RmlUi/Core/ElementDocument.h>

namespace
{
/**
 * OS id of the live UI thread, or 0 when there is none. Mirrors the engine's own
 * GGameThreadId / GRenderThreadId so IsInUIThread() needs no instance and costs a
 * relaxed load. Deliberately a single global: VaCuus runs at most one UI thread
 * per process (see the class comment for why RmlUi leaves no choice).
 */
std::atomic<uint32> GVaCuusUIThreadId{0};

/**
 * The live script host, or null (M4 Task 6). Backs
 * FVaCuusUIThread::GetActiveScriptHost() -- see its header comment for who
 * reads this and why it is process-wide. A PLAIN pointer, not an atomic, and
 * that is a documented claim rather than an oversight: it is written in Init()
 * and Exit() and read from document-host code running inside the frame loop --
 * all on the UI thread (inline mode included: the scope makes the game thread
 * BE the UI thread for the write and the read alike), and the accessor asserts
 * exactly that.
 */
IVaCuusScriptHost* GVaCuusActiveScriptHost = nullptr;

/**
 * Stack size for the UI thread, chosen rather than inherited: the platform default
 * 8 MB per thread is far more than a UI tree needs, while RmlUi's layout and style
 * resolution recurse with the document tree. 512 KB carried M1-M3; M4 raises it to
 * 2 MB because quickjs now lands on this thread and its interpreter recurses on
 * the NATIVE stack -- the engine's own overflow guard is a stack-POINTER check
 * against an anchor captured at runtime creation (js_check_stack_overflow,
 * quickjs.c:1952-1957), so its 256 KB budget (JS_SetMaxStackSize, set in
 * FVaCuusJsRuntime) must fit inside this thread's real stack UNDER whatever RmlUi
 * frames sit below the script entry point, with headroom for the C++ the deepest
 * JS frame then calls back into. 2 MB = the 256 KB JS budget + the old 512 KB
 * proven UI budget, then doubled-and-rounded for the two stacked together (spec
 * 3.3; the stack-headroom test drives deep JS recursion into RangeError, never the
 * guard page). Unix clamps any non-zero request up to at least 128 KB
 * (UnixPlatformRunnableThread.cpp), so this value survives as given.
 */
constexpr uint32 GVaCuusUIThreadStackSize = 2 * 1024 * 1024;

/**
 * The M4 kill switch: 0 = the UI thread boots WITHOUT a script host even when a
 * factory is registered, so no JS phase ever runs and quickjs is never created.
 * Read ONCE, at thread boot (Init()), which is the moment the host would be
 * created -- the cheap-any-thread read is the vacuus.IdleGate pattern
 * (VaCuusRecordingRenderInterface.cpp:47-52), but unlike the gate a later flip is
 * a documented no-op until the next thread boot: the host either exists for the
 * thread's whole life or never does, because half-created JS state has no safe
 * mid-flight teardown point.
 */
static TAutoConsoleVariable<int32> CVarVaCuusJsEnable(
	TEXT("vacuus.Js.Enable"),
	1,
	TEXT("1 (default) = the UI thread creates the registered script host at boot, enabling JavaScript.\n")
		TEXT("0 = boot without one; no JS runs and quickjs is never initialized. Read once at UI thread boot -- ")
		TEXT("flipping it later does nothing until the thread is restarted."));

/**
 * Makes the calling thread *be* the UI thread for the duration of the scope.
 *
 * Only used by the inline fallback: with no real worker thread, the frame runs on
 * the game thread, and the check(IsInUIThread()) guards protecting RmlUi have to
 * accept it. Scoped rather than latched so that outside these calls the game
 * thread is still not the UI thread and the guards still catch mistakes.
 */
struct FVaCuusInlineUIThreadScope
{
	FVaCuusInlineUIThreadScope()
		: Previous(GVaCuusUIThreadId.exchange(FPlatformTLS::GetCurrentThreadId(), std::memory_order_acq_rel))
	{
	}

	~FVaCuusInlineUIThreadScope() { GVaCuusUIThreadId.store(Previous, std::memory_order_release); }

	uint32 Previous = 0;
};

/**
 * Applies one input event to one context.
 *
 * RETURN VALUES ARE DELIBERATELY IGNORED, all of them, and that is not laziness:
 * RmlUi's are not one concept. ProcessKeyDown/KeyUp/TextInput/MouseWheel return
 * "the event was NOT consumed" (false means an element called StopPropagation),
 * while ProcessMouseMove/ButtonDown/ButtonUp/MouseLeave return
 * `!IsMouseInteracting()` -- a hover/active STATE hint evaluated after dispatch,
 * not consumption (Context.cpp:849). Collapsing the two into a single "handled"
 * bit is how embedders get this wrong. There is also nowhere for the answer to go:
 * Slate was answered synchronously on the game thread from the published snapshot,
 * frames ago in queue terms, and it cannot be un-answered now.
 */
void DispatchInputEvent(Rml::Context& Context, const FVaCuusInputEvent& Event)
{
	const int32 Modifiers = VaCuusInput::ToRmlModifiers(Event.Modifiers);

	switch (Event.Kind)
	{
		case EVaCuusInputEventKind::MouseMove:
			Context.ProcessMouseMove(Event.Position.X, Event.Position.Y, Modifiers);
			break;

		case EVaCuusInputEventKind::MouseDown:
		case EVaCuusInputEventKind::MouseUp:
		{
			const int32 Button = VaCuusInput::ToRmlMouseButton(Event.Key);
			if (Button == INDEX_NONE)
			{
				// Thumb buttons and the like: RmlUi gives no meaning to indices above 2,
				// so this is dropped rather than dispatched under a made-up number.
				UE_LOG(LogVaCuus, Verbose, TEXT("View %u: mouse button '%s' has no RmlUi index; event dropped"),
					Event.ViewId, *Event.Key.ToString());
				break;
			}

			// Position first. RmlUi's button handling resolves against its LAST
			// processed mouse position (there is no position argument), and Slate can
			// deliver a press with no move before it -- a click on a widget that just
			// appeared under the cursor, a window that just took focus, or a
			// synthesized event. Redundant when a move did precede it, and cheap:
			// ProcessMouseMove guards its mousemove dispatch on the position actually
			// having changed (Context.cpp:586-601), so this costs one hit test and
			// emits no spurious events.
			Context.ProcessMouseMove(Event.Position.X, Event.Position.Y, Modifiers);

			if (Event.Kind == EVaCuusInputEventKind::MouseDown)
			{
				Context.ProcessMouseButtonDown(Button, Modifiers);
			}
			else
			{
				Context.ProcessMouseButtonUp(Button, Modifiers);
			}
			break;
		}

		case EVaCuusInputEventKind::MouseWheel:
			// Same reason as above: the scroll target is derived from the hover element,
			// i.e. from the last processed position.
			Context.ProcessMouseMove(Event.Position.X, Event.Position.Y, Modifiers);

			// TWO conversions in one line, both easy to get wrong:
			//
			// SIGN -- RmlUi documents positive Y as DOWN (Context.h:198) while UE's
			// FPointerEvent::GetWheelDelta() is positive for wheel-UP. Hence the negation.
			//
			// UNIT -- RmlUi's 1.0 is UNIT_SCROLL_LENGTH (80.f) * the context's
			// density-independent pixel ratio (Context.cpp:28,827), NOT one pixel. UE's
			// delta is already in notches (1.0 per click), so it maps onto RmlUi's unit
			// 1:1 and needs no scaling -- but passing a pixel delta here would scroll
			// eighty times too far, which is why this is spelled out rather than assumed.
			//
			// The Vector2f overload, never the float one: ProcessMouseWheel(float, int)
			// is the single @deprecated declaration in Context.h (line 195-196).
			Context.ProcessMouseWheel(Rml::Vector2f(0.0f, -Event.WheelDelta), Modifiers);
			break;

		case EVaCuusInputEventKind::MouseLeave:
			// Not optional: without it `mouse_active` stays set, the hover chain is never
			// cleared and `:hover` styling sticks forever (Context.cpp:839-846). The next
			// ProcessMouseMove re-arms the context.
			Context.ProcessMouseLeave();
			break;

		case EVaCuusInputEventKind::KeyDown:
		{
			const Rml::Input::KeyIdentifier KeyId = VaCuusInput::ToRmlKey(Event.Key);
			if (KeyId == Rml::Input::KI_UNKNOWN)
			{
				// Normal for localized punctuation and pseudo-keys: the character still
				// arrives through the TextInput path, which is where it belongs.
				break;
			}

			Context.ProcessKeyDown(KeyId, Modifiers);

			// RmlUi does NOT synthesise a newline text-input from Return, so multiline
			// controls stay empty unless the embedder sends one -- exactly what the SDL
			// backend does (RmlUi_Platform_SDL.cpp:207-210). The `char` overload is safe
			// here and only here: it silently drops any byte above 127 (Context.cpp:553-557)
			// and '\n' is 10.
			if (KeyId == Rml::Input::KI_RETURN || KeyId == Rml::Input::KI_NUMPADENTER)
			{
				Context.ProcessTextInput('\n');
			}
			break;
		}

		case EVaCuusInputEventKind::KeyUp:
		{
			const Rml::Input::KeyIdentifier KeyId = VaCuusInput::ToRmlKey(Event.Key);
			if (KeyId != Rml::Input::KI_UNKNOWN)
			{
				Context.ProcessKeyUp(KeyId, Modifiers);
			}
			break;
		}

		case EVaCuusInputEventKind::TextInput:
			// The Rml::Character (UTF-32) overload. Never ProcessTextInput(char): it
			// returns false without dispatching for every byte above 127, so it would
			// silently swallow all non-ASCII typing.
			Context.ProcessTextInput(Rml::Character(Event.CodePoint));
			break;

		case EVaCuusInputEventKind::NavigateBack:
		{
			// The pad's Back button (controller decision D13). Not a key: RmlUi has no
			// identifier and no default action for "cancel", so this is handled here
			// rather than dispatched.
			Rml::Element* const Focus = Context.GetFocusElement();

			// The same "is this a REAL focus" rule the snapshot uses (D9): the context
			// root and a document element both hold focus routinely -- Show() focuses the
			// document itself -- and neither is something a player can back out of.
			// Blurring one of them would push focus onto the context root, where
			// ProcessKeyDown has no default action at all and the UI's keyboard goes dead.
			const bool bRealFocus = Focus != nullptr && Focus != Context.GetRootElement() &&
				Focus->GetOwnerDocument() != Focus;
			if (!bRealFocus)
			{
				UE_LOG(LogVaCuus, Verbose, TEXT("View %u: Back with nothing focused inside a document; ignored"),
					Event.ViewId);
				break;
			}

			// NOT Element::Blur(), and this is the whole bug fix.
			//
			// Blur() hands focus to the IMMEDIATE PARENT (Element.cpp:2016-2031), and
			// Element::Focus() has no tab-index gate -- it succeeds on any element whose
			// computed `focus` is not none (Element.cpp:2003-2008), while `focus` is
			// inherited and defaults to auto (StyleSheetSpecification.cpp:376). So ANY plain
			// wrapper div accepts the focus Blur() pushes at it. On our own shipped HUD,
			// where `.slot` sits inside <div id="ability-bar">, Back moved focus onto the
			// wrapper -- which D9 still counts as a real focus element, since it excludes
			// only the context root and document elements -- so bWantsKeyboardFocus stayed
			// true and the player needed one Back press per level of nesting.
			//
			// Focusing the owner document is the single step that always lands where Back
			// means to land: the document keeps focus, so ProcessDefaultAction still runs
			// and a later direction key can re-enter the UI, while D9 stops counting it as
			// "the UI wants the keyboard". Focus(false), not Focus(true): there is nothing
			// to draw a :focus-visible ring around.
			const Rml::String BlurredId = Focus->GetId();
			Rml::ElementDocument* const Document = Focus->GetOwnerDocument();
			if (Document == nullptr || !Document->Focus())
			{
				// A document with `focus: none` on it, which also means nothing inside it was
				// ever focusable -- so this should be unreachable. Fall back to the old
				// single-step blur rather than leaving focus where it was, and say so.
				UE_LOG(LogVaCuus, Warning,
					TEXT("View %u: Back could not focus the owner document of '%s'; falling back to a single blur, ")
					TEXT("so the view may still claim the keyboard"),
					Event.ViewId, UTF8_TO_TCHAR(BlurredId.c_str()));
				Focus->Blur();
				break;
			}

			// Log, not silence: for M2 this is ALL that Back does, and a player pressing B
			// and seeing only "the highlight went away" deserves to be explainable. A real
			// Back binding (close the menu, pop a screen) is game-side policy.
			UE_LOG(LogVaCuus, Log,
				TEXT("View %u: Back moved focus from '%s' to its document; the keyboard returns to the game. ")
				TEXT("Any further Back semantics are the game's own binding."),
				Event.ViewId, UTF8_TO_TCHAR(BlurredId.c_str()));
			break;
		}

		case EVaCuusInputEventKind::ImeSetTextInRange:
		case EVaCuusInputEventKind::ImeSetSelectionRange:
		case EVaCuusInputEventKind::ImeSetCompositionRange:
		case EVaCuusInputEventKind::ImeCommitComposition:
			// The only game -> UI half of the IME contract (controller decision D15). Handled
			// in VaCuusTextInput because every one of these needs the active
			// Rml::TextInputContext, the field-generation guard and the character-offset index
			// space -- none of which belongs in a switch over input kinds.
			//
			// ON THIS QUEUE rather than a separate one so that ordering against the KeyDown and
			// TextInput events the same composition produced is preserved; see the comment on
			// the Ime* kinds in VaCuusInputEvent.h.
			VaCuusTextInput::ApplyMutation(Context, Event.ViewId, Event);
			break;

		case EVaCuusInputEventKind::None:
		default:
			UE_LOG(LogVaCuus, Error, TEXT("An input event for view %u reached the drain with no kind set; dropped"),
				Event.ViewId);
			break;
	}
}
}	 // namespace

FVaCuusUIThread::FVaCuusUIThread(FVaCuusEngine& InEngine, FVaCuusScriptHostFactory InScriptHostFactory)
	: Engine(InEngine)
	, ScriptHostFactory(MoveTemp(InScriptHostFactory))
	, Queues(MakeUnique<FVaCuusUIQueues>())
{
}

FVaCuusUIThread::~FVaCuusUIThread()
{
	Stop();

	// Report "not live" before the join: from here on the object is going away,
	// and IsRunning() must not tempt anyone into using it.
	bThreadLive.store(false, std::memory_order_release);

	if (bInlineMode)
	{
		// No worker to join: the frames ran on this thread, so the teardown runs
		// here too -- under the same scope that made the frames legal.
		check(IsInGameThread());
		FVaCuusInlineUIThreadScope InlineScope;
		Exit();
		return;
	}

	// The platform destructor does Kill(true), i.e. Stop() then join, so Run() and
	// Exit() have both finished by the time this returns -- which is what makes it
	// safe for Exit() to own the RmlUi teardown. Destroying the thread before any
	// member dies is what keeps WakeEvent alive for its last waiter.
	delete Thread;
	Thread = nullptr;

	// Normally Exit() already dropped every host on the UI thread. Anything left
	// here means the worker never ran (Start() not called, or thread creation
	// failed), in which case nothing was ever booted and the game thread may
	// destroy the hosts safely.
}

bool FVaCuusUIThread::Start()
{
	if (Thread != nullptr || bInlineMode)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("UI thread is already started"));
		return true;
	}

	// Name stays within 15 characters: Linux truncates the OS thread name there.
	// BelowNormal keeps us off the game and render threads' backs.
	Thread = FRunnableThread::Create(
		this, TEXT("VaCuusUI"), GVaCuusUIThreadStackSize, TPri_BelowNormal);

	if (Thread == nullptr)
	{
		// FRunnableThread::Create() returns nullptr *without logging anything* when
		// FPlatformProcess::SupportsMultithreading() is false and the runnable has no
		// FSingleThreadRunnable -- commandlets, -nothreading, some server configs.
		// Say so loudly; the caller is expected to fall back to StartInline().
		UE_LOG(LogVaCuus, Warning,
			TEXT("Failed to create the VaCuus UI thread (SupportsMultithreading=%s); the caller must run UI frames inline"),
			FPlatformProcess::SupportsMultithreading() ? TEXT("true") : TEXT("false"));
		return false;
	}

	// Create() blocks on the init sync event, which the platform thread proc
	// triggers on BOTH branches of Init() -- so bInitSucceeded is readable here,
	// and a false means the worker already exited without running Run() or
	// Exit(). Without this check Start() would report success for a dead thread.
	if (!bInitSucceeded.load(std::memory_order_acquire))
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("The VaCuus UI thread exited during Init(); no UI frames will run"));

		// Kill(true) + join on a worker that has already returned; Init() unwound
		// whatever it had built before failing, because Exit() never ran.
		delete Thread;
		Thread = nullptr;
		return false;
	}

	bThreadLive.store(true, std::memory_order_release);

	UE_LOG(LogVaCuus, Log, TEXT("UI thread started (id %u)"), GetThreadId());
	return true;
}

bool FVaCuusUIThread::StartInline()
{
	check(IsInGameThread());
	checkf(Thread == nullptr, TEXT("StartInline() must not follow a successful Start()"));

	// CLAIMABILITY TESTED BEFORE THE SCOPE, for the same reason Init() no longer publishes
	// its id before its own claim. The scope makes this thread *be* the UI thread for its
	// whole duration, so entering it while another thread owns RmlUi would make
	// IsInUIThread() read false ON that thread -- and every check() guarding an RmlUi call
	// there fire -- for as long as the doomed boot attempt takes. Init() would refuse anyway;
	// this only stops us lying about who the UI thread is while it does.
	//
	// Not a lock, and it does not need to be: StartInline() is game-thread-only and only
	// reached when Start() already failed for want of multithreading, so there is no second
	// claimant to race with.
	if (!Engine.IsClaimableOnThisThread())
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("VaCuus UI frames cannot run inline: RmlUi is already owned by another thread"));
		return false;
	}

	// Everything Init() does -- booting RmlUi, then publishing the thread id -- happens
	// on this thread, which therefore becomes the RmlUi owner for good.
	FVaCuusInlineUIThreadScope InlineScope;
	if (!Init())
	{
		return false;
	}

	bInlineMode = true;

	UE_LOG(LogVaCuus, Warning,
		TEXT("VaCuus UI frames run INLINE on the game thread (no multithreading support); expect game-thread UI cost"));
	return true;
}

bool FVaCuusUIThread::IsInlineMode() const
{
	return bInlineMode;
}

void FVaCuusUIThread::RunFrameInline()
{
	check(IsInGameThread());
	checkf(bInlineMode, TEXT("RunFrameInline() is only valid after StartInline()"));

	if (bStopRequested.load(std::memory_order_acquire))
	{
		return;
	}

	FVaCuusInlineUIThreadScope InlineScope;

	// (inline stack anchor: M4) Every inline frame runs at a DIFFERENT depth of the
	// game thread's stack, and quickjs checks overflow against an anchor, not a size
	// (spec 2(c)) -- so the host must re-anchor before ANY JS this frame runs. At the
	// top of the frame rather than in PumpFrame(), because DrainCommands executes
	// document scripts and ExecuteScript (from Task 6 on) before the pump ever runs;
	// see IVaCuusScriptHost::OnInlineFrameEntry for the full argument.
	if (ScriptHost.IsValid())
	{
		ScriptHost->OnInlineFrameEntry();
	}

	RunFrame();
	FrameCount.fetch_add(1, std::memory_order_release);
}

bool FVaCuusUIThread::RequestGracefulShutdown(double TimeoutSeconds)
{
	check(IsInGameThread());

	if (bShutdownDrained.load(std::memory_order_acquire))
	{
		return true;
	}

	if (!bInlineMode && !bThreadLive.load(std::memory_order_acquire))
	{
		// The worker never ran (Start() failed or was never called), so there is
		// nothing queued and nothing to close.
		return true;
	}

	if (bStopRequested.load(std::memory_order_acquire))
	{
		// A hard stop already closed the queue; the Shutdown command could not even
		// be pushed. Let the caller log the fallback.
		return false;
	}

	EnqueueShutdown();

	if (bInlineMode)
	{
		// No worker to wait for: the frame that drains the command is ours to run.
		RunFrameInline();
		return bShutdownDrained.load(std::memory_order_acquire);
	}

	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!bShutdownDrained.load(std::memory_order_acquire))
	{
		if (FPlatformTime::Seconds() >= Deadline)
		{
			return false;
		}

		FPlatformProcess::Sleep(0.001f);
	}

	return true;
}

bool FVaCuusUIThread::IsStopping() const
{
	return bStopRequested.load(std::memory_order_acquire);
}

void FVaCuusUIThread::Stop()
{
	// Called from the owner's thread, including from inside Kill(true). Both halves
	// are idempotent, so calling this twice -- or after the worker already left --
	// is harmless: the store repeats and the latch is simply left set.
	bStopRequested.store(true, std::memory_order_release);

	// Load-bearing: without this the worker stays parked in Wait(MAX_uint32) and the
	// join inside Kill(true) never returns.
	WakeEvent->Trigger();
}

void FVaCuusUIThread::Trigger()
{
	// The event is auto-reset, so it is a binary latch: N triggers arriving while a
	// frame is in flight wake the worker exactly once. In inline mode nothing waits
	// on it; the owner calls RunFrameInline() instead.
	WakeEvent->Trigger();
}

uint32 FVaCuusUIThread::AllocateViewId()
{
	// Ids are unique per process, not per subsystem: several game instances share
	// this one thread and its view map.
	return NextViewId.fetch_add(1, std::memory_order_relaxed);
}

void FVaCuusUIThread::EnqueueAddView(uint32 ViewId, TUniquePtr<IVaCuusDocumentHost> Host, FIntPoint ViewSize,
	const TSharedRef<FVaCuusViewStatus>& Status)
{
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::AddView;
	Command.ViewId = ViewId;
	Command.ViewSize = ViewSize;
	Command.Host = MoveTemp(Host);
	Command.Status = Status;
	Enqueue(MoveTemp(Command));
}

void FVaCuusUIThread::EnqueueRemoveView(uint32 ViewId)
{
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::RemoveView;
	Command.ViewId = ViewId;
	Enqueue(MoveTemp(Command));
}

void FVaCuusUIThread::EnqueueLoadDocumentFile(uint32 ViewId, const FString& VfsPath, uint64 LoadSerial, FIntPoint ViewSize)
{
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::LoadDocumentFile;
	Command.ViewId = ViewId;
	Command.Payload = VfsPath;
	Command.ViewSize = ViewSize;
	Command.LoadSerial = LoadSerial;
	Enqueue(MoveTemp(Command));
}

void FVaCuusUIThread::EnqueueLoadDocumentFromMemory(uint32 ViewId, const FString& RmlSource, uint64 LoadSerial, FIntPoint ViewSize)
{
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::LoadDocumentMemory;
	Command.ViewId = ViewId;
	Command.Payload = RmlSource;
	Command.ViewSize = ViewSize;
	Command.LoadSerial = LoadSerial;
	Enqueue(MoveTemp(Command));
}

void FVaCuusUIThread::EnqueueCloseDocument(uint32 ViewId)
{
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::CloseDocument;
	Command.ViewId = ViewId;
	Enqueue(MoveTemp(Command));
}

void FVaCuusUIThread::EnqueueResize(uint32 ViewId, FIntPoint ViewSize)
{
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::Resize;
	Command.ViewId = ViewId;
	Command.ViewSize = ViewSize;
	Enqueue(MoveTemp(Command));
}

void FVaCuusUIThread::EnqueueSetVisible(uint32 ViewId, bool bVisible)
{
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::SetVisible;
	Command.ViewId = ViewId;
	Command.bVisible = bVisible;
	Enqueue(MoveTemp(Command));
}

void FVaCuusUIThread::EnqueueBindModel(uint32 ViewId, const TSharedRef<FVaCuusBoundModel>& Model)
{
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::BindModel;
	Command.ViewId = ViewId;
	Command.Model = Model;
	Enqueue(MoveTemp(Command));
}

void FVaCuusUIThread::EnqueueDumpModel(uint32 ViewId, FName ModelName)
{
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::DumpModel;
	Command.ViewId = ViewId;

	// The name rides in Payload, which is the command's general-purpose string. None is carried
	// as the empty string and means "every model of this view" -- the handler cannot tell the
	// difference between an FName that stringifies to "None" and no name at all, and only one of
	// those is a model somebody could have bound.
	Command.Payload = ModelName.IsNone() ? FString() : ModelName.ToString();
	Enqueue(MoveTemp(Command));
}

void FVaCuusUIThread::EnqueueExecuteScript(uint32 ViewId, const FString& Source, const FString& SourceName)
{
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::ExecuteScript;
	Command.ViewId = ViewId;
	Command.Payload = Source;
	Command.SourceName = SourceName;
	Enqueue(MoveTemp(Command));
}

void FVaCuusUIThread::EnqueueShutdown()
{
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::Shutdown;
	Enqueue(MoveTemp(Command));
}

void FVaCuusUIThread::EnqueueClearAssetCaches()
{
	// No ViewId: process-global caches, applied before the drain's per-view routing.
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::ClearAssetCaches;
	Enqueue(MoveTemp(Command));
}

void FVaCuusUIThread::EnqueueInput(uint32 ViewId, FVaCuusInputEvent Event)
{
	// Same rule as commands: once a stop is requested the queues are closed, so
	// nothing can be pushed behind the drain that the worker will never see.
	if (bStopRequested.load(std::memory_order_acquire))
	{
		UE_LOG(LogVaCuus, Verbose, TEXT("Input event dropped: the UI thread is stopping"));
		return;
	}

	Event.ViewId = ViewId;
	Queues->Input.Enqueue(MoveTemp(Event));

	// No Trigger() here on purpose -- see the header. The frame that consumes this is
	// the one UVaCuusSubsystem::Tick asks for later in this same game frame.
}

void FVaCuusUIThread::Enqueue(FVaCuusUICommand&& Command)
{
	// Spec §4 teardown order starts with "stop accepting commands": once a stop is
	// requested the queue is closed, so nothing can be pushed behind the drain that
	// the worker will never see (and, worse, that would keep a document alive past
	// the close in Exit()).
	if (bStopRequested.load(std::memory_order_acquire))
	{
		// A BindModel is the one command whose loss has no second symptom -- the model never
		// binds, every UpdateModel writes into a channel nothing consumes, and the idle gate
		// correctly publishes nothing (the drain's unknown-view branch makes the same
		// argument at the same level). Everything else lost here is a frame of work during
		// teardown, which Verbose is for.
		if (Command.Kind == EVaCuusCommandKind::BindModel)
		{
			UE_LOG(LogVaCuus, Error,
				TEXT("BindModel('%s') for view %u dropped: the UI thread is stopping. The model will never bind"),
				Command.Model.IsValid() ? *Command.Model->GetModelNameString() : TEXT("<none>"), Command.ViewId);
		}
		else
		{
			UE_LOG(LogVaCuus, Verbose, TEXT("UI command dropped: the UI thread is stopping"));
		}
		return;
	}

	Queues->Commands.Enqueue(MoveTemp(Command));
	Trigger();
}

bool FVaCuusUIThread::IsRunning() const
{
	// Both halves are atomic, so this answer is safe from any thread (the old
	// version read the plain Thread pointer). Stop() asks the worker to leave; it
	// does not join. Reporting "not running" from the moment the request lands is
	// the only answer that does not race the worker's own teardown, and it is what
	// callers actually want to gate work on.
	return bThreadLive.load(std::memory_order_acquire) && !bStopRequested.load(std::memory_order_acquire);
}

uint32 FVaCuusUIThread::GetThreadId() const
{
	return ThreadId.load(std::memory_order_acquire);
}

uint64 FVaCuusUIThread::GetFrameCount() const
{
	return FrameCount.load(std::memory_order_acquire);
}

int32 FVaCuusUIThread::GetNumViews() const
{
	return NumViews.load(std::memory_order_acquire);
}

int32 FVaCuusUIThread::GetNumBoundModels() const
{
	return NumBoundModels.load(std::memory_order_acquire);
}

uint64 FVaCuusUIThread::GetNumAssetCacheClears() const
{
	return NumAssetCacheClears.load(std::memory_order_acquire);
}

bool FVaCuusUIThread::WaitForFrameCount(uint64 Target, double TimeoutSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;

	while (GetFrameCount() < Target)
	{
		if (FPlatformTime::Seconds() >= Deadline)
		{
			return false;
		}

		FPlatformProcess::Sleep(0.001f);
	}

	return true;
}

bool FVaCuusUIThread::IsInUIThread()
{
	const uint32 UIThreadId = GVaCuusUIThreadId.load(std::memory_order_relaxed);
	return UIThreadId != 0 && FPlatformTLS::GetCurrentThreadId() == UIThreadId;
}

bool FVaCuusUIThread::HasScriptHost() const
{
	return bScriptHostLive.load(std::memory_order_acquire);
}

IVaCuusScriptHost* FVaCuusUIThread::GetActiveScriptHost()
{
	// The assert IS the synchronization story: one writer, same thread, no
	// concurrent reader possible (see GVaCuusActiveScriptHost).
	check(IsInUIThread());
	return GVaCuusActiveScriptHost;
}

bool FVaCuusUIThread::Init()
{
	// Runs on the worker thread (or, in inline mode, on the game thread inside the
	// inline scope). FRunnableThread::Create() waits for this to return, so
	// everything published here is visible to the caller once Start() succeeds.
	const uint32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();

	// THE CLAIM COMES FIRST; NOTHING IS PUBLISHED UNTIL IT SUCCEEDS.
	//
	// RmlUi is process-global, so it boots here once for every view that will ever exist --
	// and this thread becomes its owner. Refusing rather than asserting when somebody else
	// already owns it keeps an automation test that holds RmlUi on its own thread from
	// turning into a check() crash.
	//
	// THE ORDER IS THE POINT, and it used to be the other way round. GVaCuusUIThreadId is
	// what backs every check(FVaCuusUIThread::IsInUIThread()) in the plugin -- i.e. it IS the
	// enforcement of "every RmlUi call happens on the UI thread". Publishing this thread's id
	// before earning the right to it had two consequences, both silent:
	//
	//  - a SECOND FVaCuusUIThread booting while a first one is live (only a test ever builds
	//    two -- Tests/VaCuusUIThreadTest.cpp) took the identity away from the live UI thread
	//    for the length of its boot attempt, so that thread's own guards read false in the
	//    middle of a frame;
	//  - and the failure path then stored 0 rather than the id it had displaced, so
	//    IsInUIThread() stayed false on the REAL UI thread for the rest of the process. Where
	//    DO_CHECK is 0 that is not an assert: it is every guard around RmlUi quietly
	//    evaporating.
	//
	// With the claim first there is nothing to retract and nothing to restore -- a refused
	// boot never touched the global. The inline path is the one case that genuinely has to
	// publish before Init() runs at all, and it saves and restores for exactly that reason
	// (FVaCuusInlineUIThreadScope).
	const bool bBooted = Engine.IsClaimableOnThisThread() && Engine.Initialize();
	if (!bBooted)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("The VaCuus UI thread could not boot RmlUi (already owned by another thread?); no UI frames will run"));

		// Exit() will NOT run when Init() fails, and there is nothing here for it to do:
		// neither id below was ever published.
		bInitSucceeded.store(false, std::memory_order_release);
		return false;
	}

	// Now, and only now. The process-wide id goes last of the two because it is the one
	// every OTHER thread reads; ThreadId is only ever read for diagnostics.
	//
	// The retraction is Exit()'s matching store(0), and it is guaranteed on both paths that
	// get here: the platform thread proc runs Run() and then Exit() for exactly the case
	// where Init() returned true (PThreadRunnableThread.cpp:16-33 -- and the else branch at
	// :34-38 is why the failure path above must leave nothing behind), and inline mode sets
	// bInlineMode only after a successful Init(), which is what makes the destructor call
	// Exit(). So the publication and its retraction are paired by that contract, not here.
	ThreadId.store(CurrentThreadId, std::memory_order_release);
	GVaCuusUIThreadId.store(CurrentThreadId, std::memory_order_release);

	// (script host: M4) AFTER the RmlUi boot -- a host may reach Rml services from its
	// first call -- and AFTER the id publication, so IsInUIThread() already answers true
	// for anything the factory constructs. vacuus.Js.Enable is read ONCE, here, at the
	// only moment a host can come to exist for this thread (see the cvar's comment for
	// why later flips are no-ops). Cannot fail: a factory that returns null simply means
	// no host, same as no factory, and there is nothing to unwind if Init() had failed
	// above -- the host is created strictly after the last failure point.
	if (ScriptHostFactory && CVarVaCuusJsEnable.GetValueOnAnyThread() != 0)
	{
		ScriptHost = ScriptHostFactory();
	}
	bScriptHostLive.store(ScriptHost.IsValid(), std::memory_order_release);

	// The document hosts' route to the script host (GetActiveScriptHost --
	// AdoptDocument and CloseDocument call through it, M4 Task 6). Published
	// here, on the UI thread, before any command can be drained; Exit() retracts
	// it right after the host dies.
	GVaCuusActiveScriptHost = ScriptHost.Get();
	if (ScriptHost.IsValid())
	{
		UE_LOG(LogVaCuus, Log, TEXT("UI thread created its script host; JS phases are live"));
	}

	// The write router (M4 Task 9, spec 3.10) -- registered UNCONDITIONALLY, script host
	// or not: two-way binding is a core surface (a Blueprint game with JS off still gets
	// OnModelWrite from a data-checked checkbox). This is the registration whose absence
	// makes an M3 configuration byte-identical; the M3a/M3b suites running here with it
	// PRESENT stay untouched anyway, because their fixture models never enter the
	// router's registry (FVaCuusWriteRouter's class comment carries both halves).
	FVaCuusWriteRouter::RegisterRouter();

	// Set last, and before returning: Start() reads it as soon as Create() returns.
	bInitSucceeded.store(true, std::memory_order_release);
	return true;
}

uint32 FVaCuusUIThread::Run()
{
	while (!bStopRequested.load(std::memory_order_acquire))
	{
		WakeEvent->Wait();

		// Stop() triggers the event to break this wait; do not run a frame for it.
		if (bStopRequested.load(std::memory_order_acquire))
		{
			break;
		}

		RunFrame();
		FrameCount.fetch_add(1, std::memory_order_release);
	}

	return 0;
}

void FVaCuusUIThread::Exit()
{
	// Also runs on the worker thread, immediately after Run() returns -- the
	// FRunnable::Exit() header comment claiming "the aggregating thread" is wrong.
	// This is the teardown hook for anything Init() booted; the destructor is not,
	// because it runs on the owner's thread.
	check(IsInUIThread());

	// 0. Last word on the queue. Normally empty: the graceful path drained it in
	// DrainCommands() and Enqueue() has been closed ever since. Anything here got in
	// through the narrow window between Enqueue()'s check and Stop()'s store, or was
	// queued before a hard Stop() that skipped the in-band shutdown entirely --
	// either way the work is lost, and losing it silently is what this reports.
	// Done before the hosts die so a dropped AddView's host is destroyed here, on
	// the thread that would have booted it.
	if (const int32 NumDropped = DrainAndDiscardCommands(); NumDropped > 0)
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("UI thread exited with %d command(s) still queued (no in-band shutdown drained them); dropped"),
			NumDropped);
	}

	// 1a. EVERY DOCUMENT CLOSES FIRST, while the script host is still alive (the
	// spec 5 hard-stop split). CloseDocument() fires
	// IVaCuusScriptHost::OnDocumentClosing through the host seam
	// (FVaCuusRmlDocumentHost::CloseDocument -> GetActiveScriptHost), so unload
	// JS runs with the trees, the contexts and the runtime all live -- the
	// hard-stop equivalent of the graceful path, where the in-band Shutdown
	// command already closed everything inside DrainCommands and this loop finds
	// only nulls. Without this split the documents would die inside step 1c's
	// fused close-and-RemoveContext, AFTER the script host is gone, and their
	// unload JS would silently never run -- on exactly one of the two paths.
	for (TPair<uint32, TUniquePtr<IVaCuusDocumentHost>>& Pair : Hosts)
	{
		Pair.Value->CloseDocument();
	}

	// 1b. The script host dies SECOND, before any document host's Shutdown():
	// its own Shutdown() frees JS refs and runs quickjs finalizers that touch
	// RmlUi objects, which needs every context, element tree and instancer still
	// alive (spec 5) -- the closed trees above are only QUEUED for free, and the
	// context teardown in 1c is what actually releases them -- and quickjs
	// itself must be gone before Rml::Shutdown() in step 2. The seam pointer is
	// retracted with it: nothing after this line may find a script host.
	if (ScriptHost.IsValid())
	{
		ScriptHost->Shutdown();
		ScriptHost.Reset();
		bScriptHostLive.store(false, std::memory_order_release);
	}
	GVaCuusActiveScriptHost = nullptr;

	// 1c. Every view lets go of its context (and releases its render-side
	// resources), still on this thread. The per-host Shutdown() still contains
	// its own document close for the paths that reach it directly (RemoveView,
	// the host destructor); here 1a already emptied those slots.
	for (TPair<uint32, TUniquePtr<IVaCuusDocumentHost>>& Pair : Hosts)
	{
		Pair.Value->Shutdown();
	}
	NumViews.store(0, std::memory_order_release);

	// 2. RmlUi goes down, which destroys the RenderManagers it keyed on the hosts'
	// render interfaces -- so every host, live or retired, must still exist here.
	// Engine is held by reference for a reason: FVaCuusEngine::Get() would go
	// through FModuleManager, which asserts on a non-game thread and doubly so while
	// the module that owns us is being unloaded -- which is exactly when this runs.
	if (Engine.IsInitialized())
	{
		Engine.Shutdown();
	}

	// 2b. The write router retracts BEFORE the models die below: its registry holds raw
	// FVaCuusBoundModel pointers, and this ordering is what makes them safe to hold at
	// all. (The game-thread queue is deliberately left to its own consumer -- see
	// FVaCuusWriteRouter::UnregisterRouter.)
	FVaCuusWriteRouter::UnregisterRouter();

	// 3. Only now may the hosts themselves die, on the thread that built them. The models go
	// with them, and for the same reason RemoveView() drops them after Shutdown(): every
	// context is down by this point, so nothing holds a pointer into a UI shadow any more.
	Hosts.Empty();
	RetiredHosts.Empty();
	Models.Empty();
	NumBoundModels.store(0, std::memory_order_release);

	// 4. And the process-wide definition cache, HERE RATHER THAN AT STATIC DESTRUCTION. Its
	// entries hold TStrongObjectPtr<const UScriptStruct>, so leaving the map to the C++ runtime
	// means calling UObjectBase::ReleaseRef after main returns, against a UObject system that
	// may already be gone (FVaCuusDefinitionRegistry::ReleaseAll carries the argument). Step 3
	// above is what makes this the safe point: every context, RmlUi itself, every host and every
	// model are down, so nothing else can still reach a definition.
	//
	// Logged rather than silent because this line is otherwise unobservable -- it runs on a
	// thread nobody is watching, at a moment nothing else reports.
	if (const int32 NumDefinitions = FVaCuusDefinitionRegistry::ReleaseAll(); NumDefinitions > 0)
	{
		UE_LOG(LogVaCuus, Log, TEXT("UI thread exit: released %d cached model definition set(s)"), NumDefinitions);
	}

	GVaCuusUIThreadId.store(0, std::memory_order_release);
	ThreadId.store(0, std::memory_order_release);
}

void FVaCuusUIThread::RunFrame()
{
	// Everything below is UI-thread-affine; assert it up front so the first
	// accidental cross-thread call is caught here rather than inside RmlUi.
	check(IsInUIThread());

	// Commands first, then input: a view that was registered, resized or reloaded by
	// this frame's commands receives this frame's input against its new state, never
	// against the previous one.
	//
	// SCOPED AT THE CALL SITE RATHER THAN INSIDE EACH FUNCTION so that the three phases
	// of a UI frame sit next to each other and can be read as a decomposition of it: the
	// remainder of RunFrame() is the per-view record loop, which is already covered by
	// Update and Record inside RecordAndPublishFrame(). Nothing wraps that loop, because a
	// scope around it would double-count those two into every window it printed.
	{
		VACUUS_PERF_SCOPE(DrainCommands);
		DrainCommands();
	}
	{
		VACUUS_PERF_SCOPE(DrainInput);
		DrainInput();
	}
	{
		// (data snapshots: M3a)
		//
		// HERE, AND FOR TWO REASONS. After both drains, so a model bound or a view loaded by
		// this frame's commands is applied into this frame; and BEFORE Context::Update(), so
		// the re-evaluation a dirtied variable causes is paid inside Update -- which is where
		// spec 9 budgets it, and where the Update scope already measures it.
		VACUUS_PERF_SCOPE(DataApply);
		ApplyModelUpdates();
	}

	// (js pump: M4; Task 3 fills the internals -- rAF, timers, the bounded job drain)
	//
	// The DataApply placement argument again, one phase later: after the drains, so a
	// script loaded or enqueued by this frame's commands is pumped into this frame; and
	// before the record loop's Context::Update(), so what the callbacks wrote to the DOM
	// is laid out and drawn by THIS frame, not discovered a frame late. UNGATED on
	// HasView() for the reason the data apply is (see the record-loop comment below):
	// HasView() is a recordability test, and timers for a sizeless-but-alive view --
	// every UMG view before its first Slate tick -- must keep firing.
	//
	// The timestamp is Rml::GetSystemInterface()->GetElapsedTime(), sampled ONCE here:
	// it is the clock RmlUi advances its own animations on (Clock::GetElapsedTime,
	// Clock.cpp:7-14, consumed in Element::AdvanceAnimations, Element.cpp:2838-2849), so
	// a rAF callback and a CSS animation in the same frame see the same now (spec 3.5).
	if (ScriptHost.IsValid())
	{
		VACUUS_PERF_SCOPE(JsPump);
		ScriptHost->PumpFrame(Rml::GetSystemInterface()->GetElapsedTime());
	}

	// One recorded frame per view, each publishing its own command buffer straight to
	// the render thread and its own interactive-region snapshot straight to the game
	// thread's view handle -- no game-thread hop in either direction.
	//
	// THE DATA APPLY ABOVE IS DELIBERATELY *NOT* IN THIS LOOP, which is the obvious place for
	// it and the wrong one. HasView() is a RECORDABILITY test, not a liveness test: it
	// additionally requires a non-degenerate view size (FVaCuusRmlDocumentHost::HasView, and
	// the multi-view test's probe agrees), and every UMG view fails it until its first Slate
	// tick -- UVaCuusWidget::RebuildWidget creates its view with FIntPoint::ZeroValue on
	// purpose, because the only correct size is the arranged pixel rect UMG has not measured
	// yet (VaCuusUMGWidget.cpp:70-78). A view whose document has just been closed is skipped
	// here too, once its owed clearing frame is spent.
	//
	// Applying inside this loop would therefore leave updates sitting in the channel for a
	// view that is perfectly alive, and then deliver them all at once on the frame the size
	// arrives -- correct values, arbitrarily late, with no diagnostic. Worse, the channel is
	// latest-wins, so what actually arrives is the newest publish only, which merely LOOKS
	// like a burst.
	for (TPair<uint32, TUniquePtr<IVaCuusDocumentHost>>& Pair : Hosts)
	{
		if (Pair.Value->HasView())
		{
			Pair.Value->RecordAndPublishFrame();
		}
	}

	// (js gc: M4) The controlled collection point, LAST in the frame: every view has
	// recorded and published, so a pause here delays only the next wakeup, never this
	// frame's output (spec 3.6). The host declines almost every call -- it collects on
	// allocation growth or an OOM fallback -- which is why the scope wraps the check
	// too: the per-frame cost of deciding "no" is part of the phase's budget.
	if (ScriptHost.IsValid())
	{
		VACUUS_PERF_SCOPE(JsGC);
		ScriptHost->CollectGarbage(TEXT("frame"));
	}
}

void FVaCuusUIThread::ApplyModelUpdates()
{
	check(IsInUIThread());

	// OVER EVERY VIEW WITH MODELS, gated on nothing. See the record loop for why the gate that
	// looks like it belongs here does not.
	//
	// Costs a map walk per frame and nothing else when no model has published: a channel with
	// nothing outstanding never swaps, so ConsumeUpdate's SwapAndRead hands back the same
	// buffer, the generation has not moved and the applier does not run
	// (FVaCuusModelChannel::ConsumeUpdate). That is what spec 9's "idle -> 0 published frames"
	// row rests on -- nothing writes the DOM, so nothing changes the frame hash.
	for (TPair<uint32, TArray<TSharedRef<FVaCuusBoundModel>>>& Pair : Models)
	{
		for (const TSharedRef<FVaCuusBoundModel>& Model : Pair.Value)
		{
			Model->ApplyPendingUpdate();
		}
	}

	// The write router's revert-dirty (spec 3.10), AFTER the applies: a game that heard
	// OnModelWrite and changed the field has just applied the new truth, and re-dirtying
	// the same top-level name is a set-emplace, not a second evaluation. Before this
	// frame's Context::Update either way, so a control a click mutated in THIS frame's
	// DrainInput snaps back to the shadow within the same frame. Free when nothing was
	// routed: an empty array walk.
	FVaCuusWriteRouter::FlushPendingReverts();
}

void FVaCuusUIThread::DrainCommands()
{
	check(IsInUIThread());

	while (TOptional<FVaCuusUICommand> Command = Queues->Commands.Dequeue())
	{
		if (Command->Kind == EVaCuusCommandKind::None)
		{
			// Producer bug: Kind is the one field with no sensible default, so an
			// unset one is reported rather than guessed at.
			UE_LOG(LogVaCuus, Error,
				TEXT("A UI command for view %u reached the drain with no kind set; dropped"), Command->ViewId);
			continue;
		}

		if (Command->Kind == EVaCuusCommandKind::Shutdown)
		{
			// In-band graceful stop -- the path FVaCuusModule::StopUIThread() takes:
			// close every document now, then leave the loop after this frame. The
			// owner still joins us afterwards, and Exit() still runs the full
			// teardown. Each CloseDocument() fires OnDocumentClosing through the
			// host seam (M4 Task 6), so unload JS runs HERE, while the frame loop,
			// the contexts and the runtime are all still alive -- the graceful
			// half of spec 5's ordering; Exit()'s step-1a close loop is the
			// hard-stop half and finds these slots already empty.
			for (TPair<uint32, TUniquePtr<IVaCuusDocumentHost>>& Pair : Hosts)
			{
				Pair.Value->CloseDocument();
			}
			bStopRequested.store(true, std::memory_order_release);

			// Anything queued behind a shutdown is dead by definition; drop it here
			// and say how much, so the loss is as visible as Enqueue()'s.
			const int32 NumDropped = DrainAndDiscardCommands();
			UE_LOG(LogVaCuus, Log,
				TEXT("UI thread stopping on an in-band shutdown command (%d view(s) closed, %d queued command(s) dropped behind it)"),
				Hosts.Num(), NumDropped);

			// Published last: the owner is waiting on this to know the graceful path
			// actually happened, and everything above must be visible when it does.
			bShutdownDrained.store(true, std::memory_order_release);
			return;
		}

		if (Command->Kind == EVaCuusCommandKind::AddView)
		{
			AddView(*Command);
			continue;
		}

		if (Command->Kind == EVaCuusCommandKind::RemoveView)
		{
			RemoveView(Command->ViewId);
			continue;
		}

		if (Command->Kind == EVaCuusCommandKind::ClearAssetCaches)
		{
			ClearAssetCaches();
			continue;
		}

		if (Command->Kind == EVaCuusCommandKind::ExecuteScript)
		{
			// AHEAD OF THE HOST LOOKUP, like DumpModel: the target is the SCRIPT
			// host, whose view registry has the same membership as Hosts
			// (OnViewAdded fires exactly when AddView registers a view), and whose
			// unknown-view refusal is already the Error the BindModel lesson
			// demands (FVaCuusJsScriptHost::ExecuteScript). What must not be quiet
			// HERE is the other loss: with no script host at all, a queued script
			// is this seam's BindModel -- nothing downstream ever misses it.
			if (ScriptHost.IsValid())
			{
				ScriptHost->ExecuteScript(Command->ViewId, Command->Payload, Command->SourceName);
			}
			else
			{
				UE_LOG(LogVaCuus, Error,
					TEXT("ExecuteScript('%s') for view %u dropped: this UI thread has no script host ")
					TEXT("(vacuus.Js.Enable was 0 at boot, or the VaCuusJs module is absent)"),
					*Command->SourceName, Command->ViewId);
			}
			continue;
		}

		if (Command->Kind == EVaCuusCommandKind::DumpModel)
		{
			// AHEAD OF THE HOST LOOKUP, deliberately: the models are keyed on the view in
			// Models, not held by the host, and the lookup below drops a command for a view it
			// cannot find at Verbose. A diagnostic that answered silence would be worse than no
			// diagnostic -- somebody would read the game half, see no UI half, and conclude the
			// bind had failed when the view had simply been retired.
			DumpModel(Command->ViewId, FName(*Command->Payload));
			continue;
		}

		IVaCuusDocumentHost* Host = FindHost(Command->ViewId);
		if (Host == nullptr)
		{
			// BindModel IS NOT AN ORDINARY DROP, and it gets its own level for the same reason
			// DumpModel was hoisted above this lookup: losing it is not a lost frame of work,
			// it is this milestone's signature failure arriving by the quietest door there is.
			// The model never binds, every UpdateModel afterwards writes into a channel nothing
			// consumes, the document reads empty -- and the idle gate correctly publishes
			// nothing, so there is no second symptom anywhere. The DumpModel hoist argues that
			// it "would be the one command in the plugin that can fail silently"; that applies
			// with more force here, because the loss of THIS command is the failure the dump
			// exists to find.
			//
			// Not hoisted above the lookup like DumpModel, because unlike DumpModel this
			// command genuinely needs the host: there is nothing to do with it but report it.
			if (Command->Kind == EVaCuusCommandKind::BindModel)
			{
				UE_LOG(LogVaCuus, Error,
					TEXT("BindModel('%s') dropped: view %u is not registered on the UI thread. The model will never bind, and every ")
					TEXT("UpdateModel for it goes nowhere"),
					Command->Model.IsValid() ? *Command->Model->GetModelNameString() : TEXT("<none>"), Command->ViewId);
				continue;
			}

			// Ordinary during teardown: the view was removed while commands for it
			// were still in flight.
			UE_LOG(LogVaCuus, Verbose, TEXT("UI command for unknown view %u dropped"), Command->ViewId);
			continue;
		}

		// Applied first and for every kind: a document then loads straight into the
		// right layout size. SetViewSize() is idempotent, so a burst of resize
		// commands costs exactly one relayout -- that is the coalescing.
		if (Command->ViewSize.X > 0 && Command->ViewSize.Y > 0)
		{
			Host->SetViewSize(Command->ViewSize);
		}

		switch (Command->Kind)
		{
			case EVaCuusCommandKind::LoadDocumentFile:
				Host->LoadDocumentFromFile(Command->Payload, Command->LoadSerial);
				break;

			case EVaCuusCommandKind::LoadDocumentMemory:
				Host->LoadDocumentFromMemory(Command->Payload, Command->LoadSerial);
				break;

			case EVaCuusCommandKind::CloseDocument:
				Host->CloseDocument();
				break;

			case EVaCuusCommandKind::BindModel:
				BindModel(Command->ViewId, *Host, Command->Model);
				break;

			case EVaCuusCommandKind::SetVisible:
				Host->SetVisible(Command->bVisible);
				break;

			case EVaCuusCommandKind::Resize:
				// Nothing left to do: the view size was applied above.
				break;

			default:
				checkNoEntry();
				break;
		}
	}
}

void FVaCuusUIThread::DrainInput()
{
	check(IsInUIThread());

	// A stopping thread has already closed every document (the in-band Shutdown
	// command does that), so there is nothing left for an event to reach. Drained
	// anyway rather than left behind, so the loss is uniform with the command queue's.
	const bool bStopping = bStopRequested.load(std::memory_order_acquire);

	while (TOptional<FVaCuusInputEvent> Event = Queues->Input.Dequeue())
	{
		if (bStopping)
		{
			continue;
		}

		IVaCuusDocumentHost* Host = FindHost(Event->ViewId);
		if (Host == nullptr)
		{
			// Ordinary during teardown, and ordinary for a stale widget: the view was
			// removed while its input was still in flight.
			UE_LOG(LogVaCuus, Verbose, TEXT("Input event for unknown view %u dropped"), Event->ViewId);
			continue;
		}

		Rml::Context* Context = Host->GetContext();
		if (Context == nullptr)
		{
			// A live view whose context is gone (mid-shutdown). Nothing to dispatch to;
			// dropping is the only option and the snapshot has already been emptied, so
			// the game thread has stopped claiming this region anyway.
			continue;
		}

		DispatchInputEvent(*Context, *Event);
	}
}

void FVaCuusUIThread::AddView(FVaCuusUICommand& Command)
{
	check(IsInUIThread());

	if (!Command.Host.IsValid() || !Command.Status.IsValid())
	{
		UE_LOG(LogVaCuus, Error, TEXT("AddView for view %u carried no host"), Command.ViewId);
		return;
	}

	if (Hosts.Contains(Command.ViewId))
	{
		UE_LOG(LogVaCuus, Error, TEXT("View %u is already registered; the AddView is ignored"), Command.ViewId);
		return;
	}

	TUniquePtr<IVaCuusDocumentHost> Host = MoveTemp(Command.Host);
	if (!Host->Initialize(Command.ViewId, Command.Status.ToSharedRef()))
	{
		// Contract: a host whose Initialize() failed has rolled itself back, so it
		// can simply be dropped here (on this thread) with no Shutdown().
		UE_LOG(LogVaCuus, Error, TEXT("View %u failed to boot; it will produce no frames"), Command.ViewId);
		return;
	}

	if (Command.ViewSize.X > 0 && Command.ViewSize.Y > 0)
	{
		Host->SetViewSize(Command.ViewSize);
	}

	Hosts.Add(Command.ViewId, MoveTemp(Host));
	NumViews.store(Hosts.Num(), std::memory_order_release);

	// After the host is booted and registered: a script host that reacted by touching
	// the view would find it in every map a frame can reach it through.
	if (ScriptHost.IsValid())
	{
		ScriptHost->OnViewAdded(Command.ViewId);
	}

	UE_LOG(LogVaCuus, Log, TEXT("View %u registered on the UI thread (%d view(s) now)"),
		Command.ViewId, Hosts.Num());
}

void FVaCuusUIThread::RemoveView(uint32 ViewId)
{
	check(IsInUIThread());

	TUniquePtr<IVaCuusDocumentHost> Host;
	if (!Hosts.RemoveAndCopyValue(ViewId, Host))
	{
		UE_LOG(LogVaCuus, Verbose, TEXT("RemoveView for unknown view %u ignored"), ViewId);
		return;
	}

	NumViews.store(Hosts.Num(), std::memory_order_release);

	// BEFORE the host's Shutdown(), which is what destroys the Rml context and its
	// element tree: the script host frees this view's JS state -- wrappers, listeners,
	// eventually the context (Task 3 on) -- against a still-live tree, the only order
	// in which RmlUi's own deferred detach can then reclaim what JS let go (spec 2(g)).
	// The same shape as the Models drop below, for the mirrored reason: that one must
	// come AFTER Shutdown() because the context reads the shadows on its way down.
	if (ScriptHost.IsValid())
	{
		ScriptHost->OnViewRemoved(ViewId);
	}

	// The router lets go BEFORE the context dies, and the ordering is a use-after-free
	// guard, not tidiness: a click routed in the PREVIOUS frame's record loop may have a
	// revert-dirty still pending, and the flush runs in ApplyModelUpdates -- AFTER this
	// drain, in the same frame. Unregistering purges those pending entries, so the flush
	// can never DirtyVariable into a DataModel that Shutdown() below is about to destroy.
	FVaCuusWriteRouter::UnregisterViewModels(ViewId);

	// Drops the context and the render-side resources, but not the host: RmlUi
	// still holds a RenderManager keyed on its render interface until
	// Rml::Shutdown() (see RetiredHosts). Every other view keeps running.
	Host->Shutdown();
	RetiredHosts.Add(MoveTemp(Host));

	// AFTER Shutdown(), NOT BEFORE, and the order is the whole point: Shutdown() is what runs
	// Rml::RemoveContext, and until it has, that context's data models still hold raw void*s
	// into these models' UI shadows with no liveness check anywhere (spec 2(b)). Dropping the
	// references first could destroy a shadow the context is about to read while it tears down
	// its element tree.
	//
	// The game thread normally holds the other reference (UVaCuusView's model map), so this is
	// usually a refcount decrement rather than a destruction -- and either way the buffer is
	// only reachable from VaCuus code by then.
	TArray<TSharedRef<FVaCuusBoundModel>> RemovedModels;
	if (Models.RemoveAndCopyValue(ViewId, RemovedModels))
	{
		NumBoundModels.fetch_sub(RemovedModels.Num(), std::memory_order_release);
	}

	UE_LOG(LogVaCuus, Log, TEXT("View %u removed from the UI thread (%d view(s) left, %d model(s) dropped)"),
		ViewId, Hosts.Num(), RemovedModels.Num());
}

void FVaCuusUIThread::BindModel(uint32 ViewId, IVaCuusDocumentHost& Host, const TSharedPtr<FVaCuusBoundModel>& Model)
{
	check(IsInUIThread());

	if (!Model.IsValid())
	{
		UE_LOG(LogVaCuus, Error, TEXT("BindModel for view %u carried no model"), ViewId);
		return;
	}

	Rml::Context* Context = Host.GetContext();
	if (Context == nullptr)
	{
		// A host whose context is gone (mid-shutdown), or one that has none at all. There is
		// nothing to create the model on and no way to tell the game thread -- a BindModel
		// carries no serial -- so this line is the only trace.
		UE_LOG(LogVaCuus, Error, TEXT("View %u has no Rml context; the data model '%s' is not bound and its updates go nowhere"),
			ViewId, *Model->GetModelNameString());
		return;
	}

	if (!Model->BindToContext(*Context))
	{
		// Already logged in detail. NOT registered below: an unbound model has nothing to
		// dirty, and registering it would echo applied generations back for updates that
		// reached no DataModel.
		return;
	}

	Models.FindOrAdd(ViewId).Add(Model.ToSharedRef());
	NumBoundModels.fetch_add(1, std::memory_order_release);

	// The BindModel-drain stamp spec 3.10 asks for, landed as a REGISTRY entry rather
	// than a member on the model: this is the one point that knows the (ViewId, name,
	// model) triple, and from here on a document write into this model's storage
	// attributes to it (FVaCuusWriteRouter's span-walk comment), and JS can read it
	// through vacuus.model(name). Unregistered in RemoveView, before the ref drop.
	FVaCuusWriteRouter::RegisterModel(ViewId, Model->GetModelName(), Model.ToSharedRef());
}

void FVaCuusUIThread::DumpModel(uint32 ViewId, FName ModelName)
{
	check(IsInUIThread());

	const TArray<TSharedRef<FVaCuusBoundModel>>* ViewModels = Models.Find(ViewId);
	if (ViewModels == nullptr)
	{
		// THE MOST INFORMATIVE OUTCOME THIS COMMAND HAS. The game thread has just printed a
		// model it holds; this says the UI thread holds none for that view -- so either the bind
		// command never reached a context (BindModel above logs why, at Error, and this is the
		// line that sends the reader back to look for it) or the view has been retired and the
		// game-side handle has outlived it.
		UE_LOG(LogVaCuus, Display,
			TEXT("DumpModel:   UI thread (view %u): NO MODEL IS REGISTERED FOR THIS VIEW. Either the bind never reached a ")
			TEXT("context -- look for a BindModel error above -- or the view has already been removed"),
			ViewId);
		return;
	}

	int32 NumDumped = 0;
	for (const TSharedRef<FVaCuusBoundModel>& Model : *ViewModels)
	{
		if (ModelName.IsNone() || Model->GetModelName() == ModelName)
		{
			Model->DumpUISide(ViewId);
			++NumDumped;
		}
	}

	if (NumDumped == 0)
	{
		UE_LOG(LogVaCuus, Display,
			TEXT("DumpModel:   UI thread (view %u): %d model(s) are registered, but none is called '%s'"), ViewId,
			ViewModels->Num(), *ModelName.ToString());
	}
}

void FVaCuusUIThread::ClearAssetCaches()
{
	check(IsInUIThread());

	// HAS TO HAPPEN HERE rather than anywhere on the game thread: Rml::Factory keys
	// parsed stylesheets and templates on their file name and hands the same
	// StyleSheetContainer back for a second load, so re-loading an .rml whose .rcss
	// changed would show the OLD colours. RmlUi does exactly this pair itself, in
	// ElementDocument::ReloadStyleSheet (ElementDocument.cpp:296-297).
	//
	// SAFE WITH OTHER DOCUMENTS STILL UP, which is not obvious and is the reason this is
	// one process-wide clear rather than something scoped to the view being reloaded: an
	// ElementDocument never retains the cached StyleSheetContainer*. It merges (or
	// combines) it into a SharedPtr of its own -- ElementDocument.cpp:200-215, member at
	// ElementDocument.h:165 -- so dropping the cache cannot invalidate anything a live
	// document is holding. A future reader tempted to make this per-view should know
	// there is nothing to protect.
	Rml::Factory::ClearStyleSheetCache();
	Rml::Factory::ClearTemplateCache();

	// Textures are deliberately NOT released -- see the note in FVaCuusLiveReload.
	NumAssetCacheClears.fetch_add(1, std::memory_order_release);

	UE_LOG(LogVaCuus, Verbose, TEXT("Dropped the RmlUi stylesheet/template caches (clear #%llu)"),
		NumAssetCacheClears.load(std::memory_order_relaxed));
}

int32 FVaCuusUIThread::DrainAndDiscardCommands()
{
	check(IsInUIThread());

	int32 NumDropped = 0;
	while (Queues->Commands.Dequeue())
	{
		// The dequeued command dies at the end of this iteration -- including, for an
		// AddView, the host it carries, which was never booted and so holds nothing
		// RmlUi-affine.
		++NumDropped;
	}

	return NumDropped;
}

IVaCuusDocumentHost* FVaCuusUIThread::FindHost(uint32 ViewId) const
{
	const TUniquePtr<IVaCuusDocumentHost>* Found = Hosts.Find(ViewId);
	return Found ? Found->Get() : nullptr;
}
