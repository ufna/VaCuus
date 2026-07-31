// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "UObject/Object.h"

#include "VaCuusInteractiveSnapshot.h"
#include "VaCuusJsValue.h"

#include "Templates/SharedPointer.h"

#include "VaCuusView.generated.h"

class FGenericWindow;
class FVaCuusBoundModel;
class FVaCuusImeHandler;
class FVaCuusUIThread;
class ITextInputMethodContext;
class ITextInputMethodSystem;
class UScriptStruct;
class UVaCuusSubsystem;
struct FVaCuusInputEvent;
struct FVaCuusViewStatus;

/**
 * Everything the platform IME needs to know about WHERE a view is drawn, as plain data.
 *
 * WHY IT IS A STRUCT AND NOT AN FGeometry: `ITextInputMethodContext` wants Slate ABSOLUTE
 * (desktop) pixels, RmlUi gives view pixels, and the transform between them belongs to the
 * Slate host -- which is in another module. Passing the two rectangles instead of the
 * geometry keeps VaCuus free of a Slate dependency (every type below is ApplicationCore or
 * Core) and keeps the mapping in one line the host can be held to:
 *
 *     Absolute = AbsolutePosition + ViewPixel * (AbsoluteSize / ViewPixelSize)
 *
 * which is exact for the axis-aligned translate+scale geometry a viewport overlay or a UMG
 * slot ever has, and degenerates safely (no caret) when ViewPixelSize is zero.
 *
 * D17 -- WORLD-SPACE SURFACES MUST NOT SUPPLY ONE. There is no valid Slate-absolute mapping
 * for a UI drawn on a mesh in the world: the "screen position" of a caret on a rotating
 * quad is a projection that changes with the camera, and an IME candidate window anchored to
 * it would chase the player's head. A UVaCuusWorldComponent-style host therefore leaves
 * TextInputMethodMethodSystem null (or simply never calls UVaCuusView::UpdateIme), which
 * degrades to exactly the Linux path -- OnKeyChar -> ProcessTextInput, no composition.
 * M2 ships no world component, so this is a documented constraint rather than code.
 */
struct FVaCuusImeSurface
{
	/**
	 * The platform's text-input method system, or NULL when the platform has none.
	 *
	 * Supplied by the host (FSlateApplication::GetTextInputMethodSystem()) rather than looked
	 * up here, because that accessor is in the Slate module. Null is not an error: it is the
	 * normal state on Linux -- FLinuxApplication never overrides
	 * GenericApplication::GetTextInputMethodSystem() -- and on any world-space host (D17).
	 * Everything downstream then no-ops and typing degrades to the OnKeyChar path.
	 */
	ITextInputMethodSystem* TextInputMethodSystem = nullptr;

	/** Native window the surface is drawn in; what ITextInputMethodContext::GetWindow answers. */
	TSharedPtr<FGenericWindow> NativeWindow;

	/** The host widget's rect in Slate ABSOLUTE (desktop) pixels. */
	FVector2D AbsolutePosition = FVector2D::ZeroVector;
	FVector2D AbsoluteSize = FVector2D::ZeroVector;

	/** The view's own pixel size -- the space RmlUi's coordinates and the snapshot rects use. */
	FIntPoint ViewPixelSize = FIntPoint::ZeroValue;

	/** True while the host holds Slate keyboard focus; false deactivates the context. */
	bool bHostHasFocus = false;
};

/**
 * Broadcast on the GAME thread for the NEWEST completed document load.
 *
 * Latest completion wins. The UI thread can finish several queued loads inside one
 * drain, and the status carries one (serial, result) pair, so the game thread's
 * next poll sees only the last of them: intermediate completions are coalesced away
 * and a superseded load is never reported. That is the honest semantic, and the
 * right one for the only thing the result is used for -- "is the document that is
 * up now the one I last asked for". Callers that need to know their own request was
 * overtaken can compare UVaCuusView::GetLastRequestedLoadSerial() against
 * GetLastCompletedLoadSerial(); coalescing is logged (Verbose) as it happens.
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnVaCuusViewLoadCompleted, UVaCuusView* /*View*/, bool /*bSuccess*/);

