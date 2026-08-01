// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Templates/UniquePtr.h"

#include "VaCuusSubsystem.generated.h"

class FVaCuusUIThread;
class IVaCuusDocumentHost;
class UVaCuusStyleSet;
class UVaCuusView;

/**
 * "A reload was asked for; views that cannot reload themselves get one chance now."
 *
 * WHY IT CARRIES AN ACCUMULATOR: the flush's own log line reports how many views a
 * reload reached, and a re-arm done in here IS one of them -- reporting "reloaded 0
 * view(s)" while a subscriber just re-issued a load would make the one diagnostic live
 * reload has lie. Add one per view you loaded.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnVaCuusDocumentsReloadRequested, int32& /*InOutNumReloaded*/);

/**
 * Per-GameInstance owner of VaCuus views, and the once-per-frame pulse that
 * drives the UI thread.
 *
 * WHAT IT OWNS (and what it does not): the views its game instance created --
 * NOT the UI thread. That thread is process-wide and belongs to FVaCuusModule,
 * because RmlUi's library state (interfaces, `initialised`, the context registry)
 * is a set of process-global statics; contexts, by contrast, are per-view objects
 * inside that global state. So multi-PIE is N subsystems, 1 UI thread, N views by
 * construction, and Deinitialize() destroys only this instance's views while
 * every other client keeps rendering. (This supersedes spec §4's original
 * "one FVaCuusUIThread per UVaCuusSubsystem".)
 *
 * WHY FTickableGameObject: UGameInstanceSubsystem has no Tick of its own and 5.8
 * has no UTickableGameInstanceSubsystem, so this copies the
 * UTickableWorldSubsystem shape (construct with ETickableTickType::Never --
 * UObjects can be constructed off the game thread -- and enable in Initialize()).
 * FTSTicker is the wrong hook: it fires at the very end of FEngineLoop::Tick,
 * after Slate has already drawn.
 */
