// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Templates/UniquePtr.h"

#include "VaCuusSubsystem.generated.h"

class FVaCuusUIThread;
class IVaCuusDocumentHost;
class UScriptStruct;
class UStringTable;
class UVaCuusBundle;
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
 * The Blueprint-facing half of FOnVaCuusTranslationTableChanged (VaCuusTranslation.h, which
 * carries the whole argument for why this signal exists). Forwarded, not re-implemented, so
 * a C++ caller reaching FVaCuusTranslationRegistry::SetTable directly cannot change the
 * language behind Blueprint's back.
 *
 * Version is int64 because dynamic delegates have no uint64 property type; the registry's
 * version is monotonic from 1 and will not reach the sign bit in any process's lifetime.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnVaCuusTranslationTableChangedBP, const FString&, Tag, int64, Version);

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
	 *
	 * THIS IS AN EXTENSION SEAM, NOT THE WAY TO PUT A DOCUMENT ON SCREEN (bead
	 * VaCuus-dgl). Being public and exported made it look like a buyer entry point, and
	 * docs/buyer/setup.md advertised it as one; it never was, because the plugin's own
	 * host is not constructible from outside VaCuusRender -- its argument is a frame
	 * sink, and every sink is render-backend internal. To SHOW something, use
	 * UVaCuusWidget (screen) or UVaCuusWorldComponent (world), both exported from
	 * VaCuusRender, both of which call this for you. Call it directly only when you are
	 * bringing your own IVaCuusDocumentHost -- headless, offscreen, a test probe -- which
	 * is a real and supported thing to do: that interface is pure virtual and needs
	 * nothing linked. VaCuusRender.Build.cs carries the whole argument.
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
	 * The Blueprint-struct-recompile refusal (VaCuus-akj.16, spec M6 2(j)), and the runtime
	 * target the editor module's FVaCuusStructRecompileGuard forwards to from the struct
	 * editor's PreChange -- which fires BEFORE the compile, with the OLD property chain still
	 * alive (BroadcastPreChange at UserDefinedStructureCompilerUtils.cpp:599 precedes the
	 * compile at :622). That window is the entire design: it is the last moment any buffer
	 * laid out by the old properties can be destroyed THROUGH the type.
	 *
	 * WHAT IT DOES, in order:
	 *  1. Enqueues the definition-registry stale mark for ChangedStruct (matched models or
	 *     none -- the type may be cached purely as an element type).
	 *  2. Walks every view of every game instance (the DumpModels walk, for the same reasons
	 *     it has) and condemns every model whose root or array-element struct is
	 *     ChangedStruct: one Error each, dead flag, game-side shadow destroyed NOW.
	 *  3. Enqueues one UI-side drop per condemned model, then FENCES: Trigger +
	 *     WaitForFrameCount (documented any-thread-safe; RunFrameInline in inline mode) with
	 *     a ~100 ms budget, so the drops' normal DestroyStruct teardown runs while this
	 *     thread provably holds the old chain alive inside PreChange.
	 *  4. ONLY on timeout: each undelivered model flips to abandon-on-arrival and one Error
	 *     reports its estimated leaked bytes -- the residual leak is loud, rare and measured,
	 *     not a design principle.
	 *
	 * STATIC AND HERE for the reason DumpModels is: Views is private and stays private.
	 * Game thread (PreChange broadcasts there, and steps 2-3 are game-thread state and the
	 * SPSC queue's single producer). Returns the number of models condemned. Type-agnostic on
	 * purpose -- the editor guard only ever passes UUserDefinedStructs, but the machinery
	 * cares about pointer identity, which is what lets a native-struct test drive it.
	 */
	static int32 NotifyStructPreRecompile(const UScriptStruct* ChangedStruct);

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

	/**
	 * Publishes a localization table (M5 Task 8, spec §2(l)): its entries answer
	 * `vacuus.translate(key)` in every view's JS and the RML-text TranslateString
	 * path (FVaCuusSystemInterface). Whole-table replacement with a monotonic
	 * version — the "handler" a game registers for localization IS this push, not a
	 * per-call callback, because translate() answers synchronously on the UI thread
	 * and game code must not run there (FVaCuusTranslationRegistry has the whole
	 * argument). Game thread.
	 *
	 * A THIN DOOR TO PROCESS-WIDE STATE like RegisterStyleSet: one RmlUi, one
	 * SystemInterface, so the table serves every view of every game instance.
	 *
	 * Push BEFORE loading documents whose RML text should translate — RmlUi runs
	 * TranslateString once, at text instancing; `vacuus.translate` calls always see
	 * the newest table. After a language change: push the new table, then reload
	 * (ClearAssetCachesAndReloadAllViews) for the RML half.
	 *
	 * THE RELOAD IS ONLY FOR PARSE-TIME TEXT. Markup written as `{{ t.key }}` inside a data
	 * model re-translates in place on the next UI frame, no reload and no lost JS state —
	 * live is opt-in per string (FVaCuusTranslationVariable carries the shape and its key
	 * restrictions). Reload remains correct, and remains the only route for plain markup.
	 *
	 * Tag is the caller's own label ("ru", a culture name, anything): it is handed to
	 * OnTranslationTableChanged and to `vacuus.onLanguageChanged`, and never interpreted.
	 *
	 * TAG IS DEFAULTED RATHER THAN REQUIRED because this is a shipped BlueprintCallable: a
	 * new required pin would break every existing graph that calls it. An empty tag is a
	 * legal "the pusher did not say", and the signals carry it through unchanged.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void SetTranslationTable(const TMap<FString, FString>& Table, const FString& Tag = TEXT(""));

	/**
	 * The same push, built from an engine String Table asset — the bridge that makes UE's
	 * own localization pipeline work through VaCuus with no glue written by the caller.
	 *
	 * Each entry's value is FText::FromStringTable(TableId, Key).ToString() (Text.h:510),
	 * i.e. ALREADY RESOLVED AGAINST THE CURRENT CULTURE, so the game's `.locres` for the
	 * active language is what lands in the table. Keys come from the always-available
	 * 2-parameter FStringTable::EnumerateKeysAndSourceStrings (StringTableCore.h:162); the
	 * 3-parameter overload beside it is WITH_EDITORONLY_DATA and would not survive cooking.
	 *
	 * Call it again after a culture change — the values are resolved HERE, at call time, and
	 * nothing re-resolves itself afterwards (the same reason UpdateModel needs a re-push).
	 *
	 * @return how many entries were published; 0 with a Warning for a null or empty asset.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	int32 SetTranslationTableFromStringTable(UStringTable* StringTable, const FString& Tag);

	/**
	 * "A new translation table is installed", on the game thread. THE HANGING POINT FOR THE
	 * FText RE-PUSH: a bound model's FText fields are resolved once, on push, and can never
	 * re-resolve themselves (UVaCuusView::UpdateModel carries the whole contract), so a
	 * culture change stays invisible to them until something calls UpdateModel again. Bind
	 * here and do it, instead of wiring FInternationalization::OnCultureChanged yourself.
	 *
	 * Forwarded from the process-wide registry, so it fires however the table was pushed.
	 */
	UPROPERTY(BlueprintAssignable, Category = "VaCuus")
	FOnVaCuusTranslationTableChangedBP OnTranslationTableChanged;

	/**
	 * Loads a font face at runtime, from a path under the DevUI roots (the same VFS every other
	 * UI file comes through, so a bundled face works like a loose one).
	 *
	 * WHY THE PLUGIN OWNS THIS: Rml::LoadFontFace touches process-global font state on the UI
	 * thread, which game code has no way to reach. And it is not optional for a localized game —
	 * the face this plugin ships covers Latin only (measured: 20-7e, a0-17f plus symbols), so
	 * Russian or Chinese text without your own face renders U+FFFD for every character, and RmlUi
	 * substitutes that replacement silently (FontFaceHandleDefault.cpp:386-393).
	 *
	 * bFallbackFace puts the face in RmlUi's fallback chain, consulted IN REGISTRATION ORDER for
	 * any character the styled family lacks — which is what makes mixed-script text work without
	 * per-run font switching in the markup.
	 *
	 * Registered faces are replayed if the UI thread restarts, and re-registering the same face
	 * is a no-op, so calling this from a language-change handler is safe. RCSS `@font-face`
	 * remains the authoring route; this is for a game that only knows which face it needs at
	 * runtime. Game thread.
	 *
	 * @return false for an empty path or a face already registered.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	bool LoadFontFace(const FString& VfsPath, bool bFallbackFace = false);

	/**
	 * Mounts a UI bundle (M6): its packed files then serve every VFS open ahead of the
	 * loose roots, for every view of every game instance -- the mount table is one per
	 * process, like the UI thread and the VFS it feeds, so this is a thin door to
	 * FVaCuusBundleMountTable exactly as RegisterStyleSet is to the style registry.
	 * Idempotent per asset; multiple bundles stack in mount order, first hit wins.
	 * Mounting STEALS the asset's payload region (once-only -- the mount record
	 * retains it across unmounts), after which the asset object may be GC'd freely.
	 * Games that manage their own bundle loading call this; the config-listed bundle
	 * ([VaCuus] BundleAssetPath) mounts itself in cooked builds. Game thread.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	bool MountBundle(UVaCuusBundle* Bundle);

	/**
	 * Unmounts a bundle: removes it from the lookup, so new opens fall through to the
	 * loose roots. Reads already in flight keep their bytes (the record outlives the
	 * lookup entry), and a later MountBundle of the same asset reuses the retained
	 * record. Game thread.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	bool UnmountBundle(UVaCuusBundle* Bundle);

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

	/**
	 * Subscription to the process-wide registry signal, released in Deinitialize(). Held as
	 * a handle rather than removed by object, because the registry delegate outlives every
	 * subsystem and a PIE session that leaked one would fire into a destroyed UObject.
	 */
	FDelegateHandle TranslationChangedHandle;
};