/**
 * One routed document write (M4 Task 9, spec 3.10) -- what a data-checked checkbox, a
 * data-value input or a data-event assignment produces once the write router carries it
 * to the game thread. Model is the bound model's name; Path is the wire path within it
 * ("bPaused", "Origin.X", "Killfeed[0].Killer"); Value is the tagged payload
 * (FVaCuusJsValue). The SHADOW WAS NOT WRITTEN and the control has snapped back to it:
 * this delegate is the request, and calling UpdateModel with a changed struct is the
 * grant -- one-way data flow with an explicit ask, not two-way mutation.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FOnVaCuusModelWrite, FName, Model, const FString&, Path, const FVaCuusJsValue&, Value);

/** One `vacuus.emit(name, payload)` from this view's JS (M4 Task 9, spec 3.11). Payload is the flat key/value set the emit carried. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnVaCuusJsEvent, FName, Name, const TArray<FVaCuusJsKeyValue>&, Payload);

/**
 * Handle to one UI view: one Rml::Context living on the process-wide UI thread.
 *
 * Owns nothing thread-affine. Every method here is a non-blocking enqueue onto
 * the UI thread's command queue, stamped with this view's id so the UI thread
 * routes it to the right context; the answers come back through a shared
 * FVaCuusViewStatus that UVaCuusSubsystem::Tick polls (see PollStatus()).
 *
 * Created and destroyed exclusively through UVaCuusSubsystem, which is also this
 * object's outer and which invalidates it (Invalidate()) when the view is gone --
 * after that every call here is a logged no-op, so a widget or console command
 * holding on to the handle cannot reach a dead context.
 *
 * Game thread only.
 */
UCLASS(BlueprintType)
class VACUUS_API UVaCuusView : public UObject
{
	GENERATED_BODY()

public:
	/** Newest completed load only; see the delegate's own comment for the coalescing rule. */
	FOnVaCuusViewLoadCompleted OnLoadCompleted;

	/**
	 * Routed document writes land here, on the game thread, drained once per frame by
	 * UVaCuusSubsystem::Tick from the router's bounded queue. A write arriving with
	 * NOTHING bound logs one Warning per (model, path) -- the control already snapped
	 * back, so an unheard write is otherwise invisible. See FOnVaCuusModelWrite.
	 */
	UPROPERTY(BlueprintAssignable, Category = "VaCuus|Data")
	FOnVaCuusModelWrite OnModelWrite;

	/** `vacuus.emit` events land here; same queue, same drain, no model. */
	UPROPERTY(BlueprintAssignable, Category = "VaCuus|Data")
	FOnVaCuusJsEvent OnJsEvent;

	/**
	 * Called by UVaCuusSubsystem::CreateView() right after the AddView command is
	 * queued. InInitialViewSize is the size that command already carried, so the
	 * first widget Tick does not re-send it.
	 */
	void InitializeView(UVaCuusSubsystem* InSubsystem, uint32 InViewId,
		const TSharedRef<FVaCuusViewStatus>& InStatus, FIntPoint InInitialViewSize);

	/** Called by the subsystem once the view has been removed from the UI thread. */
	void Invalidate();

	/**
	 * Loads a document through the RmlUi file interface (paths resolve against the
	 * ordered DevUI roots -- see VaCuusContentPaths.h). Asynchronous: OnLoadCompleted
	 * reports the outcome.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void LoadDocument(const FString& VfsPath);

	/**
	 * The VFS path of the document this view was last asked to load from FILE, or empty
	 * when there is none (never loaded, loaded from memory, or closed).
	 *
	 * IT IS THE RELOAD KEY, which is the only reason it is remembered at all: live
	 * reload has to re-issue a load without knowing who asked for the first one, and
	 * neither the widget (whose own copy is guarded, see UVaCuusWidget) nor the UI thread
	 * (which keeps an Rml::ElementDocument, not a path) can answer for it.
	 *
	 * THE INVARIANT IS "THIS DESCRIBES WHAT IS SHOWING", which is why
	 * LoadDocumentFromMemory() and Close() both clear it: an inline document has no file
	 * behind it, and reloading it would destroy and rebuild identical content.
	 *
	 * The consequence, stated because it bit us: a view that FELL BACK to an inline
	 * document (vacuus.M1HUD does when its .rml is missing or unparseable) is not
	 * reloadable through this path, and iterating on a broken document is the single most
	 * valuable thing live reload does. That gap is closed one level up rather than by
	 * keeping a lying path here -- UVaCuusSubsystem::OnDocumentsReloadRequested lets the
	 * owner that chose the fallback re-arm it, because only the owner knows which file it
	 * fell back FROM and whether it wants that file back.
	 */
	const FString& GetDocumentPath() const { return DocumentPath; }

