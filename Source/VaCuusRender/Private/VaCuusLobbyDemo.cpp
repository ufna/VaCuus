// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

/**
 * vacuus.LobbyDemo -- the five-screen shooter-lobby demo over the VaCuusDemo project's
 * DevUI content (chrome.rml + lobby/collection/battlepass/career/store.rml).
 *
 * CONTENT-DEPENDENT BY DESIGN: the documents live in the HOST PROJECT's Content/DevUI
 * root, not the plugin's, and the command refuses by name when they are not served.
 * The documents carry NO script anywhere; every control is a body-class flip or a
 * document swap that THIS file performs, mirroring the parallel developer's browser
 * router (VaCuusDemo Content/DevUI/preview/preview.js) rule for rule -- that file is
 * the behavior spec the README points at, and drift from it is a bug here.
 *
 * THREE STACKED VIEWS, ONE INPUT DOOR:
 *
 *   backdrop (bottom)  a host-owned inline document showing img/bg.png. Chrome owns
 *                      the backdrop markup, but chrome composites ON TOP -- its opaque
 *                      full-bleed #bg would hide every screen -- so the host hides
 *                      chrome's #bg and paints the same art in the one layer that is
 *                      genuinely behind everything. As its own view it also never
 *                      republishes on navigation.
 *   content (middle)   the screen document, swapped by UVaCuusView::LoadDocument.
 *   chrome (top)       chrome.rml, loaded once and NEVER reloaded -- top bar, rails,
 *                      chrome-level overlays. Screen overlays (ov-match/ov-buy/ov-bp)
 *                      live in their screen documents and dim under the chrome.
 *
 * WHY ONE WIDGET ROUTES ALL INPUT: Slate's bubble path is the ancestor chain of the
 * single topmost hit-testable widget -- FHittestGrid::GetBubblePath walks
 * Advanced_GetPaintParentWidget from the best hit (HittestGrid.cpp:214-226) -- so an
 * Unhandled reply from a viewport overlay widget reaches SViewport (an ancestor) but
 * NEVER a covered sibling slot. Three stacked SVaCuusWidgets would give the top one
 * every pointer event and the other two none, whatever their replies. So the chrome
 * widget (a subclass) is the only hit-testable one and forwards what chrome's
 * interactive-region snapshot declines to the content view, answering Slate from the
 * CONTENT snapshot then; the two paint-only widgets are HitTestInvisible. The backdrop
 * publishes an empty snapshot and needs no routing at all.
 *
 * THE THREAD INVARIANT IS ABSOLUTE: every RmlUi call below happens on the UI thread --
 * the brain is driven only from IVaCuusDocumentHost methods (all UI-thread by
 * contract, VaCuusDocumentHost.h) and from Rml event dispatch. The game thread only
 * enqueues: view creation, LoadDocument, input. Navigation crosses UI -> game through
 * the write router's bounded queue (VaCuusGameBridge::EnqueueJsEvent -- core plumbing,
 * no JS involved), surfaces as UVaCuusView::OnJsEvent, and the game thread answers
 * with LoadDocument on the content view.
 */

#include "SVaCuusWidget.h"
#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"
#include "VaCuusDemoModel.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusGameBridge.h"
#include "VaCuusInputEvent.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSlateElement.h"
#include "VaCuusSubsystem.h"
#include "VaCuusUIThread.h"
#include "VaCuusView.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UnrealClient.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

/** Shared with VaCuusRender.cpp (same module, external linkage there) -- the SpawnM5HudQuad pattern. */
namespace VaCuusM1HUD
{
void SetUIInputMode(UWorld* World, bool bEnable);
void ScheduleAfter(float DelaySeconds, TFunction<void()> Work);
bool MoveMouseTo(const FVector2D& Position);
bool ClickWhereThePointerIs(const FVector2D& Position);
}	 // namespace VaCuusM1HUD

namespace VaCuusLobbyDemo
{
/** The persistent chrome and the boot screen; the other four screens arrive by navigation. */
static const TCHAR* GChromeVfsPath = TEXT("chrome.rml");
static const TCHAR* GLobbyVfsPath = TEXT("lobby.rml");

/** The event the UI thread emits for navigation and the payload key naming the screen. */
static const TCHAR* GNavEventName = TEXT("lobby_nav");
static const TCHAR* GNavScreenKey = TEXT("screen");

/**
 * Screen name -> document, and the ONLY names the game thread will load. The emit's
 * payload is document-shaped data that crossed a queue; feeding it to LoadDocument
 * unfiltered would let any future emitter load any file the VFS serves.
 */
static const TPair<const TCHAR*, const TCHAR*> GScreens[] = {
	{TEXT("lobby"), TEXT("lobby.rml")},
	{TEXT("collection"), TEXT("collection.rml")},
	{TEXT("battlepass"), TEXT("battlepass.rml")},
	{TEXT("career"), TEXT("career.rml")},
	{TEXT("store"), TEXT("store.rml")},
};

/**
 * The four PT Sans faces (VaCuusDemo Content/DevUI/fonts, OFL). Loaded on the UI
 * thread before any document parses -- the VaCuusEngine.cpp:139-148 pattern at demo
 * scope: RmlUi resolves the relative paths through the same ordered DevUI roots the
 * documents use, so `font-family: "PT Sans"` in the sheets resolves to these and the
 * bold/italic runs stop collapsing into LatoLatin Regular.
 */
static const TCHAR* GFontVfsPaths[] = {
	TEXT("fonts/PT_Sans-Web-Regular.ttf"),
	TEXT("fonts/PT_Sans-Web-Bold.ttf"),
	TEXT("fonts/PT_Sans-Web-Italic.ttf"),
	TEXT("fonts/PT_Sans-Web-BoldItalic.ttf"),
};

/**
 * preview.js BUY_ITEMS verbatim: what the shared purchase dialog shows for each buy
 * button (the "actual item in the confirm dialog" detail the README calls out as a
 * data-binding point; here the host swaps it in directly). UTF-8 em dash escaped.
 */
struct FBuyItem
{
	const char* Id;
	const char* Icon;
	const char* Sub;
};
static const FBuyItem GBuyItems[] = {
	{"buy-featured", "img/box_credits.png", "12 000 + 2 000 BONUS CREDITS \xE2\x80\x94 $ 4.99"},
	{"buy-ammo", "img/box_ammo.png", "TG AMMO BOX \xE2\x80\x94 $ 2.99"},
	{"buy-case", "img/bp_case.png", "BP CASE \xE2\x80\x94 $ 1.99"},
	{"buy-rifle", "img/skin_5_gold.png", "GOLD RIFLE \xE2\x80\x94 $ 9.99"},
	{"buy-knife", "img/tskin_3_gradient.png", "GRADIENT KNIFE \xE2\x80\x94 $ 12.99"},
	{"buy-sniper", "img/store_sniper_gold.png", "SNIPER GOLD \xE2\x80\x94 $ 9.99"},
	{"buy-smg", "img/store_smg_blueshift.png", "SMG BLUE SHIFT \xE2\x80\x94 $ 4.99"},
};

/**
 * Controls the brain routes that are NOT snapshot-interactive on their own. The
 * snapshot admits a rect for `tab-index: auto`, a known interactive tag, or the
 * `vacuus-interactive` marker (VaCuusInteractiveSnapshot.cpp:460-462); the demo's
 * documents put tab-index only on <button>, so every DIV/SPAN control -- the top-bar
 * plates, the overlay dims, the settings chips, the current battle-pass tier -- would
 * read as pass-through: functionally alive (input is enqueued whatever the FReply,
 * SVaCuusWidget.cpp:466-467) but answered Unhandled, so the click would ALSO fall
 * through to whatever sits underneath. Under a modal dim that is a real misroute: the
 * dim click would close the overlay AND press the content button beneath it. The host
 * therefore opts these in with the plugin's own marker, once per document load.
 * `.dialog` is in the list for the same reason: a click inside an open dialog's body
 * must be claimed (and routed to nothing), exactly as the browser twin behaves.
 */
static const char* GMarkerIds[] = {
	// chrome
	"login", "tb-exclaim", "tb-gold", "tb-credits", "tb-player", "tb-friends", "tb-gear",
	"tb-cancel", "chatbar", "right-rail", "rail-collapse", "rail-expand", "login-send",
	"profile-career", "chat-send",
	// lobby
	"mi-bp", "mi-career", "mi-collection", "mi-store", "play", "mode-card", "squad-slot-1",
	"squad-slot-2", "bp-premium", "bp-telegram", "store-credits", "store-ammo",
	// store
	"buy-featured", "buy-ammo", "buy-case", "buy-rifle", "buy-knife", "buy-sniper", "buy-smg",
	"buy-confirm", "store-back",
	// collection
	"col-back", "tab-weapon", "tab-trinket", "pg-prev", "pg-next", "equipped",
	// battlepass
	"bp-upsell", "bp-buy-confirm", "bp-buy-cancel", "claim-all", "bp-back",
	// career
	"tab-overview", "tab-matches", "career-back"};

static const char* GMarkerClasses[] = {"ov-dim", "ov-close", "esc-back", "chip", "dialog"};

/** Chrome-body classes CloseOverlays clears; the split of preview.js's closeOverlays list. */
static const char* GChromeOverlayClasses[] = {
	"ov-settings", "ov-login", "ov-profile", "ov-notify", "ov-friends", "ov-chat", "login-sent"};

/** Content-body classes CloseOverlays clears (screen-level overlays and their result states). */
static const char* GContentOverlayClasses[] = {"ov-match", "ov-buy", "ov-bp", "buy-done", "match-found"};

/** XML-escape for text that goes through SetInnerRML (the chat echo). */
static Rml::String EscapeRml(const Rml::String& Text)
{
	Rml::String Out;
	Out.reserve(Text.size());
	for (const char Character : Text)
	{
		switch (Character)
		{
			case '&': Out += "&amp;"; break;
			case '<': Out += "&lt;"; break;
			case '>': Out += "&gt;"; break;
			default: Out += Character; break;
		}
	}
	return Out;
}

/**
 * The UI-thread half of the demo: owns the two documents' pointers, the body-class
 * state machine and the click routing. Rule-for-rule with preview.js's click handler
 * -- same order, same prefixes, same close list -- because that file is the spec.
 *
 * LIFETIME: shared by the two host decorators (thread-safe SP), so it outlives both
 * documents; RmlUi holds it only as a raw Rml::EventListener on each loaded document
 * and detaches on document close. Constructed on the game thread, then UI-thread-only.
 */
class FVaCuusLobbyBrain final : public Rml::EventListener
{
public:
	//~ ------------------------------------------------------------ host hooks (UI thread)

