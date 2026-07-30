// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Templates/UniquePtr.h"

#include "VaCuusSubsystem.generated.h"

class FVaCuusUIThread;
class IVaCuusDocumentHost;
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
	 * Re-issues the last file load on every view of this game instance that has one.
	 * Returns how many views were reloaded. Game thread. See UVaCuusView::ReloadDocument().
	 *
	 * THE FAN-OUT LIVES HERE, not in the editor watcher (controller decision D21): the
	 * watcher has a changed FILE and no way to find views, and it must not become the thing
	 * that keeps a registry of them. Views is that registry and it is private, so this is
	 * the one door -- which also keeps "which views does a reload reach" answerable in one
	 * place rather than at every call site.
	 */
	int32 ReloadAllDocuments();

	/**
	 * Broadcast at the END of every ReloadAllDocuments() fan-out, for owners of views that
	 * the fan-out could not reach.
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

	/** The process-wide UI thread, or null if none is running. Does not start one. */
	FVaCuusUIThread* GetUIThread() const;

private:
	/** Views created by this game instance; dropped in Deinitialize(). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UVaCuusView>> Views;

	/** Set in Initialize(); gates ticking exactly like UTickableWorldSubsystem's own flag. */
	bool bInitialized = false;
};
