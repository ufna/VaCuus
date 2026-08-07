// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FVaCuusSlateElement;
class IVaCuusDocumentHost;

/**
 * Standing a view up in Slate BY HAND, for a game that cannot use UVaCuusUMGWidget.
 *
 * WHY THIS EXISTS AT ALL (bead VaCuus-akj.25). The normal door is UVaCuusUMGWidget: drop it
 * in a UMG hierarchy, name a document, done. It composes exactly ONE view into ONE widget,
 * which is the right shape for almost everything and the wrong shape for a UI that stacks
 * views -- a persistent chrome layer over a swapped content layer, say -- because stacked
 * SVaCuusWidget siblings cannot compose: Slate hands the pointer to the topmost hit-testable
 * widget, and an Unhandled reply from it goes to the game, never to a covered sibling. That
 * layout needs one widget that owns the input and routes between views itself, and building
 * it needs the two things below.
 *
 * WHY THE FACTORIES RATHER THAN THE TYPES. FVaCuusSlateElement carries the glass distiller,
 * the replay renderer and the engine-version compatibility seam; FVaCuusRmlDocumentHost
 * carries the whole UI-thread document brain. Publishing either header would drag all of
 * that into the plugin's public API and freeze it there. A caller needs none of it: it
 * creates the pair, hands them to a widget and to CreateView, and drops them at teardown.
 * So the element crosses as an OPAQUE HANDLE -- a TSharedRef to an incomplete type, which
 * copies and destroys fine because TSharedPtr's deleter is captured where the type was
 * complete -- and the host crosses as the public IVaCuusDocumentHost it already implements.
 *
 * THE WHOLE RECIPE, in the order it has to happen:
 *
 *   TSharedRef<FVaCuusSlateElement> Element = VaCuusSlateView::MakeElement();
 *   UVaCuusView* View = Subsystem->CreateView(Size, VaCuusSlateView::MakeDocumentHost(Element));
 *   TSharedRef<SVaCuusWidget> Widget = SNew(SVaCuusWidget, View, Element);
 *   Viewport->AddViewportWidgetContent(Widget, ZOrder);
 *
 * TEARDOWN IS THE PART THAT BITES, and it is the widget's contract rather than this
 * header's: call SVaCuusWidget::ReleaseOwnPointerCapture() and DetachView() BEFORE the view
 * is destroyed, then drop the widget, then the element. See SVaCuusWidget.h -- Slate's
 * captor is a weak widget path that never notices the leaf died, so a widget dropped
 * mid-drag trips an ensure inside FSlateApplication.
 */
namespace VaCuusSlateView
{
/**
 * A fresh render-side composite for one view: the frame sink the document host publishes
 * into, and the thing SVaCuusWidget composites every engine frame.
 *
 * One per view, never shared -- it owns that view's persistent render target, its replay
 * renderer's resource maps and its glass list, all of which are per-view state.
 */
VACUUSRENDER_API TSharedRef<FVaCuusSlateElement> MakeElement();

/**
 * The stock RmlUi document host over that element -- what UVaCuusSubsystem::CreateView()
 * wants, and what a game DECORATES when it needs to observe the document lifecycle
 * (IVaCuusDocumentHost is a public interface, so a wrapper that forwards every method and
 * intercepts the two it cares about is a few dozen lines).
 *
 * UI-THREAD CONTRACT, unchanged by crossing this boundary: every method on the returned
 * host runs on the VaCuus UI thread, called by the subsystem. A decorator must not touch
 * game-thread state from inside them.
 */
VACUUSRENDER_API TUniquePtr<IVaCuusDocumentHost> MakeDocumentHost(const TSharedRef<FVaCuusSlateElement>& Element);
}	 // namespace VaCuusSlateView