	void OnHostInitialized(bool bChrome, uint32 ViewId)
	{
		check(FVaCuusUIThread::IsInUIThread());
		(bChrome ? ChromeViewId : ContentViewId) = ViewId;

		// Fonts are process-global in RmlUi, so once is enough, and BEFORE any document
		// parses (this runs while the AddView command drains; every LoadDocument was
		// enqueued after AddView by the single game-thread producer, so FIFO carries the
		// ordering across the thread boundary -- the StartModelDriver argument).
		if (!bFontsLoaded)
		{
			bFontsLoaded = true;
			int32 NumLoaded = 0;
			for (const TCHAR* FontVfsPath : GFontVfsPaths)
			{
				if (VaCuusContentPaths::ResolveExistingDocument(FontVfsPath).IsEmpty())
				{
					UE_LOG(LogVaCuus, Warning,
						TEXT("vacuus.LobbyDemo: font '%s' not found under any DevUI root; its runs fall back to LatoLatin"),
						FontVfsPath);
					continue;
				}
				if (Rml::LoadFontFace(Rml::String(TCHAR_TO_UTF8(FontVfsPath))))
				{
					++NumLoaded;
				}
				else
				{
					UE_LOG(LogVaCuus, Warning, TEXT("vacuus.LobbyDemo: '%s' exists but failed to load as a font face"), FontVfsPath);
				}
			}
			UE_LOG(LogVaCuus, Log, TEXT("vacuus.LobbyDemo: %d/4 PT Sans faces loaded (UI thread)"), NumLoaded);
		}
	}

	void OnHostShutdown(bool bChrome)
	{
		check(FVaCuusUIThread::IsInUIThread());
		(bChrome ? ChromeDoc : ContentDoc) = nullptr;
	}

	/** Every view resize passes through here so the hybrid floor can be re-evaluated. */
	void OnViewResized(FIntPoint InViewSize)
	{
		check(FVaCuusUIThread::IsInUIThread());
		ViewSize = InViewSize;
		ApplyHybridFloor(ChromeDoc);
		ApplyHybridFloor(ContentDoc);
	}

	/**
	 * Called right after the inner host's LoadDocumentFromFile returned, i.e. after
	 * AdoptDocument closed the old document and showed the new one -- synchronously, on
	 * the UI thread (VaCuusRmlDocumentHost.cpp:192-249). The newest document is the
	 * LAST in the context: Context::LoadDocument appends, and a load that FAILED kept
	 * the old document up -- which the same-pointer guard turns into a no-op here.
	 */
	void OnDocumentLoaded(bool bChrome, Rml::Context* Context)
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (Context == nullptr || Context->GetNumDocuments() == 0)
		{
			return;
		}

		Rml::ElementDocument* Document = Context->GetDocument(Context->GetNumDocuments() - 1);
		Rml::ElementDocument*& Stored = bChrome ? ChromeDoc : ContentDoc;
		if (Document == nullptr || Document == Stored)
		{
			// Same pointer: a failed reload (AdoptDocument kept the old document) or a
			// duplicate hook. Re-attaching the listener would double-dispatch every click.
			return;
		}
		Stored = Document;

		// Bubble-phase click listener on the document element (the <body>): every
		// descendant click bubbles here, the one-listener shape preview.js uses.
		Document->AddEventListener(Rml::EventId::Click, this);

		ApplyInteractiveMarkers(Document);
		ApplyHybridFloor(Document);

		if (bChrome)
		{
			// Chrome composites ABOVE the content, so its own opaque full-bleed backdrop
			// must not draw -- the backdrop view under everything shows the same art.
			// One inline property on one element; the markup stays untouched on disk.
			if (Rml::Element* Bg = Document->GetElementById("bg"))
			{
				Bg->SetProperty("display", "none");
			}
			SwapBodyClassPrefix(Document, "on-", ("on-" + CurrentScreen).c_str());
		}
		else
		{
			// The twin's body classes survive navigation because its body is never
			// replaced; ours is, so the remembered state is re-applied to every new
			// content body. Transient overlay classes are not remembered by design --
			// navigateTo() closes overlays before the swap, exactly like preview.js.
			ApplyPersistentClasses(Document);
		}
	}

	/** Once per UI frame from either host: the matchmaking clock (preview.js's 4 s setTimeout). */
	void Pump()
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (MatchFoundDeadline > 0.0 && FPlatformTime::Seconds() >= MatchFoundDeadline)
		{
			MatchFoundDeadline = 0.0;
			if (ContentDoc != nullptr && ContentDoc->IsClassSet("ov-match"))
			{
				ContentDoc->SetClass("match-found", true);
			}
		}
	}

	//~ ------------------------------------------------------------ Rml::EventListener

	virtual void ProcessEvent(Rml::Event& Event) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		if (Event.GetId() != Rml::EventId::Click)
		{
			return;
		}
		RouteClick(Event.GetTargetElement());
	}

