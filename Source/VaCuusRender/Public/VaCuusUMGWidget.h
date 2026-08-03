// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Components/Widget.h"

#include "Templates/SharedPointer.h"

#include "VaCuusUMGWidget.generated.h"

// FORWARD DECLARATIONS, NOT INCLUDES, AND THAT IS LOAD-BEARING NOW THAT THIS HEADER IS
// PUBLIC. FVaCuusSlateElement and SVaCuusWidget live in Private/ and are staying there
// (VaCuusRender.Build.cs carries the surface decision); a public header that included
// them would not compile in a buyer's module at all, because UBT only puts a
// dependency's Public/ on the consumer's include path. They appear here solely as
// TSharedPtr members, and a TSharedPtr never needs its pointee complete to be declared,
// copied or destroyed: it stores an FSharedReferencer (SharedPointer.h:653), which holds
// nothing but a TReferenceControllerBase<Mode>*, and the controller's DestroyObject() is
// a VIRTUAL bound to the concrete deleter at construction time
// (SharedPointerInternals.h:69, :461-463) -- i.e. in our .cpp, where the definitions are.
// The buyer's compiler is therefore never asked what these types are.
class FVaCuusSlateElement;
class SVaCuusWidget;
class UGameInstance;
class UVaCuusSubsystem;
class UVaCuusView;

/**
 * The UMG face of one VaCuus view: drop it into a widget blueprint, name a
 * document, and the RmlUi UI composites inside that widget's slot with the
 * surrounding UMG hierarchy laying it out.
 *
 * WHAT IT ADDS OVER SVaCuusWidget: nothing but ownership. The Slate widget already
 * is the whole render and input surface; this class exists to give it a UObject
 * lifetime UMG understands -- create the view, hand it to the Slate widget, and
 * retire both on the exact release call UMG makes. Every input, hit-test and
 * pass-through decision stays in SVaCuusWidget, and so does the render path.
 *
 * WHAT IT DELIBERATELY DOES NOT DO: touch the player's input mode. Under
 * FInputModeGameOnly the game viewport holds mouse capture and Slate never routes
 * pointer events to any widget over the viewport, VaCuus or otherwise -- but that is
 * the game's decision to make (a HUD wants GameOnly, a menu wants GameAndUI), and a
 * widget that silently changed it would fight whatever the game set. vacuus.M1HUD
 * changes it because it is a debug toggle with no game to consult; this does not.
 *
 * KEEP IT HIT-TEST VISIBLE. Input only reaches SVaCuusWidget's handlers while the
 * widget is EVisibility::Visible, and UWidget::SynchronizeProperties pushes the
 * `Visibility` UPROPERTY onto the Slate widget -- so setting this widget to
 * SelfHitTestInvisible in the designer turns the whole document into a picture. The
 * default (Visible) is what the input path needs; pass-through is decided per event
 * from the interactive-region snapshot, not by the visibility flag.
 *
 * VIEW OWNERSHIP vs UWidget's LIFECYCLE, the one thing worth knowing here:
 * RebuildWidget() is not a constructor and can fire SEVERAL TIMES for one object.
 * UWidget::TakeWidget_Private() calls it whenever the cached Slate widget (a weak
 * pointer) has gone, and the release that makes it go is routine: taking a
 * UUserWidget out of the viewport destroys its SObjectWidget, whose ResetWidget()
 * calls ReleaseSlateResources(true) on the whole widget tree (SObjectWidget.cpp:55-72)
 * -- so remove-and-re-add rebuilds, and so does a designer refresh
 * (FWidgetBlueprintEditorUtils::DestroyUserWidget, which even ensure()s that the
 * release really destroyed the Slate widgets). A view per call with no matching
 * retirement would leak an RmlUi context per re-add. So: ReleaseSlateResources()
 * retires the view, AND RebuildWidget() retires any view it still finds before
 * creating the next -- which makes the invariant "at most one view per
 * UVaCuusWidget, alive exactly as long as its Slate widget" hold no matter how the
 * calls interleave, including on the GC path (UVisual::BeginDestroy releases too).
 *
 * KNOWN LIMITATION -- A FAILED BUILD IS PERMANENT FOR THIS WIDGET INSTANCE. When
 * RebuildWidget() cannot produce a view (no game instance, no UVaCuusSubsystem yet, or
 * UVaCuusSubsystem::CreateView() failed) it returns an SSpacer, and that SSpacer is what
 * goes into the Slate tree. There is NO self-retry: UWidget::TakeWidget_Private only calls
 * RebuildWidget() again once its cached `MyWidget` weak pointer has expired
 * (Widget.cpp:TakeWidget_Private), i.e. only after the parent drops the SSpacer -- and
 * SynchronizeProperties() cannot repair it either, because the widget already published to
 * the tree is an SSpacer and UWidget offers no way to swap it for a real one. So a
 * TRANSIENT failure (a widget built one frame before its subsystem exists) shows an empty
 * slot for the widget's whole lifetime; the workaround is to remove and re-add it, which
 * expires the cache and rebuilds. The structural fix -- always returning an SBox host and
 * swapping its content on a later SynchronizeProperties() -- is deliberately deferred: it
 * changes the widget type every UMG-hosted view reports, and nothing in M2 hits the
 * transient case (the subsystem is a UGameInstanceSubsystem, so it exists before any widget
 * a game can create).
 */