	/**
	 * Re-issues the last file load, forcing RmlUi to re-read the document (controller
	 * decision D21's editor live reload, and half of what vacuus.ReloadUI does).
	 *
	 * THE OTHER HALF IS NOT HERE, and the split is deliberate: an RML re-read alone shows
	 * stale CSS, because Rml::Factory keys parsed stylesheets on their file name and hands
	 * the cached one back. Dropping that cache is a PROCESS-WIDE act that must also happen
	 * when no view is live at all (an .rcss edited between PIE sessions), so it belongs to
	 * the dispatcher -- FVaCuusUIThread::EnqueueClearAssetCaches(), which
	 * UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews() enqueues once before fanning out
	 * over the views. Calling this directly, without that clear, re-reads the RML only.
	 *
	 * STILL PUBLIC, unlike the per-instance fan-out that sits behind that entry point, and
	 * the difference is real rather than an inconsistency: "re-read THIS view's document"
	 * is a coherent request an owner can have for its own reasons (the fallback re-arm in
	 * VaCuusRender is one), while "fan out over every view but skip the caches" is only ever
	 * half of the thing the caller meant.
	 *
	 * Returns false when there is nothing to reload (invalid view, or no file document).
	 */
	bool ReloadDocument();

	/** Same, from RML source text. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void LoadDocumentFromMemory(const FString& RmlSource);

	/**
	 * Evaluates Source as JavaScript against this view's context (M4). Fire and
	 * forget like every mutator here: the script runs when the UI thread drains
	 * the command, its errors land in the log (LogVaCuusJS) named after this
	 * view, and a dead view drops the call with a Warning.
	 *
	 * ORDERING IS THE QUEUE'S FIFO: enqueued after a LoadDocument*, the script
	 * runs against that document; enqueued before any load, it runs with
	 * `document === null` (feature-testable, spec 3.4). With JavaScript disabled
	 * (vacuus.Js.Enable 0 at thread boot, or no VaCuusJs module) the drain
	 * refuses it at Error rather than losing it silently.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void ExecuteScript(const FString& Source);

	/**
	 * Closes the current document; the view (and its context) stays alive and can load
	 * another. The view goes blank on the next UI frame -- the one the close still owes,
	 * which clears the render target and is also what makes RmlUi free the document
	 * (IVaCuusDocumentHost::CloseDocument).
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void Close();

	/**
	 * Shows or hides the document. A hidden view still records (empty) frames, which
	 * is what actually clears it from the screen.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void SetVisible(bool bVisible);

	/** True while this handle still refers to a live view. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	bool IsViewValid() const;

	/** Queues a re-layout at this pixel size. Only sent when the size actually changed. */
	void Resize(FIntPoint ViewSize);

