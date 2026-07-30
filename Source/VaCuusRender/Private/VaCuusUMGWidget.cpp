// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusUMGWidget.h"

#include "SVaCuusWidget.h"
#include "VaCuusDefines.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSlateElement.h"
#include "VaCuusSubsystem.h"
#include "VaCuusView.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Text/STextBlock.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VaCuusUMGWidget)

#define LOCTEXT_NAMESPACE "VaCuus"

TSharedRef<SWidget> UVaCuusWidget::RebuildWidget()
{
	// FIRST, BEFORE ANYTHING ELSE: this is a rebuild, not a construction (see the class
	// comment). Whatever view a previous build left behind is retired here, so the
	// "one live view per widget" invariant survives a re-add to the viewport, a designer
	// refresh, or any other path that drops the cached Slate widget and asks again.
	RetireView();

#if WITH_EDITOR
	if (IsDesignTime())
	{
		// No view at design time on purpose: there is no game instance to own one, and
		// booting RmlUi inside the designer would put a UI thread and a live document
		// behind every open widget blueprint. A label is what the designer needs.
		return SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("VaCuusViewDesignTime", "VaCuus View"))
			];
	}
#endif

	UGameInstance* GameInstance = ResolveGameInstance();
	UVaCuusSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<UVaCuusSubsystem>() : nullptr;
	if (!Subsystem)
	{
		// SSpacer, never SNullWidget: TakeWidget_Private mutates what RebuildWidget
		// returns and ensures on the null widget (Widget.cpp:996).
		UE_LOG(LogVaCuus, Warning,
			TEXT("UVaCuusWidget '%s' found no UVaCuusSubsystem (%s), so it has nothing to show"),
			*GetName(),
			GameInstance ? TEXT("the game instance has no subsystem collection yet")
						 : TEXT("no owning game instance"));
		return SNew(SSpacer);
	}

	// The element is the render-thread half of the view: the document host publishes
	// recorded frames into it from the UI thread and the Slate widget composites it.
	const TSharedRef<FVaCuusSlateElement> NewElement = MakeShared<FVaCuusSlateElement>();

	// ZERO INITIAL SIZE ON PURPOSE. The only correct size is this widget's arranged
	// pixel rect, and UMG has not laid us out yet -- the first SVaCuusWidget::Tick
	// queues the real one. FVaCuusRmlDocumentHost::SetViewSize rejects a
	// non-positive size, so the context simply stays at its 1x1 birth size and
	// publishes nothing until that first Tick, rather than laying out once at a
	// guessed size and visibly reflowing.
	UVaCuusView* NewView =
		Subsystem->CreateView(MakeUnique<FVaCuusRmlDocumentHost>(NewElement), FIntPoint::ZeroValue);
	if (!NewView)
	{
		// Logged in detail by the subsystem/module. The element never touched the RHI.
		return SNew(SSpacer);
	}

	View = NewView;
	OwningSubsystem = Subsystem;
	Element = NewElement;

	MyVaCuusWidget = SNew(SVaCuusWidget, NewView, NewElement);

	// The document is NOT loaded here: TakeWidget_Private calls SynchronizeProperties()
	// immediately after this on the newly-created path, and that is the one place the
	// exposed properties are pushed.
	return MyVaCuusWidget.ToSharedRef();
}

void UVaCuusWidget::SynchronizeProperties()
{
	// Super FIRST: UWidget::SynchronizeProperties stomps Visibility, Enabled, Clipping
	// and RenderOpacity on the cached Slate widget, so anything applied before it would
	// be overwritten (Widget.cpp:1466-1503).
	Super::SynchronizeProperties();

	UVaCuusView* ViewPtr = View.Get();
	if (!MyVaCuusWidget.IsValid() || !ViewPtr)
	{
		// Design time, or no view could be created. Nothing to push to.
		return;
	}

	if (!bAutoLoadDocument || DocumentPath.IsEmpty())
	{
		return;
	}

	if (DocumentPath == AppliedDocumentPath)
	{
		// Already showing it. See AppliedDocumentPath for why this guard is not an
		// optimisation: a repeat push would close and re-parse the live document.
		return;
	}

	AppliedDocumentPath = DocumentPath;
	ViewPtr->LoadDocument(DocumentPath);
}