private:
	//~ ------------------------------------------------------------ closest() helpers

	static Rml::Element* ClosestId(Rml::Element* Element, const char* Id)
	{
		for (Rml::Element* Walk = Element; Walk != nullptr; Walk = Walk->GetParentNode())
		{
			if (Walk->GetId() == Id)
			{
				return Walk;
			}
		}
		return nullptr;
	}

	static Rml::Element* ClosestClass(Rml::Element* Element, const char* ClassName)
	{
		for (Rml::Element* Walk = Element; Walk != nullptr; Walk = Walk->GetParentNode())
		{
			if (Walk->IsClassSet(ClassName))
			{
				return Walk;
			}
		}
		return nullptr;
	}

	/** swapClass(prefix, value) from preview.js: strip every body class with the prefix, set the wanted one. */
	static void SwapBodyClassPrefix(Rml::ElementDocument* Body, const char* Prefix, const char* Value)
	{
		if (Body == nullptr)
		{
			return;
		}

		const FString Classes = UTF8_TO_TCHAR(Body->GetClassNames().c_str());
		const FString PrefixStr = UTF8_TO_TCHAR(Prefix);
		TArray<FString> Names;
		Classes.ParseIntoArrayWS(Names);
		for (const FString& Name : Names)
		{
			if (Name.StartsWith(PrefixStr, ESearchCase::CaseSensitive))
			{
				Body->SetClass(Rml::String(TCHAR_TO_UTF8(*Name)), false);
			}
		}
		if (Value != nullptr && Value[0] != '\0')
		{
			Body->SetClass(Value, true);
		}
	}

	void CloseOverlays()
	{
		if (ChromeDoc != nullptr)
		{
			for (const char* ClassName : GChromeOverlayClasses)
			{
				ChromeDoc->SetClass(ClassName, false);
			}
		}
		if (ContentDoc != nullptr)
		{
			for (const char* ClassName : GContentOverlayClasses)
			{
				ContentDoc->SetClass(ClassName, false);
			}
		}
		MatchFoundDeadline = 0.0;
	}

	/**
	 * THE HYBRID FLOOR, the README's own scaling rule ("Scaling to other resolutions"):
	 * the layout is fluid down to 1500x1000, and BELOW that the fixed 1920x1080 design is
	 * letterbox-scaled instead, because the vertical budget is ~1080px and zone collisions
	 * start under ~1500px wide. `preview/preview.js` fit() does exactly this in the browser
	 * twin; this is the plugin-side equivalent the README says is owed, and it is measured
	 * rather than assumed -- at 1366x768 without it the menu is overrun by the mode card,
	 * the news column lands on the hero and the bottom teasers fall off the window.
	 *
	 * DONE WITH A TRANSFORM ON THE BODY rather than a context-wide scale, because RmlUi has
	 * no such scale: SetDensityIndependentPixelRatio only moves `dp`/`em` units and these
	 * sheets are written in `px`. The transform path is one the recorder implements (2D
	 * transforms are in the M1 subset), the snapshot reports transformed elements through
	 * ElementUtilities::GetBoundingBox (VaCuusInteractiveSnapshot.cpp:432-441), and RmlUi's
	 * own hit test inverse-projects the query point -- so clicks, hover and the Handled
	 * answer all keep agreeing with the pixels.
	 *
	 * Inline properties, not a stylesheet rule: nothing about this belongs in the content,
	 * and the twins have their own copy of the policy in preview.js.
	 */
	void ApplyHybridFloor(Rml::ElementDocument* Document) const
	{
		if (Document == nullptr)
		{
			return;
		}

		// preview.js's FLUID_MIN_W / FLUID_MIN_H, and the same design resolution.
		static constexpr int32 FluidMinWidth = 1500;
		static constexpr int32 FluidMinHeight = 1000;
		static constexpr float DesignWidth = 1920.0f;
		static constexpr float DesignHeight = 1080.0f;

		if (ViewSize.X <= 0 || ViewSize.Y <= 0)
		{
			return;
		}

		if (ViewSize.X >= FluidMinWidth && ViewSize.Y >= FluidMinHeight)
		{
			// Fluid: hand the sheets' own `width/height: 100%` back. RemoveProperty, not a
			// "100%" override -- an inline copy of the sheet's value is a second source of
			// truth for the same fact.
			Document->RemoveProperty("width");
			Document->RemoveProperty("height");
			Document->RemoveProperty("transform");
			Document->RemoveProperty("transform-origin-x");
			Document->RemoveProperty("transform-origin-y");
			return;
		}

		const float Scale = FMath::Min(float(ViewSize.X) / DesignWidth, float(ViewSize.Y) / DesignHeight);
		const float OffsetX = (float(ViewSize.X) - DesignWidth * Scale) * 0.5f;
		const float OffsetY = (float(ViewSize.Y) - DesignHeight * Scale) * 0.5f;

		Document->SetProperty("width", Rml::String(TCHAR_TO_UTF8(*FString::Printf(TEXT("%.0fpx"), DesignWidth))));
		Document->SetProperty("height", Rml::String(TCHAR_TO_UTF8(*FString::Printf(TEXT("%.0fpx"), DesignHeight))));

		// Origin at the top-left corner so the translate below is in window pixels and not
		// relative to a scaled centre; the default is 50% 50%, which would move the box.
		Document->SetProperty("transform-origin-x", "left");
		Document->SetProperty("transform-origin-y", "top");

		// translate THEN scale, in that order: RmlUi composes primitives left to right, so
		// the translate is in unscaled window pixels (the letterbox bars) and the scale then
		// shrinks the design box inside it.
		Document->SetProperty("transform",
			Rml::String(TCHAR_TO_UTF8(*FString::Printf(
				TEXT("translate(%.1fpx, %.1fpx) scale(%.4f)"), OffsetX, OffsetY, Scale))));
	}

	/** Marks the routed div/span controls interactive for the snapshot; see GMarkerIds. */
	static void ApplyInteractiveMarkers(Rml::ElementDocument* Document)
	{
		for (const char* Id : GMarkerIds)
		{
			if (Rml::Element* Element = Document->GetElementById(Id))
			{
				Element->SetAttribute("vacuus-interactive", true);
			}
		}

		Rml::ElementList Elements;
		for (const char* ClassName : GMarkerClasses)
		{
			Elements.clear();
			Document->GetElementsByClassName(Elements, ClassName);
			for (Rml::Element* Element : Elements)
			{
				Element->SetAttribute("vacuus-interactive", true);
			}
		}

		// The clickable current tier ('.tier.cur'), the one class-pair control.
		Elements.clear();
		Document->GetElementsByClassName(Elements, "tier");
		for (Rml::Element* Element : Elements)
		{
			if (Element->IsClassSet("cur"))
			{
				Element->SetAttribute("vacuus-interactive", true);
			}
		}
	}

	/** The remembered cross-navigation state, onto a freshly loaded content body. */
	void ApplyPersistentClasses(Rml::ElementDocument* Body)
	{
		// Families first: strip the baked default (lobby ships mode-dm, collection ships
		// tab-weapon skin-3, career ships career-overview), then set the remembered one.
		SwapBodyClassPrefix(Body, "mode-", Mode.c_str());
		SwapBodyClassPrefix(Body, "tab-", CollectionTab.c_str());
		SwapBodyClassPrefix(Body, "career-", CareerTab.c_str());
		SwapBodyClassPrefix(Body, "skin-", ("skin-" + Rml::ToString(Skin)).c_str());

		// Flags: exactly the ones preview.js's closeOverlays does NOT clear.
		Body->SetClass("equipped", bEquipped);
		Body->SetClass("bp-claimed", bBpClaimed);
		Body->SetClass("bp-bought", bBpBought);
	}

	/**
	 * Navigation: chrome flips `on-<screen>` NOW (it never reloads), overlays close on
	 * both bodies (preview.js's navigateTo), and the actual document swap is the game
	 * thread's -- UVaCuusView::LoadDocument, requested through the write router's
	 * bounded queue. Same-screen clicks return unchanged, like the twin's early-out.
	 */
	void Navigate(const char* Screen)
	{
		if (CurrentScreen == Screen)
		{
			return;
		}
		CurrentScreen = Screen;
		CloseOverlays();
		SwapBodyClassPrefix(ChromeDoc, "on-", ("on-" + CurrentScreen).c_str());

		TArray<FVaCuusJsKeyValue> Payload;
		FVaCuusJsKeyValue& Entry = Payload.AddDefaulted_GetRef();
		Entry.Key = GNavScreenKey;
		Entry.Value = FVaCuusJsValue::MakeString(UTF8_TO_TCHAR(Screen));
		VaCuusGameBridge::EnqueueJsEvent(ContentViewId, FName(GNavEventName), MoveTemp(Payload));
	}

	/** Opens one overlay class on the owning body after closing everything, preview.js shape. */
	void OpenOverlay(Rml::ElementDocument* Body, const char* OverlayClass)
	{
		CloseOverlays();
		if (Body != nullptr)
		{
			Body->SetClass(OverlayClass, true);
		}
	}

	/**
	 * The click router: preview.js's document-level handler, rule for rule and in its
	 * order (close affordances, rail, overlay openers, esc-back, routes, mode cycle,
	 * tabs, skins, pager, equip, chips, claims, confirms, login, chat).
	 */
	void RouteClick(Rml::Element* Target)
	{
		if (Target == nullptr)
		{
			return;
		}

		// Close affordances first: X buttons, cancel buttons, the dim, the top-bar cancel plate.
		if (ClosestClass(Target, "ov-close") != nullptr || ClosestClass(Target, "ov-dim") != nullptr ||
			ClosestId(Target, "tb-cancel") != nullptr)
		{
			CloseOverlays();
			return;
		}

		// Rail collapse/expand -- BEFORE the overlay openers: the chevron lives inside
		// #right-rail, and the rail body is the friends opener.
		if (ClosestId(Target, "rail-collapse") != nullptr)
		{
			if (ChromeDoc != nullptr)
			{
				ChromeDoc->SetClass("rail-off", true);
			}
			return;
		}
		if (ClosestId(Target, "rail-expand") != nullptr)
		{
			if (ChromeDoc != nullptr)
			{
				ChromeDoc->SetClass("rail-off", false);
			}
			return;
		}

		// Rail avatars and tool plates open the friends drawer.
		if (ClosestClass(Target, "rail-ava") != nullptr)
		{
			OpenOverlay(ChromeDoc, "ov-friends");
			return;
		}

		// Overlay openers. Chrome-level overlays flip the chrome body; the screen-level
		// three (match, buy, bp) flip the body of the document that carries the markup.
		{
			struct FOverlayRule
			{
				const char* Id;
				const char* OverlayClass;
				bool bChromeBody;
			};
			static const FOverlayRule OverlayRules[] = {
				{"play", "ov-match", false},
				{"login", "ov-login", true},
				{"tb-gear", "ov-settings", true},
				{"tb-player", "ov-profile", true},
				{"tb-exclaim", "ov-notify", true},
				{"tb-friends", "ov-friends", true},
				{"right-rail", "ov-friends", true},
				{"squad-slot-1", "ov-friends", true},
				{"squad-slot-2", "ov-friends", true},
				{"chatbar", "ov-chat", true},
				{"bp-upsell", "ov-bp", false},
			};
			for (const FOverlayRule& Rule : OverlayRules)
			{
				if (ClosestId(Target, Rule.Id) == nullptr)
				{
					continue;
				}
				OpenOverlay(Rule.bChromeBody ? ChromeDoc : ContentDoc, Rule.OverlayClass);
				if (FCStringAnsi::Strcmp(Rule.Id, "play") == 0)
				{
					// Matchmaking finds a match after ~4 s unless cancelled (preview.js).
					MatchFoundDeadline = FPlatformTime::Seconds() + 4.0;
				}
				return;
			}

			// The buy buttons share one rule body: open ov-buy and put the actual item
			// into the shared dialog (preview.js BUY_ITEMS).
			for (const FBuyItem& Item : GBuyItems)
			{
				if (ClosestId(Target, Item.Id) == nullptr)
				{
					continue;
				}
				OpenOverlay(ContentDoc, "ov-buy");
				if (ContentDoc != nullptr)
				{
					if (Rml::Element* Icon = ContentDoc->GetElementById("ov-buy-icon"))
					{
						Icon->SetAttribute("src", Item.Icon);
					}
					if (Rml::Element* Sub = ContentDoc->GetElementById("ov-buy-sub"))
					{
						Sub->SetInnerRML(EscapeRml(Item.Sub));
					}
				}
				return;
			}
		}

		// ESC-back affordance (shared class on every sub-screen's hint).
		if (ClosestClass(Target, "esc-back") != nullptr)
		{
			Navigate("lobby");
			return;
		}

		// Page routes (the README's routing table).
		{
			struct FRouteRule
			{
				const char* Id;
				const char* Screen;
			};
			static const FRouteRule RouteRules[] = {
				{"mi-bp", "battlepass"}, {"bp-premium", "battlepass"}, {"bp-telegram", "battlepass"},
				{"mi-career", "career"}, {"profile-career", "career"},
				{"mi-collection", "collection"},
				{"mi-store", "store"}, {"store-credits", "store"}, {"store-ammo", "store"},
				{"tb-gold", "store"}, {"tb-credits", "store"},
				{"col-back", "lobby"}, {"bp-back", "lobby"}, {"career-back", "lobby"}, {"store-back", "lobby"},
			};
			for (const FRouteRule& Rule : RouteRules)
			{
				if (ClosestId(Target, Rule.Id) != nullptr)
				{
					Navigate(Rule.Screen);
					return;
				}
			}
		}

		// Mode cycle on the lobby card: DEATHMATCH -> DUEL -> PARTNERS -> ...
		if (ClosestId(Target, "mode-card") != nullptr)
		{
			static const char* Modes[] = {"mode-dm", "mode-duel", "mode-part"};
			int32 Index = 0;
			for (int32 ModeIndex = 0; ModeIndex < UE_ARRAY_COUNT(Modes); ++ModeIndex)
			{
				if (Mode == Modes[ModeIndex])
				{
					Index = ModeIndex;
					break;
				}
			}
			Mode = Modes[(Index + 1) % UE_ARRAY_COUNT(Modes)];
			SwapBodyClassPrefix(ContentDoc, "mode-", Mode.c_str());
			return;
		}

		// Collection tabs.
		if (ClosestId(Target, "tab-weapon") != nullptr)
		{
			CollectionTab = "tab-weapon";
			SwapBodyClassPrefix(ContentDoc, "tab-", CollectionTab.c_str());
			return;
		}
		if (ClosestId(Target, "tab-trinket") != nullptr)
		{
			CollectionTab = "tab-trinket";
			SwapBodyClassPrefix(ContentDoc, "tab-", CollectionTab.c_str());
			return;
		}

		// Career tabs.
		if (ClosestId(Target, "tab-overview") != nullptr)
		{
			CareerTab = "career-overview";
			SwapBodyClassPrefix(ContentDoc, "career-", CareerTab.c_str());
			return;
		}
		if (ClosestId(Target, "tab-matches") != nullptr)
		{
			CareerTab = "career-matches";
			SwapBodyClassPrefix(ContentDoc, "career-", CareerTab.c_str());
			return;
		}

		// Skin thumbnails: thw3 / tht3 -> skin-3 (shared index across the two tabs).
		if (Rml::Element* Thumb = ClosestClass(Target, "thumb"))
		{
			const Rml::String& Id = Thumb->GetId();
			if (Id.size() == 4 && Id[0] == 't' && Id[1] == 'h' && (Id[2] == 'w' || Id[2] == 't') &&
				Id[3] >= '1' && Id[3] <= '5')
			{
				Skin = Id[3] - '0';
				SwapBodyClassPrefix(ContentDoc, "skin-", ("skin-" + Rml::ToString(Skin)).c_str());
				return;
			}
		}

		// Pager arrows cycle the skin index, wrapping 1..5.
		if (ClosestId(Target, "pg-prev") != nullptr || ClosestId(Target, "pg-next") != nullptr)
		{
			Skin += (ClosestId(Target, "pg-next") != nullptr) ? 1 : -1;
			if (Skin < 1)
			{
				Skin = 5;
			}
			if (Skin > 5)
			{
				Skin = 1;
			}
			SwapBodyClassPrefix(ContentDoc, "skin-", ("skin-" + Rml::ToString(Skin)).c_str());
			return;
		}

		// Equip toggle.
		if (ClosestId(Target, "equipped") != nullptr)
		{
			bEquipped = !bEquipped;
			if (ContentDoc != nullptr)
			{
				ContentDoc->SetClass("equipped", bEquipped);
			}
			return;
		}

		// Settings chips: radio behaviour within the row.
		if (Rml::Element* Chip = ClosestClass(Target, "chip"))
		{
			if (Rml::Element* Row = Chip->GetParentNode())
			{
				const int32 NumChildren = Row->GetNumChildren();
				for (int32 Index = 0; Index < NumChildren; ++Index)
				{
					Rml::Element* Sibling = Row->GetChild(Index);
					if (Sibling != nullptr && Sibling->IsClassSet("chip"))
					{
						Sibling->SetClass("on", Sibling == Chip);
					}
				}
			}
			return;
		}

		// Battle pass claim (button or the current tier card itself). Persists across
		// navigation: not in the twin's close list, so not in ours.
		if (ClosestId(Target, "claim-all") != nullptr ||
			(ClosestClass(Target, "tier") != nullptr && ClosestClass(Target, "cur") != nullptr))
		{
			bBpClaimed = true;
			if (ContentDoc != nullptr)
			{
				ContentDoc->SetClass("bp-claimed", true);
			}
			return;
		}

		// Battle pass premium purchase confirm: CONFIRM PURCHASE -> ACTIVATED.
		if (ClosestId(Target, "bp-buy-confirm") != nullptr)
		{
			bBpBought = true;
			if (ContentDoc != nullptr)
			{
				ContentDoc->SetClass("bp-bought", true);
			}
			return;
		}

		// Store purchase confirm -> PURCHASED (transient; overlay close clears it).
		if (ClosestId(Target, "buy-confirm") != nullptr)
		{
			if (ContentDoc != nullptr)
			{
				ContentDoc->SetClass("buy-done", true);
			}
			return;
		}

		// Login flow: SEND CODE -> the code half appears.
		if (ClosestId(Target, "login-send") != nullptr)
		{
			if (ChromeDoc != nullptr)
			{
				ChromeDoc->SetClass("login-sent", true);
			}
			return;
		}

		// Squad chat send: append the typed message locally (the twin's preview sugar).
		if (ClosestId(Target, "chat-send") != nullptr)
		{
			AppendChatMessage();
			return;
		}
	}

	void AppendChatMessage()
	{
		if (ChromeDoc == nullptr)
		{
			return;
		}

		auto* Input = rmlui_dynamic_cast<Rml::ElementFormControl*>(ChromeDoc->GetElementById("chat-input"));
		if (Input == nullptr)
		{
			return;
		}

		const Rml::String Text = Input->GetValue();
		if (Text.find_first_not_of(" \t") == Rml::String::npos)
		{
			return;
		}

		Rml::ElementList Logs;
		ChromeDoc->GetElementsByClassName(Logs, "chat-log");
		if (Logs.empty())
		{
			return;
		}

		if (Rml::Element* Message = Logs[0]->AppendChild(ChromeDoc->CreateElement("div")))
		{
			Message->SetClass("chat-msg", true);
			Message->SetInnerRML(
				"<span class=\"chat-who\">You:</span><span class=\"chat-what\">" + EscapeRml(Text) + "</span>");
		}
		Input->SetValue("");
	}

	//~ ------------------------------------------------------------ state (UI thread only)

	Rml::ElementDocument* ChromeDoc = nullptr;
	Rml::ElementDocument* ContentDoc = nullptr;
	uint32 ChromeViewId = 0;
	uint32 ContentViewId = 0;
	bool bFontsLoaded = false;

	/** Which screen chrome believes is up; flips at click time, the document follows. */
	Rml::String CurrentScreen = "lobby";

	/** Newest view size either host was given; what the hybrid floor is computed from. */
	FIntPoint ViewSize = FIntPoint::ZeroValue;

	/** Wall-clock deadline for MATCH FOUND, or 0 when no search is running. */
	double MatchFoundDeadline = 0.0;

	//~ The remembered cross-navigation state (the twin keeps it by never resetting body
	//~ classes; we keep it here because the content body IS replaced). Defaults mirror
	//~ the documents' baked body classes.
	Rml::String Mode = "mode-dm";
	Rml::String CollectionTab = "tab-weapon";
	Rml::String CareerTab = "career-overview";
	int32 Skin = 3;
	bool bEquipped = false;
	bool bBpClaimed = false;
	bool bBpBought = false;
};

