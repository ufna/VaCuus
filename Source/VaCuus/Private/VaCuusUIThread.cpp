// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusUIThread.h"

#include "VaCuusDefines.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusInputMap.h"
#include "VaCuusUIQueues.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>

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
 * Stack size for the UI thread, chosen rather than inherited: RmlUi's layout and
 * style resolution recurse with the document tree and QuickJS lands on this
 * thread in M4, so the platform default is not obviously enough -- while the
 * default 8 MB per thread is far more than a UI tree needs. Unix clamps any
 * non-zero request up to at least 128 KB (UnixPlatformRunnableThread.cpp), so
 * this value survives as given.
 */
constexpr uint32 GVaCuusUIThreadStackSize = 512 * 1024;

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
			// inherited and defaults to auto (StyleSheetSpecification.cpp:375). So ANY plain
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

		case EVaCuusInputEventKind::None:
		default:
			UE_LOG(LogVaCuus, Error, TEXT("An input event for view %u reached the drain with no kind set; dropped"),
				Event.ViewId);
			break;
	}
}
}	 // namespace

FVaCuusUIThread::FVaCuusUIThread(FVaCuusEngine& InEngine)
	: Engine(InEngine)
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

	// Everything Init() does -- publishing the thread id, booting RmlUi -- happens
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

void FVaCuusUIThread::EnqueueShutdown()
{
	FVaCuusUICommand Command;
	Command.Kind = EVaCuusCommandKind::Shutdown;
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
		UE_LOG(LogVaCuus, Verbose, TEXT("UI command dropped: the UI thread is stopping"));
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

bool FVaCuusUIThread::Init()
{
	// Runs on the worker thread (or, in inline mode, on the game thread inside the
	// inline scope). FRunnableThread::Create() waits for this to return, so
	// everything published here is visible to the caller once Start() succeeds.
	const uint32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
	ThreadId.store(CurrentThreadId, std::memory_order_release);
	GVaCuusUIThreadId.store(CurrentThreadId, std::memory_order_release);

	// RmlUi is process-global, so it boots here once for every view that will ever
	// exist -- and this thread becomes its owner. Refusing rather than asserting
	// when somebody else already owns it keeps an automation test that holds RmlUi
	// on its own thread from turning into a check() crash.
	const bool bBooted = Engine.IsClaimableOnThisThread() && Engine.Initialize();
	if (!bBooted)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("The VaCuus UI thread could not boot RmlUi (already owned by another thread?); no UI frames will run"));

		// Exit() will NOT run when Init() fails, so this is the only chance to
		// unwind. All there is to retract is the thread-id publication.
		GVaCuusUIThreadId.store(0, std::memory_order_release);
		ThreadId.store(0, std::memory_order_release);

		bInitSucceeded.store(false, std::memory_order_release);
		return false;
	}

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

	// 1. Every view lets go of its documents and its context (and releases its
	// render-side resources), still on this thread.
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

	// 3. Only now may the hosts themselves die, on the thread that built them.
	Hosts.Empty();
	RetiredHosts.Empty();

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
	DrainCommands();
	DrainInput();
	// (data snapshots: M3)

	// One recorded frame per view, each publishing its own command buffer straight to
	// the render thread and its own interactive-region snapshot straight to the game
	// thread's view handle -- no game-thread hop in either direction.
	for (TPair<uint32, TUniquePtr<IVaCuusDocumentHost>>& Pair : Hosts)
	{
		if (Pair.Value->HasView())
		{
			Pair.Value->RecordAndPublishFrame();
		}
	}
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
			// teardown.
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

		IVaCuusDocumentHost* Host = FindHost(Command->ViewId);
		if (Host == nullptr)
		{
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

	// Drops the context and the render-side resources, but not the host: RmlUi
	// still holds a RenderManager keyed on its render interface until
	// Rml::Shutdown() (see RetiredHosts). Every other view keeps running.
	Host->Shutdown();
	RetiredHosts.Add(MoveTemp(Host));

	UE_LOG(LogVaCuus, Log, TEXT("View %u removed from the UI thread (%d view(s) left)"), ViewId, Hosts.Num());
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