/**
 * VACUUSRENDER_API IS THE FIX FOR VaCuus-dgl, AND THE BUG IT FIXES WAS INVISIBLE FROM
 * INSIDE THIS REPOSITORY. Without the macro this class still REGISTERS -- UHT stamps the
 * module's API macro on the generated registrar whatever the class says
 * (VaCuusUMGWidget.gen.cpp:22, `VACUUSRENDER_API UClass* Z_Construct_UClass_UVaCuusWidget`),
 * so Blueprint and UMG found it fine -- while every hand-written member stayed hidden.
 * On Linux UBT compiles modular targets with `-fvisibility-ms-compat`
 * (LinuxToolChain.cs:437-440), which is "global TYPES default, global functions and
 * variables hidden": the vtable and typeinfo are exported, the methods are not. The
 * delivered libUnrealEditor-VaCuusRender.so proved it -- `Z_Construct_UClass_UVaCuusWidget`
 * and `_ZTV13UVaCuusWidget` present, `UVaCuusWidget::` members zero -- so a buyer who got
 * precompiled binaries could place this widget in a blueprint and could not link one line
 * of C++ against it. Nothing in-tree could ever notice: every consumer so far compiles the
 * plugin from source, where Private/ is on the include path and visibility is irrelevant.
 * The observable is `nm -D`, and it is what the fix is checked against.
 */
UCLASS(meta = (DisplayName = "VaCuus View"))
class VACUUSRENDER_API UVaCuusWidget : public UWidget
{
	GENERATED_BODY()

public:
	/**
	 * Document to show, as an RmlUi VFS path (resolved against
	 * a DevUI root by FVaCuusFileInterface -- see VaCuusContentPaths.h). Empty means "no document";
	 * call LoadDocument() at runtime instead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaCuus", meta = (DisplayName = "Document"))
	FString DocumentPath;

	/** Load DocumentPath as soon as the widget is built. Off means the game decides when. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaCuus")
	bool bAutoLoadDocument = true;

	/**
	 * Replaces the current document. Asynchronous, like everything else on the view:
	 * the UI thread loads it and reports back through UVaCuusView::OnLoadCompleted.
	 *
	 * Also updates DocumentPath, so a later rebuild (a re-add to the viewport, a
	 * designer refresh) comes back with the document that was actually last asked for
	 * rather than the one the asset was saved with.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void LoadDocument(FString Path);

	/** Closes the document. The view stays alive and can load another. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void Close();

	/**
	 * This widget's view handle, or null before the widget is built / after it is
	 * released. Everything the view can do that this class does not wrap (Resize,
	 * SetVisible, IsLoadPending, the snapshot) is reachable through it.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	UVaCuusView* GetView() const;

	//~ Begin UWidget
	virtual void SynchronizeProperties() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
#if WITH_EDITOR
	virtual const FText GetPaletteCategory() override;
#endif
	//~ End UWidget

protected:
	//~ Begin UWidget
	virtual TSharedRef<SWidget> RebuildWidget() override;
	//~ End UWidget

private:
	/**
	 * The game instance whose UVaCuusSubsystem owns this widget's view.
	 *
	 * THROUGH GetWorld(), NOT GetOwningPlayer(). UWidget::GetOwningPlayer() and
	 * GetOwningLocalPlayer() both resolve through UUserWidget::PlayerContext, which is
	 * only set when the user widget was created with a player controller --
	 * CreateWidget(GetWorld(), ...) and CreateWidget(GameInstance, ...) both leave it
	 * empty and would hand back null (UserWidget.cpp:1457-1464). UWidget::GetWorld()
	 * has no such hole: it goes through the widget tree to the owning user widget's
	 * world (Widget.cpp:1644-1654), which every hosting mode has.
	 *
	 * The outer-chain fallback covers a UVaCuusWidget that is NOT in a widget tree at
	 * all -- constructed directly under a game instance, which is what vacuus.UMGDemo
	 * and VaCuus.UMG.Widget do, and the only case GetWorld() cannot answer.
	 */
	UGameInstance* ResolveGameInstance() const;

	/**
	 * Retires the view, if there is one, and forgets it. Idempotent.
	 *
	 * The Slate widget is detached FIRST (which also hands Slate's navigation config
	 * back), so no queued resize or input command can land behind the removal.
	 */
	void RetireView();

	/** This widget's view; null until built. Owned by the subsystem, referenced here. */
	UPROPERTY(Transient)
	TObjectPtr<UVaCuusView> View;

	/** Who to retire the view with. Weak: world teardown may get to it first. */
	UPROPERTY(Transient)
	TWeakObjectPtr<UVaCuusSubsystem> OwningSubsystem;

	/**
	 * DocumentPath as last handed to the view.
	 *
	 * SynchronizeProperties() is not a one-shot: UMG calls it on every designer
	 * property edit and after every rebuild, and re-issuing the same load would close
	 * and re-parse the document (losing its state) each time. Comparing against this
	 * is what makes the push idempotent.
	 */
	FString AppliedDocumentPath;

	/** The Slate widget, or null at design time / when no view could be created. */
	TSharedPtr<SVaCuusWidget> MyVaCuusWidget;

	/** This view's render-thread composite target; shared with its document host. */
	TSharedPtr<FVaCuusSlateElement> Element;
};