/**
 * Host decorator: the real FVaCuusRmlDocumentHost does everything it always does; this
 * only tells the brain when its document changed, pulses the matchmaking clock, and
 * carries the view id in. Both instances share one brain.
 */
class FVaCuusLobbyDemoHost final : public IVaCuusDocumentHost
{
public:
	FVaCuusLobbyDemoHost(TUniquePtr<IVaCuusDocumentHost> InInner, const TSharedRef<FVaCuusLobbyBrain>& InBrain, bool bInChrome)
		: Inner(MoveTemp(InInner))
		, Brain(InBrain)
		, bChrome(bInChrome)
	{
	}

	//~ Begin IVaCuusDocumentHost
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) override
	{
		if (!Inner->Initialize(InViewId, InStatus))
		{
			return false;
		}
		Brain->OnHostInitialized(bChrome, InViewId);
		return true;
	}

	virtual void Shutdown() override
	{
		Brain->OnHostShutdown(bChrome);
		Inner->Shutdown();
	}

	virtual void SetViewSize(FIntPoint ViewSize) override
	{
		Inner->SetViewSize(ViewSize);

		// After the inner host, so the context is already re-laid out at the new size when
		// the floor re-evaluates against it.
		Brain->OnViewResized(ViewSize);
	}

	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override
	{
		Inner->LoadDocumentFromFile(VfsPath, LoadSerial);
		Brain->OnDocumentLoaded(bChrome, Inner->GetContext());
	}

	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) override
	{
		Inner->LoadDocumentFromMemory(RmlSource, LoadSerial);
		Brain->OnDocumentLoaded(bChrome, Inner->GetContext());
	}

	virtual void CloseDocument() override
	{
		Brain->OnHostShutdown(bChrome);
		Inner->CloseDocument();
	}

	virtual void SetVisible(bool bVisible) override { Inner->SetVisible(bVisible); }
	virtual bool HasView() const override { return Inner->HasView(); }
	virtual Rml::Context* GetContext() const override { return Inner->GetContext(); }

	virtual void RecordAndPublishFrame() override
	{
		// Before the record, so a deadline that fired this frame is drawn this frame.
		// Both hosts pulse it; the deadline test makes the second call a no-op.
		Brain->Pump();
		Inner->RecordAndPublishFrame();
	}
	//~ End IVaCuusDocumentHost