UCLASS()
class VACUUS_API UVaCuusSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	UVaCuusSubsystem();

	//~ Begin USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem

	//~ Begin FTickableGameObject
	/** Polls view status, then wakes the UI thread for exactly one frame. Never blocks. */
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override;
	virtual bool IsTickable() const override;
	/** UI must keep animating while the game is paused. */
	virtual bool IsTickableWhenPaused() const override { return true; }
	/** This instance's world, so the engine ticks us exactly once per frame even with several PIE clients. */
	virtual UWorld* GetTickableGameObjectWorld() const override;
	//~ End FTickableGameObject

	/**
	 * Creates a view: allocates a process-unique id, hands the document host over to
	 * the UI thread (starting it if this is the first view in the process) and
	 * returns the handle to talk to it with.
	 *
	 * The host comes from the caller because building one needs the render-side
	 * pieces (the Slate element), which live in VaCuusRender -- a module that
	 * depends on this one. Returns null if the UI thread is unavailable.
	 */
	UVaCuusView* CreateView(TUniquePtr<IVaCuusDocumentHost> Host, FIntPoint InitialViewSize);

	/** Retires the view on the UI thread and invalidates the handle. Safe with a stale handle. */
	void DestroyView(UVaCuusView* View);

	/**
	 * THE ONE DOOR FOR A WHOLE RELOAD, and it is process-wide: drop RmlUi's parsed
	 * stylesheet/template caches, then re-issue the last file load on every view of every
	 * game instance in this process. Returns how many views were reloaded. Game thread.
	 *
	 * BOTH HALVES OR NEITHER, WHICH IS WHY THIS IS A FUNCTION AND NOT A COMMENT. M2 shipped
	 * the RML half on its own and an .rcss edit silently did nothing: Rml::Factory keys
	 * parsed stylesheets and templates on FILE NAME, in process-global statics that outlive
	 * a PIE session (Deinitialize() deliberately leaves the UI thread running), so a re-read
	 * takes the previous session's stylesheet back. Dropping those caches therefore has to
	 * happen even when the fan-out reaches ZERO views -- "stop PIE, edit the .rcss, press
	 * Play" is the case -- so it can never be a step inside a per-view or per-instance
	 * reload. It is FVaCuusUIThread::EnqueueClearAssetCaches(), enqueued ONCE, first.
	 *
	 * The per-instance fan-out is PRIVATE so that pairing cannot be skipped: this static is
	 * a member, so it reaches the private fan-out without friendship, and nothing outside
	 * the class can. A runtime reload hook -- M3's data binding, a gameplay debug command --
	 * gets the clear whether or not its author knew there was one to get. (The same warning
	 * at the view level is on UVaCuusView::ReloadDocument(), which is public because a
	 * caller who wants ONE view re-read and nothing else is asking a coherent question.)
	 *
	 * Reason is diagnostic only: it names the trigger in this call's log line ("file
	 * change", "vacuus.ReloadUI", ...).
	 */
	static int32 ClearAssetCachesAndReloadAllViews(const TCHAR* Reason);

	/**
	 * `vacuus.DumpModel`'s walk: prints the model diagnostic (spec 8) for every view of every
	 * game instance in this process that matches. ViewId 0 means every view, ModelName None
	 * means every model. Returns how many models were dumped. Game thread.
	 *
	 * STATIC AND HERE FOR THE REASON ClearAssetCachesAndReloadAllViews() IS: Views is private
	 * and stays private, a static member reaches it without friendship, and a console command
	 * has no other way to find a view -- the alternative, iterating every UObject of the class,
	 * would also pick up the CDO and views belonging to a game instance that is tearing down.
	 * The world-context walk is the same one that entry point uses and for the same reason:
	 * GEditor's PIE accessors see instance 0 only.
	 *
	 * PRINTS A HEADER EVEN WHEN NOTHING MATCHES. A diagnostic command that answers nothing is
	 * indistinguishable from one that did not run, which is the exact confusion this milestone's
	 * failure mode already creates.
	 */
	static int32 DumpModels(uint32 ViewId, FName ModelName);

	/**
	 * Broadcast at the END of every per-instance fan-out (see ReloadAllDocuments below),
	 * for owners of views that the fan-out could not reach.
	 *
	 * THE CASE IT EXISTS FOR is a view showing an INLINE FALLBACK: its DocumentPath is
	 * empty by design (see UVaCuusView::GetDocumentPath), so ReloadDocument() refuses it,
	 * and without this hook "create the .rml the HUD complained about, save it" would
	 * never reach the screen -- only toggling the HUD off and on would. vacuus.M1HUD
	 * subscribes while it is on its inline document and re-arms the file load from here.
	 *
	 * At the end rather than the start so a subscriber's load is not immediately followed
	 * by the loop reloading the same view a second time.
	 */
	FOnVaCuusDocumentsReloadRequested OnDocumentsReloadRequested;

	/**
	 * Registers a material-decorator style set (M5 Task 5b): its keys become resolvable
	 * from RCSS as `decorator: shader(<key>)`. Game thread. Returns how many entries were
	 * accepted; each refusal — wrong domain, scene-texture/VT sampling, key collision —
	 * has logged its own named Error (see FVaCuusStyleRegistry::RegisterStyleSet).
	 *
	 * A THIN DOOR TO PROCESS-WIDE STATE, like the UI thread itself: the registry is one
	 * per process (there is one RmlUi and one recorder contract), so registering here
	 * makes the keys visible to every view of every game instance. The subsystem carries
	 * the API because game code and Blueprint reach the plugin through it.
	 *
	 * Register BEFORE loading documents that use the keys: a `shader(<key>)` compiled
	 * before its key was registered is refused per element and stays refused until the
	 * element restyles or the document reloads (RmlUi caches decorator failure).
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	int32 RegisterStyleSet(UVaCuusStyleSet* StyleSet);

	/**
	 * Unregisters a style set. Live draws naming its keys skip with a latched log; the
	 * materials stay rooted until the render thread provably stopped resolving them
	 * (the deferred-release fence, drained by Tick). Game thread.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void UnregisterStyleSet(UVaCuusStyleSet* StyleSet);

	/** The process-wide UI thread, or null if none is running. Does not start one. */
	FVaCuusUIThread* GetUIThread() const;

private:
	/**
	 * Re-issues the last file load on every view of THIS game instance that has one, then
	 * broadcasts OnDocumentsReloadRequested. Returns how many views were reloaded.
	 * See UVaCuusView::ReloadDocument().
	 *
	 * THE FAN-OUT LIVES HERE, not in the editor watcher (controller decision D21): the
	 * watcher has a changed FILE and no way to find views, and it must not become the thing
	 * that keeps a registry of them. Views is that registry, it is private, and keeping the
	 * fan-out beside it keeps "which views does a reload reach" answerable in one place.
	 *
	 * PRIVATE BECAUSE IT IS HALF A RELOAD -- the RML re-read without the process-wide cache
	 * drop, which is the bug M2 shipped once. ClearAssetCachesAndReloadAllViews() is the
	 * only caller and is the only thing that can be: it is a static member of this class, so
	 * it needs no friendship, and nothing outside the class has access. Do not make this
	 * public "for a caller who only wants one game instance" -- that caller still needs the
	 * clear, so what it wants is a second correctly-paired door, not this one.
	 */
	int32 ReloadAllDocuments();

	/** Views created by this game instance; dropped in Deinitialize(). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UVaCuusView>> Views;

	/** Set in Initialize(); gates ticking exactly like UTickableWorldSubsystem's own flag. */
	bool bInitialized = false;
};