void UVaCuusWidget::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);

	// (1) MOUSE CAPTURE, AND IT MUST HAPPEN HERE.
	//
	// Slate stores its captor as a WEAK WIDGET PATH whose IsValid() only tests
	// Num() > 0, so it does not notice that the captured leaf has been destroyed:
	// releasing a VaCuus widget mid-drag leaves a captor pointing at nothing and the
	// next mouse-up trips `ensureMsgf(MouseCaptorPath.Widgets.Num() > 0, ...)` at
	// SlateApplication.cpp:5558. SVaCuusWidget does NOT do this for itself -- neither
	// its destructor nor DetachView() touches capture, and its destructor is the wrong
	// place anyway (it can run from inside Slate's own tick or paint, and by then the
	// widget is already half-gone). vacuus.M1HUD's teardown does exactly this; a
	// UMG-hosted view is the other path that can drop a captured widget, so it does too.
	//
	// PER USER, NOT ReleaseAllPointerCapture() (bead VaCuus-akj.6.16). That call drops
	// capture for EVERY Slate user, so in split-screen -- where each local player is a
	// separate FSlateUser with its own captor -- releasing this widget would also abort an
	// unrelated player's in-flight drag on a completely different widget. The precise
	// question is per user ("does THIS widget hold YOUR capture"), and FSlateUser answers
	// exactly that.
	if (MyVaCuusWidget.IsValid())
	{
		MyVaCuusWidget->ReleaseOwnPointerCapture(*FString::Printf(TEXT("UVaCuusWidget '%s' release"), *GetName()));
	}

	// (2) NAVIGATION CONFIG, via DetachView() inside RetireView().
	//
	// SVaCuusWidget installs FNullNavigationConfig while it holds focus (controller
	// decision D12) and restores it in DetachView(), OnFocusLost() and its destructor.
	// Nothing is duplicated here -- what this call adds is DETERMINISM: resetting a
	// shared pointer does not destroy a widget the parent panel or a pending Slate
	// event still references, so relying on the destructor would leave the null config
	// installed for an unknown number of frames. In UMG that is worse than in the
	// console path: the config is application-global, so a released VaCuus widget would
	// keep suppressing Tab and arrow navigation for every sibling UMG panel in the same
	// user widget.
	RetireView();

	// (3) IME CONTEXT -- ALREADY DONE, AND SAID SO EXPLICITLY (controller decision D18).
	//
	// RetireView() above calls SVaCuusWidget::DetachView(), whose FIRST act is
	// UVaCuusView::DetachIme(): the platform text-input system is told to deactivate and
	// unregister, and the context's back-pointer is nulled. Nothing extra belongs here, and the
	// reason is the same determinism argument as (2): this call site is a KNOWN teardown moment
	// for the Slate widget, and ITextInputMethodSystem holds our context by TSharedRef -- so an
	// IME left registered would keep the OS pointing at this widget's native window for as many
	// frames as it takes something else to drop the last reference. The destructor is too late,
	// which is exactly what D18 says.
	//
	// A separate call here would be worse than redundant: it would need its own null checks and
	// would make two places responsible for one invariant.

	// Our reference has to go, and it has to be the last one: the UMG designer's
	// FWidgetBlueprintEditorUtils::DestroyUserWidget ensure()s that a released widget's
	// Slate widgets are actually destroyed, and WidgetBlueprint.cpp reports a "Leak
	// Detected!" on compile for any UWidget that still owns one.
	MyVaCuusWidget.Reset();

	// The element is refcounted and thread-safe; whichever of the game, UI or render
	// thread drops the last reference destroys it, and the view's render-side release is
	// already ordered after its last publish because the UI thread enqueues both.
	Element.Reset();

	// So a rebuild reloads the document instead of deciding it is already up.
	AppliedDocumentPath.Reset();
}

void UVaCuusWidget::LoadDocument(FString Path)
{
	// Kept even without a view, so a load requested before the widget is built is
	// applied by SynchronizeProperties() when it is.
	DocumentPath = MoveTemp(Path);

	UVaCuusView* ViewPtr = View.Get();
	if (!ViewPtr)
	{
		return;
	}

	AppliedDocumentPath = DocumentPath;
	if (DocumentPath.IsEmpty())
	{
		ViewPtr->Close();
	}
	else
	{
		ViewPtr->LoadDocument(DocumentPath);
	}
}

void UVaCuusWidget::Close()
{
	AppliedDocumentPath.Reset();
	if (UVaCuusView* ViewPtr = View.Get())
	{
		ViewPtr->Close();
	}
}

UVaCuusView* UVaCuusWidget::GetView() const
{
	return View.Get();
}

#if WITH_EDITOR
const FText UVaCuusWidget::GetPaletteCategory()
{
	return LOCTEXT("VaCuusPaletteCategory", "VaCuus");
}
#endif

UGameInstance* UVaCuusWidget::ResolveGameInstance() const
{
	if (const UWorld* World = GetWorld())
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			return GameInstance;
		}
	}

	// Not in a widget tree: the outer chain is the only thing left to ask.
	return GetTypedOuter<UGameInstance>();
}

void UVaCuusWidget::RetireView()
{
	// Detach first: the widget stops queueing resize and input commands for a view
	// that is about to be removed, and hands Slate's navigation config back.
	if (MyVaCuusWidget.IsValid())
	{
		MyVaCuusWidget->DetachView();
	}

	UVaCuusView* ViewPtr = View.Get();
	View = nullptr;
	if (!ViewPtr)
	{
		return;
	}

	// If the subsystem is already gone (world or engine teardown got here first) it has
	// invalidated the view for us.
	if (UVaCuusSubsystem* Subsystem = OwningSubsystem.Get())
	{
		Subsystem->DestroyView(ViewPtr);
	}
	OwningSubsystem = nullptr;
}