	//~ ------------------------------------------------------------------ Data binding (M3a)
	//~
	//~ THREE CONTRACTS GOVERN EVERY CALL BELOW, and each is RmlUi's rather than ours.
	//~
	//~ 1. BIND BEFORE LoadDocument. `data-model` is read exactly once, when the document's
	//~    body is parented into the context -- Element::SetParent looks the attribute up in
	//~    Context::GetDataModelPtr and, failing to find it, logs Log::LT_ERROR and moves on
	//~    (Element.cpp:2202-2219). There is no retry and no second lookup. So a model created
	//~    after its document loaded attaches to nothing and every `{{Field}}` in that document
	//~    resolves against no model.
	//~
	//~    THE LT_ERROR IS REAL OUTPUT, not a compiled-out one: Log::Message forwards to the
	//~    registered SystemInterface unconditionally (Log.cpp:11-31) and ours routes LT_ERROR to
	//~    UE_LOG(LogVaCuus, Error) -- FVaCuusSystemInterface::LogMessage carries the full
	//~    argument and the list of what genuinely is compiled out. What that line cannot say is
	//~    that a BIND was missing, because from RmlUi's side nothing was ever asked for; so
	//~    BindModel() also warns when it can see a load has already been requested on this view,
	//~    which is the closest thing to an up-front check there is.
	//~
	//~ 2. THERE IS NO UNBIND, AND THERE WILL NOT BE. RmlUi has no API for it: the only
	//~    teardown is Context::RemoveDataModel, and that is a one-way door -- it calls
	//~    Element::SetDataModel(nullptr) on every attached root (Context.cpp:1097-1111), and
	//~    SetDataModel is PRIVATE (Element.h:662, and SetParent is the only caller), so a
	//~    document that is still loaded is permanently detached with no way to re-attach it.
	//~    Re-binding the same name instead is refused: Context::CreateDataModel returns a
	//~    falsy constructor (Context.cpp:1074-1075) and the existing model keeps pointing at
	//~    the OLD shadow. A model therefore lives as long as its view, and dies with the
	//~    context.
	//~
	//~ 3. ON DOCUMENT RELOAD, DO NOTHING. The context, the model, its variables and the
	//~    values in the UI shadow all survive a load -- only element-attached DataViews are
	//~    rebuilt, and RmlUi re-initialises those itself (Context::LoadDocument runs
	//~    DataModel::Update over every model right after the load event, Context.cpp:305-306,
	//~    and DataViews::Update updates every newly added view unconditionally,
	//~    DataView.cpp:76-88). VaCuus loads the new document BEFORE closing the old one
	//~    (FVaCuusRmlDocumentHost::AdoptDocument), so the new one attaches while the model is
	//~    fully live. Tearing the model down around a reload would race the load, and by
	//~    contract 2 it could not be put back.
	//~
	//~ Game thread only, like every other mutator here.

	/**
	 * Creates a data model called ModelName over Type on this view's RmlUi context, and binds
	 * one variable per bindable top-level property of Type to a UI-thread-owned shadow of it.
	 *
	 * Asynchronous, like every other call here: the model is created when the UI thread drains
	 * the command, which -- the queue being FIFO from one producer -- is before any
	 * LoadDocument enqueued after this. That ordering is contract 1 above.
	 *
	 * Type's properties are walked by FVaCuusModelLayout, which logs one line per property it
	 * cannot bind (unsupported kind, RmlUi-illegal wire name, deprecated, editor-only). A
	 * struct with no bindable property still gets an empty model, because what `data-model`
	 * needs to resolve is the MODEL.
	 *
	 * @return false, with an Error or Warning saying why, for: a null Type, an unnamed model,
	 *         a second model of the same name on this view, a type whose layout could not be
	 *         built, or a view that is no longer registered. Never partially succeeds.
	 */
	bool BindModel(FName ModelName, const UScriptStruct* Type);

	/**
	 * Reads Data through ModelName's layout, marks whatever changed since the last call, and
	 * leaves it for the next UVaCuusSubsystem::Tick to publish. Cheap and allocation-free in
	 * the steady state; call it every frame.
	 *
	 * THE TYPE IS A PARAMETER, AND IT IS NOT CEREMONY. An untyped (FName, const void*) makes
	 * "wrong type" and "wrong size" undiagnosable: both read whatever happens to sit at the
	 * layout's offsets, and the first FString field turns that into a crash rather than a
	 * wrong number. The Blueprint node gets the type for free from its wildcard pin, so both
	 * surfaces reach this same check.
	 *
	 * A MODEL CONTAINING FText MUST BE RE-PUSHED AFTER A CULTURE CHANGE, OR PUSHED EVERY FRAME.
	 * That is a contract, not the recommendation above. An FText field is stored in the shadow
	 * as FText::AsCultureInvariant(Live.ToString()) -- resolved once, HERE, on the game thread --
	 * so that the UI thread never touches FTextLocalizationManager (FVaCuusModelSampler's header
	 * carries that argument; it is what makes the design thread-safe at all). A projected text
	 * has an empty TextId, so FTextHistory_Base::CanUpdateDisplayString returns false
	 * (TextHistory.cpp:940-943) and it can NEVER re-resolve itself.
	 *
	 * A culture change is therefore invisible until the next call to this function. Push a
	 * struct of FText labels once when a settings menu opens, let the player change language,
	 * and every label stays in the old language for the life of the view -- with no log line,
	 * because nothing on this path is wrong. Hang a re-push off
	 * FInternationalization::Get().OnCultureChanged() (Internationalization.h:201), or just call
	 * this every frame as everything else here assumes.
	 *
	 * Nothing is written, and one line is logged, for: a model that is not bound (Warning), a
	 * type that is not the bound one (Error), or a null Data (Warning). A view that has been
	 * invalidated drops the call at Verbose -- this runs at frame rate, and a louder level
	 * would bury the log during teardown, exactly as SendInput() already argues.
	 */
	void UpdateModel(FName ModelName, const UScriptStruct* Type, const void* Data);