private:
	TUniquePtr<IVaCuusDocumentHost> Inner;
	TSharedRef<FVaCuusLobbyBrain> Brain;
	bool bChrome = false;
};

/**
 * The single hit-testable widget of the stack: a full SVaCuusWidget on the CHROME view
 * (text fields, IME, keyboard and capture machinery all live where the inputs are),
 * plus forwarding of everything chrome's snapshot declines to the CONTENT view -- and
 * an answer to Slate from the content snapshot then. See the file header for why
 * sibling widgets cannot compose this via Unhandled replies.
 */
class SVaCuusLobbyRouterWidget final : public SVaCuusWidget
{
	using Super = SVaCuusWidget;

public:
	SLATE_BEGIN_ARGS(SVaCuusLobbyRouterWidget)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UVaCuusView* InChromeView,
		const TSharedRef<FVaCuusSlateElement>& InChromeElement, UVaCuusView* InContentView)
	{
		Super::Construct(Super::FArguments(), InChromeView, InChromeElement);
		ContentView = InContentView;
	}

	//~ Begin SWidget
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		const FReply Reply = Super::OnMouseMove(MyGeometry, MouseEvent);

		// The content context ALWAYS gets the move: hover state on a content control
		// must clear when the pointer wanders onto chrome, and RmlUi only ever clears
		// hover from a processed move or leave. Both views are fullscreen at identical
		// geometry, so one transform serves both.
		const FIntPoint Position = ToContentPixels(MyGeometry, MouseEvent.GetScreenSpacePosition());
		SendToContent(FVaCuusInputEvent::MouseMove(Position, RouterModifierState(MouseEvent)));

		return Reply.IsEventHandled() ? Reply : ContentReply(Position);
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		const FReply Reply = Super::OnMouseButtonDown(MyGeometry, MouseEvent);
		if (Reply.IsEventHandled())
		{
			// Chrome claimed it (its snapshot covers the point); the press must NOT also
			// reach the content -- that is exactly the dim-over-button misroute.
			return Reply;
		}

		const FIntPoint Position = ToContentPixels(MyGeometry, MouseEvent.GetScreenSpacePosition());
		SendToContent(FVaCuusInputEvent::MouseButton(
			/*bDown=*/true, Position, MouseEvent.GetEffectingButton(), RouterModifierState(MouseEvent)));

		// No capture and no Slate focus for content presses on purpose: every content
		// control is a click (no drags, no text fields -- those are chrome's), and the
		// chrome view keeps the keyboard story in one place.
		return ContentReply(Position);
	}

	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		// Sampled BEFORE Super clears it: a chrome-captured press means the whole
		// down/up pair belonged to chrome and the content must not hear the up.
		const bool bChromeHeldCapture = IsTrackingMouseCapture_Debug();

		const FReply Reply = Super::OnMouseButtonUp(MyGeometry, MouseEvent);

		const FIntPoint Position = ToContentPixels(MyGeometry, MouseEvent.GetScreenSpacePosition());
		if (!bChromeHeldCapture)
		{
			SendToContent(FVaCuusInputEvent::MouseButton(
				/*bDown=*/false, Position, MouseEvent.GetEffectingButton(), RouterModifierState(MouseEvent)));
		}

		return Reply.IsEventHandled() ? Reply : ContentReply(Position);
	}

	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		const FReply Reply = Super::OnMouseWheel(MyGeometry, MouseEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}

		const FIntPoint Position = ToContentPixels(MyGeometry, MouseEvent.GetScreenSpacePosition());
		SendToContent(FVaCuusInputEvent::MouseWheel(Position, MouseEvent.GetWheelDelta(), RouterModifierState(MouseEvent)));
		return ContentReply(Position);
	}

	virtual void OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		Super::OnMouseEnter(MyGeometry, MouseEvent);
		SendToContent(FVaCuusInputEvent::MouseMove(
			ToContentPixels(MyGeometry, MouseEvent.GetScreenSpacePosition()), RouterModifierState(MouseEvent)));
	}

	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override
	{
		Super::OnMouseLeave(MouseEvent);
		SendToContent(FVaCuusInputEvent::MouseLeave());
	}

	virtual void OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override
	{
		Super::OnMouseCaptureLost(CaptureLostEvent);
		SendToContent(FVaCuusInputEvent::MouseLeave());
	}

	virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override
	{
		const FCursorReply Reply = Super::OnCursorQuery(MyGeometry, CursorEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}

		if (const UVaCuusView* ContentPtr = ContentView.Get())
		{
			const FVaCuusInteractiveSnapshot& Snapshot = ContentPtr->GetSnapshot();
			const FIntPoint Position = ToContentPixels(MyGeometry, CursorEvent.GetScreenSpacePosition());
			if (Snapshot.Contains(Position))
			{
				return FCursorReply::Cursor(Snapshot.Cursor);
			}
		}
		return FCursorReply::Unhandled();
	}
	//~ End SWidget