namespace VaCuusUMGDemo
{
/**
 * The runtime proof for the UMG path: the same M1 HUD document the console toggle
 * shows, but hosted by UVaCuusWidget instead of by a hand-built SVaCuusWidget.
 *
 * WHY IT IS NOT A WIDGET BLUEPRINT: a UUserWidget asset would prove the same thing
 * and could not be driven from -ExecCmds in a headless run, which is where the
 * screenshot comes from. TakeWidget() is the exact call UMG itself makes on a child
 * widget, and AddViewportWidgetContent is where a UUserWidget's own Slate tree ends
 * up, so the path under test is the real one minus the asset.
 *
 * DELIBERATELY DOES NOT TOUCH THE INPUT MODE. vacuus.M1HUD does, because a debug
 * toggle has no game to consult; a UMG-hosted view inherits whatever the game set,
 * and so does this. Pointer input therefore only reaches this demo under
 * GameAndUI/UIOnly -- which is the honest behaviour, and the render path this command
 * exists to prove does not need it.
 */
static const TCHAR* GDocumentVfsPath = TEXT("m1_hud.rml");

struct FState
{
	/** Rooted: nothing else references a widget built outside a widget tree. */
	TStrongObjectPtr<UVaCuusWidget> Widget;

	/** What TakeWidget() handed back, kept so it can be removed from the viewport again. */
	TSharedPtr<SWidget> SlateWidget;

	TWeakObjectPtr<UGameViewportClient> Viewport;
	FDelegateHandle WorldTearDownHandle;
};

static TUniquePtr<FState> GState;

static void TearDown()
{
	if (!GState)
	{
		return;
	}
	TUniquePtr<FState> State = MoveTemp(GState);

	FWorldDelegates::OnWorldBeginTearDown.Remove(State->WorldTearDownHandle);

	// Out of the tree first, so no further paint or input can arrive; safe because this
	// still holds a reference, so nothing is destroyed yet and no captor can dangle.
	if (State->SlateWidget.IsValid())
	{
		if (UGameViewportClient* Viewport = State->Viewport.Get())
		{
			Viewport->RemoveViewportWidgetContent(State->SlateWidget.ToSharedRef());
		}
	}

	// The call under test: it releases mouse capture, hands the navigation config back
	// and retires the view.
	if (UVaCuusWidget* Widget = State->Widget.Get())
	{
		Widget->ReleaseSlateResources(/*bReleaseChildren=*/true);
	}

	State->SlateWidget.Reset();
	State->Widget.Reset();

	UE_LOG(LogVaCuus, Log, TEXT("UMG demo off"));
}

static void OnWorldBeginTearDown(UWorld* World)
{
	if (GState)
	{
		UE_LOG(LogVaCuus, Log, TEXT("UMG demo: world tear-down, switching off"));
		TearDown();
	}
}

static void Toggle()
{
	if (GState)
	{
		TearDown();
		return;
	}

	if (!GEngine || !GEngine->GameViewport)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.UMGDemo needs a game viewport (PIE or -game); it does nothing in a pure editor session"));
		return;
	}

	UGameViewportClient* Viewport = GEngine->GameViewport;
	UWorld* World = Viewport->GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (!GameInstance)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.UMGDemo: no game instance on the viewport's world"));
		return;
	}

	// Outered to the game instance, which is also how ResolveGameInstance() finds it
	// without a widget tree.
	UVaCuusWidget* Widget = NewObject<UVaCuusWidget>(GameInstance);
	Widget->DocumentPath = GDocumentVfsPath;
	Widget->bAutoLoadDocument = true;

	// Builds the Slate widget, creates the view and (through SynchronizeProperties)
	// queues the document load -- all of it, in one call, exactly as UMG does it.
	const TSharedRef<SWidget> SlateWidget = Widget->TakeWidget();

	GState = MakeUnique<FState>();
	GState->Widget.Reset(Widget);
	GState->SlateWidget = SlateWidget;
	GState->Viewport = Viewport;
	GState->WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddStatic(&OnWorldBeginTearDown);

	Viewport->AddViewportWidgetContent(SlateWidget, /*ZOrder=*/100);

	const UVaCuusView* View = Widget->GetView();
	UE_LOG(LogVaCuus, Log, TEXT("UMG demo on (view %s, document '%s')"),
		View ? *FString::Printf(TEXT("%u"), View->GetViewId()) : TEXT("none"), GDocumentVfsPath);
}

static FAutoConsoleCommand GToggleCommand(
	TEXT("vacuus.UMGDemo"),
	TEXT("Toggle a UVaCuusWidget (the UMG wrapper) hosting DevUI/m1_hud.rml over the game viewport. ")
	TEXT("Proves the UMG path renders the same document as vacuus.M1HUD."),
	FConsoleCommandDelegate::CreateStatic(&Toggle));
}	 // namespace VaCuusUMGDemo

#undef LOCTEXT_NAMESPACE
