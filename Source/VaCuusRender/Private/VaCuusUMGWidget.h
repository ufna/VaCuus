// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Components/Widget.h"

#include "Templates/SharedPointer.h"

#include "VaCuusUMGWidget.generated.h"

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
 */
UCLASS(meta = (DisplayName = "VaCuus View"))
class UVaCuusWidget : public UWidget
{
	GENERATED_BODY()

public:
	/**
	 * Document to show, as an RmlUi VFS path (resolved against
	 * <Project>/Content/DevUI by FVaCuusFileInterface). Empty means "no document";
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