private:
	/** SVaCuusWidget::ToViewPixels is private; same two steps, same floor (SVaCuusWidget.cpp:366-373). */
	static FIntPoint ToContentPixels(const FGeometry& Geometry, const UE::Slate::FDeprecateVector2DResult& ScreenPosition)
	{
		const FVector2f Local = FVector2f(Geometry.AbsoluteToLocal(ScreenPosition));
		return FIntPoint(
			FMath::FloorToInt(Local.X * Geometry.Scale), FMath::FloorToInt(Local.Y * Geometry.Scale));
	}

	static FVaCuusModifierState RouterModifierState(const FInputEvent& Event)
	{
		FVaCuusModifierState State;
		State.bControlDown = Event.IsControlDown();
		State.bShiftDown = Event.IsShiftDown();
		State.bAltDown = Event.IsAltDown();
		State.bCommandDown = Event.IsCommandDown();
		State.bCapsLock = Event.AreCapsLocked();
		return State;
	}

	void SendToContent(const FVaCuusInputEvent& Event)
	{
		if (UVaCuusView* ContentPtr = ContentView.Get())
		{
			ContentPtr->SendInput(Event);
		}
	}

	/** Handled iff the content snapshot covers the point; Unhandled falls through to the game. */
	FReply ContentReply(FIntPoint Position) const
	{
		if (const UVaCuusView* ContentPtr = ContentView.Get())
		{
			if (ContentPtr->GetSnapshot().Contains(Position))
			{
				return FReply::Handled();
			}
		}
		return FReply::Unhandled();
	}

	TWeakObjectPtr<UVaCuusView> ContentView;
};

/** Everything the toggle owns while it is ON; the vacuus.M1HUD FState shape, three views wide. */
struct FState
{
	TWeakObjectPtr<UVaCuusSubsystem> Subsystem;
	TWeakObjectPtr<UGameViewportClient> Viewport;
	TWeakObjectPtr<UWorld> InputWorld;
	FDelegateHandle WorldTearDownHandle;

	TWeakObjectPtr<UVaCuusView> BackdropView;
	TWeakObjectPtr<UVaCuusView> ContentView;
	TWeakObjectPtr<UVaCuusView> ChromeView;

	TSharedPtr<FVaCuusSlateElement> BackdropElement;
	TSharedPtr<FVaCuusSlateElement> ContentElement;
	TSharedPtr<FVaCuusSlateElement> ChromeElement;

	TSharedPtr<SVaCuusWidget> BackdropWidget;
	TSharedPtr<SVaCuusWidget> ContentWidget;
	TSharedPtr<SVaCuusLobbyRouterWidget> RouterWidget;

	/** The OnJsEvent adapter carrying navigation requests to LoadDocument (WorldDemo shape). */
	TStrongObjectPtr<UVaCuusDemoWriteListener> NavListener;
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

	// The navigation ear goes first: a nav event drained THIS tick must not reach a
	// handler whose views are about to be retired.
	if (State->NavListener.IsValid())
	{
		if (UVaCuusView* ContentView = State->ContentView.Get())
		{
			ContentView->OnJsEvent.RemoveDynamic(State->NavListener.Get(), &UVaCuusDemoWriteListener::HandleJsEvent);
		}
		State->NavListener->OnEvent = nullptr;
		State->NavListener.Reset();
	}