	/** True while a model of that name is bound to this view. Game thread. */
	bool HasModel(FName ModelName) const;

	/**
	 * Fields of ModelName the UI thread has not confirmed applying, or INDEX_NONE when no such
	 * model is bound.
	 *
	 * The API's only observable, and the diagnostic that tells the two silent failures apart:
	 * a number that climbs and never falls means the model reached no context (nothing is
	 * consuming the channel), while a steady 0 means the UI is keeping up. Harvests the echo,
	 * so it is not const.
	 */
	int32 NumOutstandingModelFields(FName ModelName);

	/**
	 * `vacuus.DumpModel`'s per-view body (spec 8): prints the layout, the game-side shadow and
	 * the pending/unacknowledged sets for ModelName now, and enqueues the UI thread's half --
	 * the UI shadow, the published dirty set and the applied generation -- which appears in the
	 * log a UI frame later.
	 *
	 * TWO HALVES ON TWO THREADS, because the UI shadow is a UScriptStruct instance the UI thread
	 * writes with no synchronisation at all: an FString field in it is freed and reallocated by
	 * every apply that touches it, so reading one from here would be a use-after-free rather
	 * than a stale number. FVaCuusUIThread::EnqueueDumpModel carries the request across.
	 *
	 * ModelName None dumps every model bound to this view.
	 *
	 * @return how many models were dumped from THIS side. Zero means nothing is bound under that
	 *         name on the game thread, which is a different failure from the UI thread having
	 *         nothing -- and the two lines say so separately.
	 */
	int32 DumpModel(FName ModelName);

	/**
	 * Publishes every bound model's outstanding fields to the UI thread. Once per frame, from
	 * UVaCuusSubsystem::Tick.
	 *
	 * SEPARATE FROM UpdateModel(), AND DRIVEN FROM THE SUBSYSTEM, for two reasons. It
	 * coalesces: several UpdateModel calls in one frame -- several actors each updating their
	 * own field -- become one publish and one triple-buffer swap rather than N. And it lands
	 * inside the existing GameTick perf scope (VaCuusSubsystem.cpp), where spec 9's
	 * game-thread budget is measured; from an actor tick it would be outside every scope.
	 */
	void PublishModelUpdates();

	/**
	 * Blueprint's BindModel: the wildcard struct pin supplies the UScriptStruct, so a designer
	 * cannot get the type wrong and C++ and Blueprint reach the same check.
	 *
	 * CustomThunk because a wildcard pin has no C++ signature -- `const int32&` is the
	 * engine's placeholder for one, and execCreateModelFromStruct reads the real
	 * FStructProperty off the script stack. The declared function below is never called; the
	 * thunk replaces it.
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "VaCuus|Data",
		meta = (CustomStructureParam = "Struct", DisplayName = "Create Model From Struct"))
	bool CreateModelFromStruct(FName ModelName, const int32& Struct);
	DECLARE_FUNCTION(execCreateModelFromStruct);

	/** Blueprint's UpdateModel; same wildcard pin, same type check. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "VaCuus|Data",
		meta = (CustomStructureParam = "Struct", DisplayName = "Update Whole Model"))
	void UpdateWholeModel(FName ModelName, const int32& Struct);
	DECLARE_FUNCTION(execUpdateWholeModel);

	/**
	 * Queues one input event for this view's context. Never blocks, never fails: a
	 * dead view drops it.
	 *
	 * The single choke point for input on the game thread, which is also why the
	 * frame-ordering diagnostic below lives here rather than in the widget.
	 */
	void SendInput(const FVaCuusInputEvent& Event);

	uint32 GetViewId() const { return ViewId; }

	/**
	 * How many input events this handle has queued for the UI thread.
	 *
	 * This is the ONLY observable that can answer "was that event forwarded at all",
	 * and one decision needs it: controller decision D12's pass-through key set is
	 * defined as keys the widget neither consumes nor enqueues, and an event that was
	 * dropped on the game thread leaves no other trace anywhere. Counted here rather
	 * than in the widget because this is the single choke point every event goes
	 * through, widget or not.
	 *
	 * Counts events ACCEPTED for the queue: an event dropped because the view is already
	 * invalid does not increment it.
	 */
	uint64 GetNumInputEventsQueued() const { return NumInputEventsQueued; }

	/**
	 * Frames this view's document host has RECORDED. Useful for headless waits.
	 *
	 * Not "published": since the idle short-circuit (M2 Task 12) a frame that draws
	 * what the render thread already has is recorded and then withheld, so a static
	 * document publishes a handful of times and records forever. See
	 * FVaCuusViewStatus::FramesRecorded for why a waiter wants the recording count.
	 */
	uint64 GetFramesRecorded() const;

	/**
	 * Frames this view has PUBLISHED to the render thread, i.e. those the idle short-circuit
	 * let through. Never above GetFramesRecorded().
	 *
	 * READ IT AGAINST GetFramesRecorded(): once the gate starts firing the two diverge, and
	 * that divergence is this view's idle signal -- the only per-view one there is. A view
	 * whose recorded count climbs while its published count sits still is idle and its
	 * picture is living in the render target; a view where both sit still is not running
	 * frames at all, which is a different problem. vacuus.M1HUD.PerfLog answers the same
	 * question for the whole process at once and cannot separate two views.
	 *
	 * NOT a substitute for GetFramesRecorded() in a headless wait: on a static document this
	 * number stops advancing after a handful of frames, so a threshold on it never completes.
	 */
	uint64 GetFramesPublished() const;

	/**
	 * Serial of the newest load this view has ASKED for, and of the newest one the UI
	 * thread has FINISHED. Equal means the view is showing the answer to the last
	 * request; a completed serial above your own request's means yours was
	 * superseded and its result was never reported (see the delegate comment).
	 * Serials start at 1; 0 means "none yet".
	 */
	uint64 GetLastRequestedLoadSerial() const;
	uint64 GetLastCompletedLoadSerial() const;

	/** True while a queued load has not been answered yet. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	bool IsLoadPending() const;

	/**
	 * Where this view is interactive, as of the UI thread's newest published frame.
	 *
	 * THE CONTRACT SLATE NEEDS: this is a plain value type owned by this handle, so
	 * `Snapshot.Contains(P)` is a synchronous, lock-free, never-failing answer to
	 * "does the UI want this pointer event" -- which is what lets SVaCuusWidget
	 * return Handled/Unhandled without ever asking the UI thread anything.
	 *
	 * LIFETIME: the reference is to this object's own cache and stays valid until the
	 * NEXT PollStatus(), i.e. for the rest of the game frame. That is the guarantee
	 * that matters -- ONE STABLE SNAPSHOT PER DISPATCH BATCH, AT MOST ONE FRAME OLD --
	 * because it is what keeps the answer from changing between a mouse-down and the
	 * mouse-up that releases its capture. Reading the triple buffer per event would
	 * not.
	 *
	 * THE ACTUAL ORDERING, measured rather than assumed (bead VaCuus-akj.6.13): input
	 * is dispatched BEFORE this cache is refreshed, so a handler sees the snapshot
	 * polled in the PREVIOUS game frame. On Linux, SDL events are processed
	 * synchronously inside FPlatformApplicationMisc::PumpMessages
	 * (LaunchEngineLoop.cpp:5784) -- FLinuxApplication::AddPendingEvent only defers
	 * when GPumpingMessagesOutsideOfMainLoop is set, and that flag is set on Windows
	 * only (WindowsPlatformMisc.cpp:4439) -- while PollStatus() runs from
	 * UVaCuusSubsystem::Tick inside GEngine->Tick (LaunchEngineLoop.cpp:5859), 75
	 * lines later. So the ordering is: dispatch input (frame N) -> refresh cache
	 * (frame N) -> dispatch input (frame N+1). One frame of staleness, and the
	 * per-frame stability above is unaffected.
	 *
	 * Before the first published frame this is the default snapshot: Generation 0,
	 * no rects, nothing interactive.
	 */
	const FVaCuusInteractiveSnapshot& GetSnapshot() const { return CachedSnapshot; }