	VaCuusM1HUD::SetUIInputMode(State->InputWorld.Get(), /*bEnable=*/false);

	// Spec §4 teardown order per widget: detach (stop queueing), release capture
	// (only the router ever takes any), pull out of the viewport, drop the ref.
	UGameViewportClient* Viewport = State->Viewport.Get();
	const auto RemoveWidget = [Viewport](TSharedPtr<SVaCuusWidget> Widget)
	{
		if (!Widget.IsValid())
		{
			return;
		}
		Widget->DetachView();
		Widget->ReleaseOwnPointerCapture(TEXT("VaCuus lobby demo teardown"));
		if (Viewport != nullptr)
		{
			Viewport->RemoveViewportWidgetContent(Widget.ToSharedRef());
		}
	};
	RemoveWidget(State->RouterWidget);
	RemoveWidget(State->ContentWidget);
	RemoveWidget(State->BackdropWidget);
	State->RouterWidget.Reset();
	State->ContentWidget.Reset();
	State->BackdropWidget.Reset();

	// Retire the views; the UI thread closes documents and contexts on its own thread.
	// The brain lives inside the two host decorators until the UI thread retires them.
	if (UVaCuusSubsystem* Subsystem = State->Subsystem.Get())
	{
		Subsystem->DestroyView(State->ChromeView.Get());
		Subsystem->DestroyView(State->ContentView.Get());
		Subsystem->DestroyView(State->BackdropView.Get());
	}

	State->ChromeElement.Reset();
	State->ContentElement.Reset();
	State->BackdropElement.Reset();

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus lobby demo off"));
}

static void OnWorldBeginTearDown(UWorld* World)
{
	if (GState)
	{
		UE_LOG(LogVaCuus, Log, TEXT("VaCuus lobby demo: world tear-down, switching off"));
		TearDown();
	}
}

static void OnViewLoadCompleted(UVaCuusView* View, bool bSuccess)
{
	if (!GState || bSuccess || View == nullptr)
	{
		return;
	}

	// No inline fallback here, deliberately: this demo is content-dependent by charter,
	// and a fallback probe document would hide exactly the breakage worth seeing.
	UE_LOG(LogVaCuus, Error,
		TEXT("vacuus.LobbyDemo: a document failed to load on view %u ('%s'); the previous document (if any) is still up"),
		View->GetViewId(), *View->GetDocumentPath());
}

static void Toggle()
{
	if (GState)
	{
		TearDown();
		return;
	}

	// THE NAMED REFUSAL, FIRST -- before any viewport or subsystem requirement, so a
	// content-less host (VcHost, automation) hears exactly what is missing and nothing
	// is half-created. chrome.rml and lobby.rml are the boot set; the other four
	// screens fail later, by name, through OnViewLoadCompleted if absent.
	for (const TCHAR* RequiredVfsPath : {GChromeVfsPath, GLobbyVfsPath})
	{
		if (VaCuusContentPaths::ResolveExistingDocument(RequiredVfsPath).IsEmpty())
		{
			UE_LOG(LogVaCuus, Error,
				TEXT("vacuus.LobbyDemo: '%s' is not served by any DevUI root (%s). This demo needs the VaCuusDemo ")
				TEXT("project's Content/DevUI documents; nothing was created"),
				RequiredVfsPath, *FString::Join(VaCuusContentPaths::GetDocumentRoots(), TEXT(" | ")));
			return;
		}
	}

	if (!GEngine || !GEngine->GameViewport)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("vacuus.LobbyDemo needs a game viewport (PIE or -game); it does nothing in a pure editor session"));
		return;
	}

	UGameViewportClient* Viewport = GEngine->GameViewport;
	UWorld* World = Viewport->GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UVaCuusSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<UVaCuusSubsystem>() : nullptr;
	if (!Subsystem)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.LobbyDemo: no UVaCuusSubsystem on this game instance"));
		return;
	}

	const FIntPoint InitialViewSize =
		Viewport->Viewport ? Viewport->Viewport->GetSizeXY() : FIntPoint(1920, 1080);

	const TSharedRef<FVaCuusLobbyBrain> Brain = MakeShared<FVaCuusLobbyBrain>();

	// View order IS the load-order contract: AddView(backdrop, content, chrome) drain
	// before any LoadDocument enqueued below, so the fonts (loaded in the first
	// decorated Initialize) precede every document parse.
	TSharedRef<FVaCuusSlateElement> BackdropElement = MakeShared<FVaCuusSlateElement>();
	TSharedRef<FVaCuusSlateElement> ContentElement = MakeShared<FVaCuusSlateElement>();
	TSharedRef<FVaCuusSlateElement> ChromeElement = MakeShared<FVaCuusSlateElement>();

	UVaCuusView* BackdropView =
		Subsystem->CreateView(MakeUnique<FVaCuusRmlDocumentHost>(BackdropElement), InitialViewSize);
	UVaCuusView* ContentView = Subsystem->CreateView(
		MakeUnique<FVaCuusLobbyDemoHost>(MakeUnique<FVaCuusRmlDocumentHost>(ContentElement), Brain, /*bChrome=*/false),
		InitialViewSize);
	UVaCuusView* ChromeView = Subsystem->CreateView(
		MakeUnique<FVaCuusLobbyDemoHost>(MakeUnique<FVaCuusRmlDocumentHost>(ChromeElement), Brain, /*bChrome=*/true),
		InitialViewSize);
	if (BackdropView == nullptr || ContentView == nullptr || ChromeView == nullptr)
	{
		UE_LOG(LogVaCuus, Error, TEXT("vacuus.LobbyDemo: view creation failed; demo not shown"));
		if (BackdropView != nullptr)
		{
			Subsystem->DestroyView(BackdropView);
		}
		if (ContentView != nullptr)
		{
			Subsystem->DestroyView(ContentView);
		}
		if (ChromeView != nullptr)
		{
			Subsystem->DestroyView(ChromeView);
		}
		return;
	}

	GState = MakeUnique<FState>();
	GState->Subsystem = Subsystem;
	GState->Viewport = Viewport;
	GState->InputWorld = World;
	GState->BackdropView = BackdropView;
	GState->ContentView = ContentView;
	GState->ChromeView = ChromeView;
	GState->BackdropElement = BackdropElement;
	GState->ContentElement = ContentElement;
	GState->ChromeElement = ChromeElement;
	GState->WorldTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddStatic(&OnWorldBeginTearDown);

	ContentView->OnLoadCompleted.AddStatic(&OnViewLoadCompleted);
	ChromeView->OnLoadCompleted.AddStatic(&OnViewLoadCompleted);

	// The navigation ear: the brain's lobby_nav emits surface here (game thread), and
	// only whitelisted screens ever reach LoadDocument.
	GState->NavListener = TStrongObjectPtr<UVaCuusDemoWriteListener>(NewObject<UVaCuusDemoWriteListener>());
	GState->NavListener->OnEvent = [](FName Name, const TArray<FVaCuusJsKeyValue>& Payload)
	{
		if (!GState || Name != FName(GNavEventName))
		{
			return;
		}

		const FVaCuusJsKeyValue* ScreenEntry = Payload.FindByPredicate(
			[](const FVaCuusJsKeyValue& Entry) { return Entry.Key == GNavScreenKey; });
		if (ScreenEntry == nullptr || ScreenEntry->Value.Kind != EVaCuusJsValueKind::String)
		{
			UE_LOG(LogVaCuus, Warning, TEXT("vacuus.LobbyDemo: navigation event without a screen name; ignored"));
			return;
		}

		for (const TPair<const TCHAR*, const TCHAR*>& Screen : GScreens)
		{
			if (ScreenEntry->Value.String == Screen.Key)
			{
				if (UVaCuusView* View = GState->ContentView.Get())
				{
					UE_LOG(LogVaCuus, Display, TEXT("vacuus.LobbyDemo: navigating to %s ('%s')"), Screen.Key, Screen.Value);
					View->LoadDocument(Screen.Value);
				}
				return;
			}
		}
		UE_LOG(LogVaCuus, Warning,
			TEXT("vacuus.LobbyDemo: navigation to unknown screen '%s' refused"), *ScreenEntry->Value.String);
	};
	ContentView->OnJsEvent.AddDynamic(GState->NavListener.Get(), &UVaCuusDemoWriteListener::HandleJsEvent);

	// The backdrop: chrome owns the art but composites on top, so the same bg.png
	// shows from the content-side backdrop.rml in the bottom view (see the file
	// header and that document's own comment). A FILE, not an inline document, so
	// its relative img src resolves through the DevUI roots like every other
	// document's -- an inline document's vacuus://memory.rml source URL is served
	// by no root, and RmlUi's URL join mangles even an absolute POSIX src path.
	if (VaCuusContentPaths::ResolveExistingDocument(TEXT("backdrop.rml")).IsEmpty())
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("vacuus.LobbyDemo: backdrop.rml not found under any DevUI root; the demo runs without its backdrop"));
	}
	else
	{
		BackdropView->LoadDocument(TEXT("backdrop.rml"));
	}

	// Content first, chrome second; the brain flips chrome's body to on-lobby when
	// chrome.rml adopts.
	ContentView->LoadDocument(GLobbyVfsPath);
	ChromeView->LoadDocument(GChromeVfsPath);

	// The stack. Only the router is hit-testable; Slate's bubble path is the topmost
	// hit widget's ancestor chain (HittestGrid.cpp:214-226), so the two paint layers
	// must not be candidates at all.
	TSharedRef<SVaCuusWidget> BackdropWidget = SNew(SVaCuusWidget, BackdropView, BackdropElement);
	BackdropWidget->SetVisibility(EVisibility::HitTestInvisible);
	Viewport->AddViewportWidgetContent(BackdropWidget, /*ZOrder=*/90);

	TSharedRef<SVaCuusWidget> ContentWidget = SNew(SVaCuusWidget, ContentView, ContentElement);
	ContentWidget->SetVisibility(EVisibility::HitTestInvisible);
	Viewport->AddViewportWidgetContent(ContentWidget, /*ZOrder=*/91);

	TSharedRef<SVaCuusLobbyRouterWidget> RouterWidget =
		SNew(SVaCuusLobbyRouterWidget, ChromeView, ChromeElement, ContentView);
	Viewport->AddViewportWidgetContent(RouterWidget, /*ZOrder=*/92);

	GState->BackdropWidget = BackdropWidget;
	GState->ContentWidget = ContentWidget;
	GState->RouterWidget = RouterWidget;

	VaCuusM1HUD::SetUIInputMode(World, /*bEnable=*/true);

	// Display, the M5 packaged-gate precedent: one boot line that survives Shipping's
	// verbosity floor and names what is up.
	UE_LOG(LogVaCuus, Display,
		TEXT("VaCuus lobby demo on: chrome view %u over content view %u over backdrop view %u (initial %dx%d)"),
		ChromeView->GetViewId(), ContentView->GetViewId(), BackdropView->GetViewId(),
		InitialViewSize.X, InitialViewSize.Y);
}