	//~ IME (Task 9). The Slate host's whole interface to the platform text-input system:
	//~ everything RmlUi-shaped and everything ITextInputMethodContext-shaped stays behind
	//~ these three calls. Game thread only.

	/**
	 * Tells the IME where this view is drawn and whether the host has keyboard focus (see
	 * FVaCuusImeSurface). Called once per game frame by the host; cheap when nothing moved,
	 * and a complete no-op on a platform with no IME system.
	 *
	 * The FIRST call is what creates the handler, so a view nobody hosts in Slate never
	 * builds an IME context at all -- which is the right answer for a headless view and for a
	 * world-space one (D17).
	 */
	void UpdateIme(const FVaCuusImeSurface& Surface);

	/**
	 * Controller decision D14a: a press landed on a rect flagged
	 * EVaCuusRectFlags::TextInput, so activate the platform IME context on THIS click
	 * instead of waiting for the snapshot that will confirm it a frame later. Without this
	 * the player's first composition is lost -- the same bug D11 fixed for Slate focus.
	 */
	void NotifyImeTextInputClicked();

	/**
	 * Deactivates, unregisters and forgets the IME context NOW (controller decision D18).
	 *
	 * Called from every teardown site rather than left to destruction: the platform system
	 * holds the context by TSharedRef, so a context that outlives its Slate widget keeps the
	 * OS pointing at a window that is gone.
	 */
	void DetachIme();

	/**
	 * What the IME bridge is doing right now.
	 *
	 * ONE STRUCT RATHER THAN FOUR ACCESSORS, and it is deliberately on the VIEW: the handler and
	 * the context are both declared in a PRIVATE header (VaCuusTextInput.h), so a caller in
	 * another module -- VaCuus.Input.TextEntry lives in VaCuusRender, next to the widget whose
	 * OnKeyChar it drives -- cannot name their types at all. The view is already the facade for
	 * everything else about a view; being the facade for this too is what keeps the IME
	 * implementation private without making it untestable.
	 */
	struct FImeStatus
	{
		/** False until the first UpdateIme(): a view nobody hosts in Slate has no bridge. */
		bool bHandlerBuilt = false;

		/** True where the platform offers no ITextInputMethodSystem -- the Linux path (D16). */
		bool bPlatformImeAbsent = true;

		/** True once the context has been handed to the platform system. Never true on Linux. */
		bool bRegistered = false;

		/** True while the platform reports OUR context as the active one. */
		bool bContextActive = false;
	};

	FImeStatus GetImeStatus() const;

	/**
	 * The platform-facing context, so a test can drive the 14 ITextInputMethodContext virtuals
	 * the way the OS would. Null before the first UpdateIme().
	 *
	 * NON-NULL EVEN WITHOUT A PLATFORM IME, which is the point: answering those virtuals needs
	 * only the shadow state and the surface, so the whole game-thread half of the IME is built
	 * and observable on a platform that will never call it (D16).
	 */
	ITextInputMethodContext* GetImeContextForTesting() const;

	/**
	 * Once per game frame, from UVaCuusSubsystem::Tick: refreshes the snapshot cache
	 * and turns the UI thread's newest load result into an OnLoadCompleted
	 * broadcast. Both halves exist to keep everything the UI thread publishes on the
	 * game thread by the time anyone reads it.
	 */
	void PollStatus();

	//~ Begin UObject
	/** Last-chance IME teardown; the explicit DetachIme() sites are the intended ones (D18). */
	virtual void BeginDestroy() override;
	//~ End UObject

private:
	/** Copies the newest published snapshot into CachedSnapshot, if there is a newer one. */
	void RefreshSnapshot();

	/**
	 * Logs the one line of evidence for the ordering claim on GetSnapshot(): what the
	 * frame's first input event saw versus what this frame's poll produced. Once per
	 * session, Verbose -- it answers a design question, not a per-frame one.
	 */
	void LogFrameOrderingOnce();

	/** The UI thread through the subsystem, or null once invalidated / after module shutdown. */
	FVaCuusUIThread* GetUIThread() const;

	/** The owner; also this object's outer. Weak so module/world teardown order cannot bite. */
	UPROPERTY(Transient)
	TWeakObjectPtr<UVaCuusSubsystem> OwningSubsystem;

	/** Shared with this view's document host on the UI thread. */
	TSharedPtr<FVaCuusViewStatus> Status;

	/**
	 * Data models bound to this view, by the name a document's `data-model` writes.
	 *
	 * SHARED WITH THE UI THREAD, which holds its own reference per view and drops it in
	 * RemoveView() -- after the context that points into each model's shadow is gone.
	 *
	 * NOT CLEARED BY Invalidate(), deliberately: it is what lets UpdateModel() tell "this view
	 * is dead" from "you never bound that model", which are different mistakes with different
	 * fixes. Nothing publishes through it once the view is unregistered
	 * (PublishModelUpdates() returns early), and the entries die with this object.
	 */
	TMap<FName, TSharedPtr<FVaCuusBoundModel>> Models;

	/**
	 * The platform IME bridge, created by the first UpdateIme() and destroyed by DetachIme().
	 *
	 * TSharedPtr because ITextInputMethodSystem::RegisterContext takes the CONTEXT by
	 * TSharedRef and the handler is what owns that context -- the platform can outlive our
	 * intent to keep it, which is precisely why Shutdown() is called explicitly.
	 */
	TSharedPtr<FVaCuusImeHandler> ImeHandler;

	/**
	 * Game-thread-owned copy of the newest snapshot the UI thread published.
	 *
	 * A COPY, not a reference into the triple buffer, for two reasons: the buffer's
	 * read reference dies at the next swap (TripleBuffer.h's own warning), and a
	 * per-frame snapshot must not change under Slate's feet mid-frame. Refreshed
	 * only when Generation moved, so an idle UI costs one integer comparison per
	 * frame and the TArray keeps its allocation.
	 */
	FVaCuusInteractiveSnapshot CachedSnapshot;

	/** GFrameCounter when CachedSnapshot last took a NEW generation. */
	uint64 SnapshotCachedOnFrame = 0;

	//~ Frame-ordering evidence for bead VaCuus-akj.6.13, logged once per session
	//~ (Verbose) the first time input and a poll happen in the same frame. Recording
	//~ what the FIRST event of a frame saw, because that is the one whose staleness
	//~ the ordering claim is about.

	/** GFrameCounter of the frame whose first input event is recorded below. */
	uint64 InputObservedFrame = 0;

	/** Snapshot generation that first event answered from, and when it was cached. */
	uint64 InputObservedGeneration = 0;
	uint64 InputObservedCachedOnFrame = 0;

	/** Set by SendInput(), consumed by the next PollStatus() in the same frame. */
	bool bInputOrderingLogPending = false;
	bool bLoggedFrameOrdering = false;

	/** Process-unique; allocated by the UI thread and stamped into every command. */
	uint32 ViewId = 0;

	/** Cleared by Invalidate(); gates every enqueue. */
	bool bRegistered = false;

	/** Backs GetNumInputEventsQueued(); game thread only, like everything else here. */
	uint64 NumInputEventsQueued = 0;

	/** Backs GetDocumentPath(); see there for why it is only ever a FILE path. */
	FString DocumentPath;

	/** Next load's serial. Strictly increasing, so a stale completion cannot be mistaken for a new one. */
	uint64 NextLoadSerial = 1;

	/** Newest completion already broadcast. */
	uint64 LastBroadcastLoadSerial = 0;

	/** Last size pushed to the UI thread; resize commands are only sent on change. */
	FIntPoint LastViewSize = FIntPoint::ZeroValue;
};