static FAutoConsoleCommand GLobbyDemoCommand(
	TEXT("vacuus.LobbyDemo"),
	TEXT("Toggle the shooter-lobby demo (host project DevUI: chrome.rml + five screens): persistent chrome view over ")
	TEXT("a swapped content view over a backdrop view, PT Sans faces, the full body-class state model (navigation, ")
	TEXT("overlays, mode cycle, matchmaking, tabs, skins, equip, claims, purchases, login, chat) with no script in ")
	TEXT("any document. Refuses by name when the content is not served."),
	FConsoleCommandDelegate::CreateStatic(&Toggle));

static FAutoConsoleCommand GLobbyDemoShotCommand(
	TEXT("vacuus.LobbyDemo.Shot"),
	TEXT("Take a UI-inclusive screenshot after [delaySeconds] (default 0). The vacuus.M5Glass.Shot shape, so a ")
	TEXT("headless run can schedule several beats on one command line."),
	FConsoleCommandWithArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args)
		{
			const float DelaySeconds = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.0f;
			VaCuusM1HUD::ScheduleAfter(DelaySeconds,
				[]
				{
					UE_LOG(LogVaCuus, Log, TEXT("vacuus.LobbyDemo.Shot: requesting a UI screenshot"));
					FScreenshotRequest::RequestScreenshot(/*bInShowUI=*/true);
				});
		}));

static FAutoConsoleCommand GLobbyDemoClickCommand(
	TEXT("vacuus.LobbyDemo.Click"),
	TEXT("Move the pointer to <x> <y> (window pixels) and left-click there through Slate's real routing, after ")
	TEXT("[delaySeconds]. Headless flow driving; pair with vacuus.LobbyDemo.Rects for measured coordinates."),
	FConsoleCommandWithArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogVaCuus, Error, TEXT("vacuus.LobbyDemo.Click expects <x> <y> [delaySeconds]"));
				return;
			}
			const FVector2D Position(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]));
			const float DelaySeconds = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 0.0f;
			VaCuusM1HUD::ScheduleAfter(DelaySeconds,
				[Position]
				{
					VaCuusM1HUD::MoveMouseTo(Position);
					VaCuusM1HUD::ClickWhereThePointerIs(Position);
				});
		}));

static FAutoConsoleCommand GLobbyDemoRectsCommand(
	TEXT("vacuus.LobbyDemo.Rects"),
	TEXT("Print the chrome and content views' interactive-region snapshots after [delaySeconds]: what the router ")
	TEXT("claims for chrome, what falls through to the content, and what reaches the game."),
	FConsoleCommandWithArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args)
		{
			const float DelaySeconds = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.0f;
			VaCuusM1HUD::ScheduleAfter(DelaySeconds,
				[]
				{
					if (!GState)
					{
						UE_LOG(LogVaCuus, Error, TEXT("vacuus.LobbyDemo.Rects needs the lobby demo to be on"));
						return;
					}

					const auto DumpView = [](const TCHAR* Label, const UVaCuusView* View)
					{
						if (View == nullptr)
						{
							return;
						}
						const FVaCuusInteractiveSnapshot& Snapshot = View->GetSnapshot();
						UE_LOG(LogVaCuus, Log, TEXT("LobbyRects[%s]: view %u generation=%llu size=%dx%d rects=%d"),
							Label, View->GetViewId(), Snapshot.Generation, Snapshot.ViewSize.X, Snapshot.ViewSize.Y,
							Snapshot.InteractiveRects.Num());
						const int32 NumRects =
							FMath::Min(Snapshot.InteractiveRects.Num(), Snapshot.RectFlags.Num());
						for (int32 Index = 0; Index < NumRects; ++Index)
						{
							const FIntRect& Rect = Snapshot.InteractiveRects[Index];
							UE_LOG(LogVaCuus, Log, TEXT("LobbyRects[%s]:   [%2d] (%4d,%4d)-(%4d,%4d) centre (%4d,%4d) flags=%d"),
								Label, Index, Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y,
								(Rect.Min.X + Rect.Max.X) / 2, (Rect.Min.Y + Rect.Max.Y) / 2,
								int32(Snapshot.RectFlags[Index]));
						}
					};
					DumpView(TEXT("chrome"), GState->ChromeView.Get());
					DumpView(TEXT("content"), GState->ContentView.Get());
				});
		}));
}	 // namespace VaCuusLobbyDemo
