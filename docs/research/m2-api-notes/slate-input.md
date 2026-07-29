# M2 API notes: Slate/UMG input plumbing

## SUMMARY
SLeafWidget is the right base for VaCuus M2 input: it adds nothing input-related of its own (it only makes OnPaint/ComputeDesiredSize pure, supplies FNoChildren, and marks SetVisibility `override final`) — every input virtual comes from SWidget and all of them are non-deprecated in 5.8. ISlateViewport/SViewport give VaCuus nothing SLeafWidget lacks for the screen-space case: the extra ISlateViewport virtuals (OnQueryPopupMethod, OnFinishedPointerInput, TranslateMouseCoordinateForCustomHitTestChild, software-cursor hooks) are all also SWidget virtuals or reachable another way, and SViewport::OnPaint hard-requires a `FSlateShaderResource*` from GetViewportRenderTargetTexture() — incompatible with VaCuus's existing FSlateDrawElement::MakeCustom / ICustomSlateElement path in SVaCuusHUDWidget. The one thing SViewport uniquely owns is `SetCustomHitTestPath(ICustomHitTestPath)`, which is exactly the engine's world-space-UI mechanism (FWidget3DHitTester in WidgetComponent.cpp) and the right precedent for spec §8's `UVaCuusWorldComponent` case. For pass-through, note Slate's hit-test returns only the topmost widget path and bubbles UP to ancestors — since viewport overlay widgets are children of SGameLayerManager which is SViewport's content, returning FReply::Unhandled() genuinely falls through to the game, but it does NOT fall through to sibling overlay widgets at lower ZOrder. For UMG, UNativeWidgetHost and USpacer are the canonical minimal UWidget shapes; RebuildWidget must never return SNullWidget (there is an ensure), and SynchronizeProperties/OnWidgetRebuilt only run on the newly-created path inside TakeWidget_Private.

## APIS (37)

### SLeafWidget (class decl + what it actually adds)
```
class SLeafWidget : public SWidget {
public:
  SLATECORE_API SLeafWidget();
  SLATECORE_API virtual ~SLeafWidget();
  SLATECORE_API virtual void SetVisibility( TAttribute<EVisibility> InVisibility ) override final;
private:
  virtual int32 OnPaint(...) const override = 0;
  virtual FVector2D ComputeDesiredSize(float) const override = 0;
  SLATECORE_API virtual FChildren* GetChildren() override;
  SLATECORE_API virtual void OnArrangeChildren(const FGeometry&, FArrangedChildren&) const override;
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SLeafWidget.h:27-70
NOTES: SLeafWidget declares ZERO input virtuals — every input override you write comes from SWidget and is a plain `virtual ... override`. SetVisibility is `override final`, so a subclass cannot override it, but you can still CALL SetVisibility(TAttribute<EVisibility>) with a bound delegate (impl is a straight pass-through to SWidget::SetVisibility, /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Private/Widgets/SLeafWidget.cpp:16-19). SLeafWidget also sets TWidgetTypeTraits<SLeafWidget>::SupportsInvalidation()==true.

### SWidget::OnMouseMove
```
SLATECORE_API virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:415
NOTES: Bubbles. Not deprecated. Fires only if the widget is hit-test visible (EVisibility::Visible or SelfHitTestInvisible-parent rules).

### SWidget::OnMouseButtonDown
```
SLATECORE_API virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:388
NOTES: Bubbles. Companion tunneling variant OnPreviewMouseButtonDown at :397 (same signature) — engine comment says use sparingly.

### SWidget::OnMouseButtonUp
```
SLATECORE_API virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:406
NOTES: Bubbles. Only routed to the mouse captor while capture is held.

### SWidget::OnMouseButtonDoubleClick
```
SLATECORE_API virtual FReply OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:461
NOTES: Note the parameter names differ from the other pointer handlers (InMyGeometry/InMouseEvent) — cosmetic only.

### SWidget::OnMouseWheel
```
SLATECORE_API virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:438
NOTES: Delta via MouseEvent.GetWheelDelta() (float, Y-axis only). Map to Rml::Context::ProcessMouseWheel(Vector2f, mods) with SIGN FLIPPED: Slate wheel-up is positive, RmlUi documents positive as 'right and down'.

### SWidget::OnMouseEnter / OnMouseLeave
```
SLATECORE_API virtual void OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);
SLATECORE_API virtual void OnMouseLeave(const FPointerEvent& MouseEvent);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:423, :430
NOTES: Both return void (no FReply). OnMouseLeave takes NO geometry. Header calls the routing 'a custom bubble strategy'. OnMouseLeave is the natural place to queue Rml::Context::ProcessMouseLeave().

### SWidget::OnCursorQuery
```
SLATECORE_API virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:445
NOTES: NOTE const method — it can only read the interactive-region snapshot, never mutate widget state. Return FCursorReply::Unhandled() or FCursorReply::Cursor(EMouseCursor::Type) (/w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Input/CursorReply.h:24, :33). Companion OnMapCursor at SWidget.h:452 returns TOptional<TSharedRef<SWidget>> for a custom cursor widget.

### SWidget::OnKeyDown / OnKeyUp / OnPreviewKeyDown
```
SLATECORE_API virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);
SLATECORE_API virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);
SLATECORE_API virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:357, :366, :348
NOTES: OnKeyDown/Up bubble; OnPreviewKeyDown tunnels and, if handled, suppresses OnKeyDown on the focused widget. All require the widget to hold user focus (see SupportsKeyboardFocus).

### SWidget::OnKeyChar
```
SLATECORE_API virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:335
NOTES: FCharacterEvent::GetCharacter() returns TCHAR (Events.h:636). Feed Rml::Context::ProcessTextInput(Character) — this is the text path, distinct from OnKeyDown.

### SWidget::OnAnalogValueChanged
```
SLATECORE_API virtual FReply OnAnalogValueChanged(const FGeometry& MyGeometry, const FAnalogInputEvent& InAnalogInputEvent);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:375
NOTES: FAnalogInputEvent : public FKeyEvent; value via GetAnalogValue() -> float (Events.h:582). This is the gamepad-axis entry point for spec §8.

### SWidget::OnFocusReceived / OnFocusLost / OnFocusChanging
```
SLATECORE_API virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent);
SLATECORE_API virtual void OnFocusLost(const FFocusEvent& InFocusEvent);
SLATECORE_API virtual void OnFocusChanging(const FWeakWidgetPath& PreviousFocusPath, const FWidgetPath& NewWidgetPath, const FFocusEvent& InFocusEvent);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:316, :323, :326
NOTES: OnFocusReceived does NOT bubble (header explicitly). OnFocusLost returns void and takes no geometry. FFocusEvent::GetCause() -> EFocusCause (Events.h:79).

### SWidget::SupportsKeyboardFocus / bCanSupportFocus
```
SLATECORE_API virtual bool SupportsKeyboardFocus() const;   // base returns false
bool CanSupportFocus() const { return bCanSupportFocus; }
protected: uint8 bCanSupportFocus : 1;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:929, :922, :1866; impl /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Private/Widgets/SWidget.cpp:977-980
NOTES: Base returns FALSE — you MUST override SupportsKeyboardFocus() to return true or the widget never gets key events (SViewport does exactly this at SViewport.h:106). bCanSupportFocus defaults true in the SWidget ctor (SWidget.cpp:217) and is a protected bitfield you assign directly in Construct — there is no setter.

### SWidget::IsInteractable
```
virtual bool IsInteractable() const { return false; }
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:1004
NOTES: Non-exported inline. Affects tooltip/cursor heuristics only, not event routing.

### SWidget::GetHitTestBoundingRect
```
SLATECORE_API virtual FSlateRect GetHitTestBoundingRect() const;   // default: GetPaintSpaceGeometry().GetRenderBoundingRect()
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:717; impl SWidget.cpp:1468-1471
NOTES: The ONLY per-widget hook that narrows what FHittestGrid considers hit — a single rect, not per-pixel and not multi-region. If the interactive-region snapshot degenerates to one bounding rect this is a cheap win; for multi-region pass-through you must use the Unhandled-bubbling approach instead.

### SWidget::SetCanTick / ForceVolatile / SetVisibility
```
void SetCanTick(bool bInCanTick);
inline void ForceVolatile(bool bForce);
SLATECORE_API virtual void SetVisibility(TAttribute<EVisibility> InVisibility);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:677, :1148, :1068
NOTES: Already used by SVaCuusHUDWidget::Construct. SetVisibility accepts a TAttribute, so a snapshot-driven delegate can flip Visible <-> HitTestInvisible per frame without touching the final override in SLeafWidget.

### FReply capture / focus / lock builders
```
FReply& CaptureMouse(TSharedRef<SWidget> InMouseCaptor);
FReply& ReleaseMouseCapture();
SLATECORE_API FReply& SetUserFocus(TSharedRef<SWidget> GiveMeFocus, EFocusCause ReasonFocusIsChanging = EFocusCause::SetDirectly, bool bInAllUsers = false);
SLATECORE_API FReply& ClearUserFocus(EFocusCause ReasonFocusIsChanging, bool bInAllUsers = false);
FReply& LockMouseToWidget(TSharedRef<SWidget> InWidget);
FReply& ReleaseMouseLock();
FReply& UseHighPrecisionMouseMovement(TSharedRef<SWidget> InMouseCaptor);
SLATECORE_API FReply& SetMousePos(const FIntPoint& NewMousePos);
FReply& DetectDrag(const TSharedRef<SWidget>& DetectDragInMe, FKey MouseButton);
FReply& PreventThrottling();
[[nodiscard]] static FReply Handled();
[[nodiscard]] static FReply Unhandled();
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Input/Reply.h:28, 114, 51, 60, 93, 103, 38, 48, 129, 157, 233, 241
NOTES: Handled()/Unhandled() are [[nodiscard]] — you must consume the return. All builders return FReply& so they chain. UseHighPrecisionMouseMovement implies capture + hidden cursor; ReleaseMouseCapture() also clears high-precision mode. ClearUserFocus(bool) at :54 forwards to the EFocusCause overload.

### FPointerEvent accessors
```
const UE::Slate::FDeprecateVector2DResult& GetScreenSpacePosition() const;
const UE::Slate::FDeprecateVector2DResult& GetLastScreenSpacePosition() const;
const UE::Slate::FDeprecateVector2DResult& GetCursorDelta() const;
FKey GetEffectingButton() const;
float GetWheelDelta() const;            // == WheelOrGestureDelta.Y
uint32 GetPointerIndex() const;
bool IsTouchEvent() const;
const TSet<FKey>& GetPressedButtons() const;
EGestureEvent GetGestureType() const;
const UE::Slate::FDeprecateVector2DResult& GetGestureDelta() const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Input/Events.h:1066, 1069, 1072, 1078, 1081, 1084, 1093, 1114, 1102, 1108
NOTES: Position getters return FDeprecateVector2DResult (implicitly converts to both FVector2f and FVector2D) — do NOT write `const FVector2D&` bindings against them. FSlateApplicationBase::CursorPointerIndex (SlateApplicationBase.h:547) is the mouse's pointer index; touch uses other indices.

### FInputEvent accessors (shared by key/char/pointer)
```
bool IsRepeat() const;
bool IsShiftDown() const; bool IsControlDown() const; bool IsAltDown() const; bool IsCommandDown() const; bool AreCapsLocked() const;
const FModifierKeysState& GetModifierKeys() const;
uint32 GetUserIndex() const;
FInputDeviceId GetInputDeviceId() const;
FPlatformUserId GetPlatformUserId() const;
uint64 GetEventTimestamp() const;
const FWidgetPath* GetEventPath() const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Input/Events.h:210, 220, 244, 268, 292, 316, 324, 332, 340, 348, 356, 380
NOTES: GetEventTimestamp() is FPlatformTime::Cycles64 — use it directly as the timestamp for the §4 input ring buffer instead of re-stamping. Modifier bools map 1:1 onto Rml::Input::KM_SHIFT/KM_CTRL/KM_ALT/KM_META/KM_CAPSLOCK. None of these are deprecated in 5.8.

### FKeyEvent accessors
```
FKey GetKey() const;            // Events.h:471
uint32 GetCharacter() const;    // :481
uint32 GetKeyCode() const;      // :491
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Input/Events.h:430-491
NOTES: struct FKeyEvent : public FInputEvent (:430). Build the FKey -> Rml::Input::KeyIdentifier table off GetKey(); GetKeyCode() is platform-specific and should not be used for the mapping.

### FGeometry: absolute<->local and size
```
FORCEINLINE_DEBUGGABLE UE::Slate::FDeprecateVector2DResult AbsoluteToLocal(UE::Slate::FDeprecateVector2DParameter AbsoluteCoordinate) const;
inline UE::Slate::FDeprecateVector2DResult LocalToAbsolute(UE::Slate::FDeprecateVector2DParameter LocalCoordinate) const;
inline UE::Slate::FDeprecateVector2DResult GetLocalSize() const;
inline UE::Slate::FDeprecateVector2DResult GetAbsolutePosition() const;
inline bool IsUnderLocation(const UE::Slate::FDeprecateVector2DParameter& AbsoluteCoordinate) const;
float Scale;  // public member
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Layout/Geometry.h:433, 446, 510, 540, 421
NOTES: AbsoluteToLocal yields LOCAL (Slate) units; multiply by Geometry.Scale to reach texture/pixel space — this is exactly what FWidget3DHitTester and FCEFWebBrowserWindow::GetCefMouseEvent do. SVaCuusHUDWidget::ComputeWindowRect already owns the rect convention; reuse it so hit-test coords and the recorded frame size agree.

### IInputProcessor (complete interface, 5.8)
```
class IInputProcessor {
public:
  IInputProcessor(){}; virtual ~IInputProcessor() = default;
  virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) = 0;   // ONLY pure virtual
  virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent&) { return false; }
  virtual bool HandleKeyUpEvent(FSlateApplication&, const FKeyEvent&) { return false; }
  virtual bool HandleAnalogInputEvent(FSlateApplication&, const FAnalogInputEvent&) { return false; }
  virtual bool HandleMouseMoveEvent(FSlateApplication&, const FPointerEvent&) { return false; }
  virtual bool HandleMouseButtonDownEvent(FSlateApplication&, const FPointerEvent&) { return false; }
  virtual bool HandleMouseButtonUpEvent(FSlateApplication&, const FPointerEvent&) { return false; }
  virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication&, const FPointerEvent&) { return false; }
  virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication&, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) { return false; }
  virtual bool HandleMotionDetectedEvent(FSlateApplication&, const FMotionEvent&) { return false; }
  virtual const TCHAR* GetDebugName() const { return TEXT(""); }
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Slate/Public/Framework/Application/IInputProcessor.h:17-54
NOTES: Whole interface is header-inline, no export macro. There is NO OnKeyChar/text hook — character input cannot be intercepted here, only via SWidget::OnKeyChar. Returning true CONSUMES the event before Slate routing. Note wheel and gesture arrive in one callback with the gesture pointer nullable.

### FSlateApplication::RegisterInputPreProcessor / Unregister
```
SLATE_API bool RegisterInputPreProcessor(TSharedPtr<class IInputProcessor> InputProcessor);
SLATE_API bool RegisterInputPreProcessor(TSharedPtr<class IInputProcessor> InputProcessor, const int32 Index);
SLATE_API bool RegisterInputPreProcessor(TSharedPtr<class IInputProcessor> InputProcessor, const EInputPreProcessorType Type);
SLATE_API bool RegisterInputPreProcessor(TSharedPtr<class IInputProcessor> InputProcessor, const FInputPreprocessorRegistrationKey& Info);
SLATE_API void UnregisterInputPreProcessor(TSharedPtr<class IInputProcessor> InputProcessor);
SLATE_API int32 FindInputPreProcessor(TSharedPtr<class IInputProcessor> InputProcessor, const EInputPreProcessorType& Type) const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h:1544, 1552, 1560, 1568, 1574, 1590
NOTES: Prefer the FInputPreprocessorRegistrationKey overload (Type + Priority) — that is what CommonUI uses. DEPRECATED: the single-arg FindInputPreProcessor(TSharedPtr<IInputProcessor>) at :1582 is UE_DEPRECATED(5.5); use the (processor, Type) form. Also deprecated: SetNavigationConfigFactory at :1433 (UE_DEPRECATED 4.20).

### EInputPreProcessorType / FInputPreprocessorRegistrationKey
```
UENUM() enum class EInputPreProcessorType : uint8 { Overlay = 0, PreEngine, Engine, PreEditor, Editor, PreGame, Game, Count };
USTRUCT() struct FInputPreprocessorRegistrationKey { GENERATED_BODY()
  UPROPERTY(Config) EInputPreProcessorType Type = EInputPreProcessorType::Game;
  UPROPERTY(Config) int32 Priority = INDEX_NONE; };
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h:189-208, 222-236
NOTES: Ascending order = earlier evaluation; earlier buckets can block later ones. Game is the default and lowest priority. VaCuus's world-space forwarder should sit in PreGame or Game — never Engine/Editor.

### FAnalogCursor
```
class FAnalogCursor : public IInputProcessor, public TSharedFromThis<FAnalogCursor> {
  SLATE_API virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override;
  SLATE_API virtual bool HandleKeyDownEvent/HandleKeyUpEvent/HandleAnalogInputEvent/HandleMouseMoveEvent(...) override;
  virtual int32 GetOwnerUserIndex() const { return 0; }
  SLATE_API void SetAcceleration/SetMaxSpeed/SetStickySlowdown/SetDeadZone(float);
  SLATE_API void SetMode(AnalogCursorMode::Type);   // Accelerated | Direct
  SLATE_API virtual void UpdateCursorPosition(FSlateApplication&, TSharedRef<FSlateUser>, const FVector2D&, bool bForce = false);
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Slate/Public/Framework/Application/AnalogCursor.h:35-107
NOTES: DEPRECATED overload: UpdateCursorPosition(FSlateApplication&, TSharedRef<ICursor>, const FVector2D&, bool) is UE_DEPRECATED(4.24) at AnalogCursor.h:78-79 — always use the FSlateUser form at :81. EAnalogStick { Left, Right, Max } at :25.

### FCommonAnalogCursor (CommonUI)
```
class FCommonAnalogCursor : public FAnalogCursor, public FGCObject {
  template <typename AnalogCursorT = FCommonAnalogCursor>
  static TSharedRef<AnalogCursorT> CreateAnalogCursor(const UCommonUIActionRouterBase& InActionRouter);
  UE_API virtual void Deinitialize();
protected:
  UE_API FCommonAnalogCursor(const UCommonUIActionRouterBase& InActionRouter);   // protected ctor
  UE_API virtual void Initialize();
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Plugins/Runtime/CommonUI/Source/CommonUI/Public/Input/CommonAnalogCursor.h:35-183
NOTES: Hard-coupled to UCommonUIActionRouterBase (const member reference) and requires GEngine->GameViewportClientClass to derive from UCommonGameViewportClient before it is even registered (CommonUIActionRouterBase.cpp:355-358). NOT reusable by VaCuus without taking a CommonUI dependency — model your own FAnalogCursor subclass on it instead. Class uses the UE_API macro (COMMONUI_API), not an inline export macro.

### FNavigationConfig
```
class FNavigationConfig : public TSharedFromThis<FNavigationConfig> {
  SLATE_API virtual EUINavigation GetNavigationDirectionFromKey(const FKeyEvent&) const;
  SLATE_API virtual EUINavigation GetNavigationDirectionFromAnalog(const FAnalogInputEvent&);
  SLATE_API virtual void OnRegister();  SLATE_API virtual void OnUnregister();
  SLATE_API virtual void OnUserRemoved(int32 UserIndex);
  virtual void OnNavigationChangedFocus(TSharedPtr<SWidget> Old, TSharedPtr<SWidget> New, FFocusEvent);
  SLATE_API virtual EUINavigationAction GetNavigationActionFromKey(const FKeyEvent&) const;
  bool bTabNavigation, bKeyNavigation, bAnalogNavigation, bIgnoreModifiersForNavigationActions;
  TMap<FKey, EUINavigation> KeyEventRules;  TMap<FKey, EUINavigationAction> KeyActionRules;
};
// install:
SLATE_API void FSlateApplication::SetNavigationConfig(TSharedRef<FNavigationConfig>);
TSharedRef<FNavigationConfig> FSlateApplication::GetNavigationConfig() const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Slate/Public/Framework/Application/NavigationConfig.h:60-139; SlateApplication.h:1426, 1420
NOTES: DEPRECATED: GetNavigationActionForKey(const FKey&) at NavigationConfig.h:86-87 (UE_DEPRECATED 4.24, no multi-user support). FNullNavigationConfig (:143) disables tab/key/analog navigation entirely — that is the config to install while a VaCuus document owns focus and RmlUi's own nav-* handles direction, otherwise Slate will steal arrow keys before OnKeyDown.

### UWidget: the three methods a wrapper implements
```
protected: UMG_API virtual TSharedRef<SWidget> RebuildWidget();          // Widget.h:1148
protected: UMG_API virtual void OnWidgetRebuilt();                        // Widget.h:1151
public:    UMG_API virtual void SynchronizeProperties();                  // Widget.h:938
public:    UMG_API virtual void ReleaseSlateResources(bool bReleaseChildren);  // declared on UVisual
public:    UMG_API TSharedRef<SWidget> TakeWidget();                      // Widget.h:823
public:    UMG_API TSharedPtr<SWidget> GetCachedWidget() const;           // Widget.h:857
protected: TWeakPtr<SWidget> MyWidget;                                    // Widget.h:1195
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/UMG/Public/Components/Widget.h:1148, 1151, 938, 823, 857, 1195; /w/Unreal/UnrealEngine/Engine/Source/Runtime/UMG/Public/Components/Visual.h:17
NOTES: ReleaseSlateResources is declared on UVisual, not UWidget — override it as `virtual void ReleaseSlateResources(bool) override` and always call Super first. UWidget::RebuildWidget base ensureMsgf(false) + returns SNew(SSpacer) (Widget.cpp:1438-1442). TakeWidget_Private ensures you never return SNullWidget (Widget.cpp:996) and calls SynchronizeProperties()+OnWidgetRebuilt() ONLY on the newly-created path (Widget.cpp:1081-1097).

### UNativeWidgetHost (smallest engine UWidget wrapper)
```
UCLASS(MinimalAPI) class UNativeWidgetHost : public UWidget {
  GENERATED_UCLASS_BODY()
  UMG_API void SetContent(TSharedRef<SWidget> InContent);
  TSharedPtr<SWidget> GetContent() const { return NativeWidget; }
  UMG_API virtual void ReleaseSlateResources(bool bReleaseChildren) override;
protected:
  UMG_API virtual TSharedRef<SWidget> RebuildWidget() override;
  TSharedPtr<SWidget> NativeWidget;
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/UMG/Public/Components/NativeWidgetHost.h:15-38; impl NativeWidgetHost.cpp:18-72
NOTES: Ctor sets bIsVariable=false. RebuildWidget wraps content in SNew(SBox); SetContent mutates the live SBox via MyWidget.Pin() static-cast. Its GetDefaultContent() returns a marching-ants SBorder at design time and SNullWidget otherwise — note it can only get away with SNullWidget because SBox wraps it.

### USpacer (minimal leaf-wrapping UWidget with a synced property)
```
UCLASS() class USpacer : public UWidget {
  UPROPERTY(EditAnywhere, BlueprintReadWrite, Getter, Setter, BlueprintSetter="SetSize", Category="Appearance") FVector2D Size;
  UMG_API virtual void SynchronizeProperties() override;
  UMG_API virtual void ReleaseSlateResources(bool bReleaseChildren) override;
protected:
  UMG_API virtual TSharedRef<SWidget> RebuildWidget() override;
  TSharedPtr<SSpacer> MySpacer;
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/UMG/Public/Components/Spacer.h:19-54; impl /w/Unreal/UnrealEngine/Engine/Source/Runtime/UMG/Private/Components/Spacer.cpp:15-70
NOTES: The exact shape to copy for UVaCuusWidget: typed TSharedPtr member, ctor sets bIsVariable + SetVisibilityInternal(ESlateVisibility::SelfHitTestInvisible), RebuildWidget assigns the member and returns .ToSharedRef(), SynchronizeProperties calls Super then early-returns on invalid, ReleaseSlateResources calls Super then .Reset().

### ISlateViewport input surface (the delta vs SWidget)
```
class ISlateViewport {
  virtual FIntPoint GetSize() const = 0;
  virtual class FSlateShaderResource* GetViewportRenderTargetTexture() const = 0;
  virtual bool RequiresVsync() const = 0;
  virtual void Tick(const FGeometry&, double InCurrentTime, float DeltaTime);
  virtual FReply OnFocusReceived(const FFocusEvent& InFocusEvent);      // NOTE: no FGeometry
  virtual void OnFocusLost(const FFocusEvent& InFocusEvent);
  virtual bool IsSoftwareCursorVisible() const;
  virtual FVector2D GetSoftwareCursorPosition() const;
  virtual TOptional<FVirtualPointerPosition> TranslateMouseCoordinateForCustomHitTestChild(...) const;
  virtual FPopupMethodReply OnQueryPopupMethod() const;
  virtual void OnFinishedPointerInput();
  virtual FNavigationReply OnNavigation(const FGeometry&, const FNavigationEvent&);
  virtual void OnDrawViewport(...) final { }   // deprecated-and-final
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Rendering/RenderingCommon.h:486-931 (GetSize :513, GetViewportRenderTargetTexture :518, RequiresVsync :576, OnFocusReceived :754, OnQueryPopupMethod :852, OnFinishedPointerInput :843, TranslateMouseCoordinate... :601/:? , OnDrawViewport :499)
NOTES: Every input virtual here has a same-named SWidget counterpart EXCEPT the signature drift on OnFocusReceived (no geometry) and OnCursorQuery (non-const here, const on SWidget). OnQueryPopupMethod, OnFinishedPointerInput, TranslateMouseCoordinateForCustomHitTestChild and OnNavigation ALSO exist on SWidget (SWidget.h:621 and SViewport.h:237-242 shows SViewport merely forwards them). Conclusion for the plan: ISlateViewport buys nothing for screen-space VaCuus, and its two pure virtuals (GetViewportRenderTargetTexture, RequiresVsync) force a FSlateShaderResource you do not have.

### SViewport::SetCustomHitTestPath + ICustomHitTestPath
```
SLATE_API void SViewport::SetCustomHitTestPath(TSharedPtr<ICustomHitTestPath> CustomHitTestPath);
SLATE_API TSharedPtr<ICustomHitTestPath> SViewport::GetCustomHitTestPath();

class ICustomHitTestPath {
  virtual TArray<FWidgetAndPointer> GetBubblePathAndVirtualCursors(const FGeometry& InGeometry, FVector2D DesktopSpaceCoordinate, bool bIgnoreEnabledStatus) const = 0;
  virtual void ArrangeCustomHitTestChildren(FArrangedChildren& ArrangedChildren) const = 0;
  virtual TOptional<FVirtualPointerPosition> TranslateMouseCoordinateForCustomHitTestChild(const SWidget& ChildWidget, const FGeometry& MyGeometry, const FVector2D ScreenSpaceMouseCoordinate, const FVector2D LastScreenSpaceMouseCoordinate) const = 0;
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Slate/Public/Widgets/SViewport.h:142, 144; /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Input/HittestGrid.h:31-41
NOTES: THIS is the world-space UI answer for spec §8, and the one thing only SViewport offers. Get the viewport widget via FSlateApplication::Get().GetGameViewport() -> TSharedPtr<SViewport> (SlateApplication.h:617). All three methods are const; note the interface still uses raw FVector2D, not the FDeprecate types.

### FVirtualPointerPosition
```
struct FVirtualPointerPosition {
  FVirtualPointerPosition();
  FVirtualPointerPosition(const UE::Slate::FDeprecateVector2DParameter& InCurrentCursorPosition, const UE::Slate::FDeprecateVector2DParameter& InLastCursorPosition);
  UE::Slate::FDeprecateVector2DResult CurrentCursorPosition;
  UE::Slate::FDeprecateVector2DResult LastCursorPosition;
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Input/Events.h:134-148
NOTES: How a custom hit-test path hands Slate a synthetic cursor position in the child's local space — the mechanism that makes world-space UI input work without a real cursor.

### UGameViewportClient overlay attach (already used by M1)
```
ENGINE_API virtual void AddViewportWidgetContent(TSharedRef<class SWidget> ViewportContent, const int32 ZOrder = 0);
ENGINE_API virtual void RemoveViewportWidgetContent(TSharedRef<class SWidget> ViewportContent);
ENGINE_API virtual void AddViewportWidgetForPlayer(ULocalPlayer* Player, TSharedRef<SWidget> ViewportContent, const int32 ZOrder);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Engine/Classes/Engine/GameViewportClient.h:258, 265, 275
NOTES: AddViewportWidgetContent adds into ViewportOverlayWidget (SOverlay) — GameViewportClient.cpp:3376-3385 — and that overlay lives inside SGameLayerManager which is SViewport's CONTENT (`ViewportWidget->SetContent(GameLayerManager->AsWidget())`, GameViewportClient.cpp:1326). So SViewport is an ANCESTOR of the VaCuus overlay: FReply::Unhandled() genuinely bubbles to the game. VaCuusRender.cpp:178 already uses ZOrder 100.

### FSlateApplication focus / IME / focus-changing hooks
```
SLATE_API bool SetUserFocus(uint32 UserIndex, const TSharedPtr<SWidget>& WidgetToFocus, EFocusCause ReasonFocusIsChanging = EFocusCause::SetDirectly);
SLATE_API void SetAllUserFocus(const TSharedPtr<SWidget>& WidgetToFocus, EFocusCause ReasonFocusIsChanging = EFocusCause::SetDirectly);
SLATE_API bool SetKeyboardFocus(const TSharedPtr<SWidget>& OptionalWidgetToFocus, EFocusCause = EFocusCause::SetDirectly);
SLATE_API void SetUserFocusToGameViewport(uint32 UserIndex, EFocusCause = EFocusCause::SetDirectly);
DECLARE_MULTICAST_DELEGATE_FiveParams(FOnFocusChanging, const FFocusEvent&, const FWeakWidgetPath&, const TSharedPtr<SWidget>&, const FWidgetPath&, const TSharedPtr<SWidget>&);
FOnFocusChanging& OnFocusChanging();
ITextInputMethodSystem* GetTextInputMethodSystem() const;
SLATE_API TSharedPtr<SViewport> GetGameViewport() const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h:678, 686, 724, 644, 590-591, 362, 617
NOTES: SetUserFocusToGameViewport is how you hand focus BACK to the game when a VaCuus document closes. GetTextInputMethodSystem() forwards to PlatformApplication and may return nullptr — always null-check.

### ITextInputMethodContext / ITextInputMethodSystem (IME)
```
class ITextInputMethodContext {
  enum class ECaretPosition { Beginning, Ending };
  virtual bool IsComposing() = 0;  virtual bool IsReadOnly() = 0;  virtual uint32 GetTextLength() = 0;
  virtual void GetSelectionRange(uint32& OutBeginIndex, uint32& OutLength, ECaretPosition& OutCaretPosition) = 0;
  virtual void SetSelectionRange(const uint32 InBeginIndex, const uint32 InLength, const ECaretPosition) = 0;
  virtual void GetTextInRange(const uint32 InBeginIndex, const uint32 InLength, FString& OutString) = 0;
  virtual void SetTextInRange(const uint32 InBeginIndex, const uint32 InLength, const FString& InString) = 0;
  virtual int32 GetCharacterIndexFromPoint(const FVector2D& InPoint) = 0;
  virtual bool GetTextBounds(const uint32 InBeginIndex, const uint32 InLength, FVector2D& OutPosition, FVector2D& OutSize) = 0;
  virtual void GetScreenBounds(FVector2D& OutPosition, FVector2D& OutSize) = 0;
  virtual TSharedPtr<FGenericWindow> GetWindow() = 0;
  virtual void BeginComposition() = 0;
  virtual void UpdateCompositionRange(const int32 InBeginIndex, const uint32 InLength) = 0;
  virtual void EndComposition() = 0;
};
class ITextInputMethodSystem {
  virtual TSharedPtr<ITextInputMethodChangeNotifier> RegisterContext(const TSharedRef<ITextInputMethodContext>&) = 0;
  virtual void UnregisterContext(const TSharedRef<ITextInputMethodContext>&) = 0;
  virtual void ActivateContext(const TSharedRef<ITextInputMethodContext>&) = 0;
  virtual void DeactivateContext(const TSharedRef<ITextInputMethodContext>&) = 0;
  virtual bool IsActiveContext(const TSharedRef<ITextInputMethodContext>&) const = 0;
};
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/ApplicationCore/Public/GenericPlatform/ITextInputMethodSystem.h:14-218
NOTES: EVERY ITextInputMethodContext method is pure virtual and NON-const, and all are called SYNCHRONOUSLY on the game thread by the platform IME. That is fundamentally at odds with §4's 'UI thread exclusively owns Rml' rule — GetTextInRange/GetTextLength/GetTextBounds need authoritative RmlUi state right now. Plan for a game-thread-side mirror of the focused input element's text+caret, refreshed from the same published snapshot. Note the module is ApplicationCore, not Core.

### Rml::Context input entry points (vendored 0ae381e)
```
bool ProcessKeyDown(Input::KeyIdentifier key_identifier, int key_modifier_state);
bool ProcessKeyUp(Input::KeyIdentifier key_identifier, int key_modifier_state);
bool ProcessTextInput(Character character);  bool ProcessTextInput(char);  bool ProcessTextInput(const String&);
bool ProcessMouseMove(int x, int y, int key_modifier_state);
bool ProcessMouseButtonDown(int button_index, int key_modifier_state);   // Left 0, Right 1, Middle 2
bool ProcessMouseButtonUp(int button_index, int key_modifier_state);
bool ProcessMouseWheel(Vector2f wheel_delta, int key_modifier_state);
bool ProcessMouseLeave();
bool IsMouseInteracting() const;
Element* GetHoverElement();  Element* GetFocusElement();
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Include/RmlUi/Core/Context.h:156, 162, 167-173, 181, 187, 193, 202, 207, 237, 115, 118
NOTES: ProcessMouseWheel(float, int) at :196 is marked @deprecated in the header — use the Vector2f overload. Mouse coords are ints in context/window space, so convert with Geometry.AbsoluteToLocal(...) * Geometry.Scale and round consistently with SVaCuusHUDWidget::ComputeWindowRect. Modifier bits: Input::KM_CTRL/SHIFT/ALT/META/CAPSLOCK/NUMLOCK/SCROLLLOCK (Input.h:233-240). IsMouseInteracting() is the UI-thread-side truth that feeds the game-thread interactive-region snapshot.

## PATTERNS (6)

### SLeafWidget subclass input override block for SVaCuusWidget (all 5.8-current signatures, drop into the existing SVaCuusHUDWidget header)
```cpp
//~ Begin SWidget input
virtual bool SupportsKeyboardFocus() const override { return true; }
virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent) override;
virtual void  OnFocusLost(const FFocusEvent& InFocusEvent) override;
virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent) override;
virtual FReply OnAnalogValueChanged(const FGeometry& MyGeometry, const FAnalogInputEvent& InAnalogInputEvent) override;
virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
virtual FReply OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent) override;
virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
virtual void  OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
virtual void  OnMouseLeave(const FPointerEvent& MouseEvent) override;
virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override;
//~ End SWidget input
```
(precedent: Signatures copied verbatim from /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:316-461 and :929; SViewport.h:106 for the SupportsKeyboardFocus override idiom.)

### Capture-on-drag / release-on-last-button-up, the exact FReply idiom the engine uses for a texture-space UI
```cpp
FReply SVaCuusWidget::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
    if (!Snapshot->IsInteractiveAt(ToUiPixels(MyGeometry, MouseEvent)))
    {
        return FReply::Unhandled();   // bubbles up to SViewport -> game input
    }
    Queue.PushButtonDown(MouseEvent, ToUiPixels(MyGeometry, MouseEvent));

    FReply Reply = FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::Mouse);
    if (MouseEvent.GetPressedButtons().Num() == 1)
    {
        Reply.CaptureMouse(SharedThis(this));
    }
    return Reply;
}

FReply SVaCuusWidget::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
    Queue.PushButtonUp(MouseEvent, ToUiPixels(MyGeometry, MouseEvent));
    FReply Reply = FReply::Handled();
    if (MouseEvent.GetPressedButtons().IsEmpty())
    {
        Reply.ReleaseMouseCapture();
    }
    return Reply;
}
```
(precedent: FWebBrowserViewport::OnMouseButtonDown/Up, /w/Unreal/UnrealEngine/Engine/Source/Runtime/WebBrowser/Private/WebBrowserViewport.cpp:52-78 (capture on first button, release when GetPressedButtons().IsEmpty()). CEF captures via MouseEvent.GetEventPath()->Widgets.Last().Widget; SharedThis(this) is equivalent and simpler for a leaf.)

### Screen -> texture-space coordinate conversion
```cpp
FIntPoint SVaCuusWidget::ToUiPixels(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
    // Local (Slate units) -> UI pixels. Must use the SAME rounding convention as
    // SVaCuusHUDWidget::ComputeWindowRect, which sizes the recorded RmlUi frame.
    const FVector2f Local = Geometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
    return FIntPoint(FMath::FloorToInt(Local.X * Geometry.Scale),
                     FMath::FloorToInt(Local.Y * Geometry.Scale));
}
```
(precedent: FCEFWebBrowserWindow::GetCefMouseEvent, /w/Unreal/UnrealEngine/Engine/Source/Runtime/WebBrowser/Private/CEF/CEFWebBrowserWindow.cpp:2637-2656 (AbsoluteToLocal then *= ViewportScaleFactor); FWidget3DHitTester, /w/Unreal/UnrealEngine/Engine/Source/Runtime/UMG/Private/Components/WidgetComponent.cpp:175 (`InGeometry.AbsoluteToLocal(Desktop) * InGeometry.Scale`).)

### IInputProcessor subclass + register/unregister lifecycle for world-space forwarding
```cpp
class FVaCuusWorldInputProcessor : public IInputProcessor, public TSharedFromThis<FVaCuusWorldInputProcessor>
{
public:
    virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}
    virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
    virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
    virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override;
    virtual const TCHAR* GetDebugName() const override { return TEXT("VaCuusWorldInput"); }
};

// register (subsystem Initialize):
FSlateApplication::Get().RegisterInputPreProcessor(Processor,
    FInputPreprocessorRegistrationKey{ EInputPreProcessorType::PreGame, /*Priority=*/ INDEX_NONE });

// unregister (subsystem Deinitialize):
if (FSlateApplication::IsInitialized())
{
    FSlateApplication::Get().UnregisterInputPreProcessor(Processor);
    Processor.Reset();
}
```
(precedent: UCommonUIActionRouterBase::RegisterAnalogCursorTick / Deinitialize, /w/Unreal/UnrealEngine/Engine/Plugins/Runtime/CommonUI/Source/CommonUI/Private/Input/CommonUIActionRouterBase.cpp:353-382 — note the FSlateApplication::IsInitialized() guard before unregistering on teardown.)

### UWidget wrapper for the Slate widget (UMG surface)
```cpp
// Header
UCLASS() class VACUUS_API UVaCuusWidget : public UWidget
{
    GENERATED_BODY()
public:
    virtual void SynchronizeProperties() override;
    virtual void ReleaseSlateResources(bool bReleaseChildren) override;
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    TSharedPtr<class SVaCuusWidget> MyVaCuusWidget;
};

// Impl
TSharedRef<SWidget> UVaCuusWidget::RebuildWidget()
{
    if (IsDesignTime()) { return SNew(SBox)[ SNew(STextBlock).Text(LOCTEXT("VaCuus", "VaCuus View")) ]; }
    MyVaCuusWidget = SNew(SVaCuusWidget).DocumentPath(DocumentPath);
    return MyVaCuusWidget.ToSharedRef();
}
void UVaCuusWidget::SynchronizeProperties()
{
    Super::SynchronizeProperties();
    if (!MyVaCuusWidget.IsValid()) { return; }
    MyVaCuusWidget->SetDocumentPath(DocumentPath);
}
void UVaCuusWidget::ReleaseSlateResources(bool bReleaseChildren)
{
    Super::ReleaseSlateResources(bReleaseChildren);
    MyVaCuusWidget.Reset();
}
```
(precedent: USpacer (/w/Unreal/UnrealEngine/Engine/Source/Runtime/UMG/Private/Components/Spacer.cpp:24-70) for the member/Reset/Super shape; UWebBrowser (/w/Unreal/UnrealEngine/Engine/Plugins/Runtime/WebBrowserWidget/Source/WebBrowserWidget/Private/WebBrowser.cpp:72-113) for the IsDesignTime() placeholder branch.)

### World-space input via ICustomHitTestPath on the game SViewport
```cpp
class FVaCuusWorldHitTester : public ICustomHitTestPath
{
    virtual TArray<FWidgetAndPointer> GetBubblePathAndVirtualCursors(
        const FGeometry& InGeometry, FVector2D DesktopSpaceCoordinate, bool bIgnoreEnabledStatus) const override;
    virtual void ArrangeCustomHitTestChildren(FArrangedChildren& ArrangedChildren) const override;
    virtual TOptional<FVirtualPointerPosition> TranslateMouseCoordinateForCustomHitTestChild(
        const SWidget& ChildWidget, const FGeometry& MyGeometry,
        const FVector2D ScreenSpaceMouseCoordinate, const FVector2D LastScreenSpaceMouseCoordinate) const override;
};

// install (share one instance across all UVaCuusWorldComponents, like WidgetComponent does):
if (TSharedPtr<SViewport> VP = FSlateApplication::Get().GetGameViewport())
{
    TSharedPtr<ICustomHitTestPath> Path = VP->GetCustomHitTestPath();
    if (!Path.IsValid()) { Path = MakeShared<FVaCuusWorldHitTester>(GetWorld()); VP->SetCustomHitTestPath(Path); }
    StaticCastSharedPtr<FVaCuusWorldHitTester>(Path)->RegisterComponent(this);
}
```
(precedent: FWidget3DHitTester + UWidgetComponent::RegisterHitTesterWithViewport / UnregisterHitTesterWithViewport, /w/Unreal/UnrealEngine/Engine/Source/Runtime/UMG/Private/Components/WidgetComponent.cpp:153-257 and :1097-1135 (refcount by registered components, SetCustomHitTestPath(nullptr) when the last one leaves).)

## PITFALLS
- SLeafWidget declares `virtual void SetVisibility(TAttribute<EVisibility>) override final` (SLeafWidget.h:35). You cannot override SetVisibility in SVaCuusWidget. Snapshot-driven hit-testability must come from a TAttribute delegate passed INTO SetVisibility, or from returning FReply::Unhandled() — not from an override.
- SWidget::SupportsKeyboardFocus() returns false in the base (SWidget.cpp:977-980). Without overriding it to true, OnKeyDown/OnKeyUp/OnKeyChar/OnFocusReceived will never fire no matter what FReply::SetUserFocus you return.
- Unhandled pointer events bubble UP the ancestor chain, never DOWN to lower-ZOrder siblings. FHittestGrid::GetBubblePath returns the path through the TOPMOST widget only (HittestGrid.h:53-58). Pass-through to the game works because SViewport is an ancestor of the overlay (GameViewportClient.cpp:1326 sets GameLayerManager as SViewport content), but a VaCuus overlay at ZOrder 100 will still block any other UMG widget beneath it in the same SOverlay.
- There is no per-pixel hit-test hook on SWidget. GetHitTestBoundingRect() (SWidget.h:717) narrows to ONE rect only. Multi-region pass-through must be implemented as: stay hit-test visible, test the point against the snapshot inside each handler, return FReply::Unhandled() to fall through. Consequence: cursor/tooltip/hover still resolve to VaCuus even over pass-through regions unless OnCursorQuery also returns FCursorReply::Unhandled().
- OnCursorQuery is `const` (SWidget.h:445). It cannot lazily rebuild or mutate cached state — the interactive-region snapshot must be readable from a const method (mutable/atomic pointer).
- OnMouseLeave takes no FGeometry (SWidget.h:430) and OnFocusLost takes no FGeometry (SWidget.h:323) — you cannot compute a coordinate there. Cache the last geometry in Tick/OnPaint if you need one.
- ISlateViewport::OnFocusReceived(const FFocusEvent&) has a DIFFERENT signature from SWidget::OnFocusReceived(const FGeometry&, const FFocusEvent&) (RenderingCommon.h:754 vs SWidget.h:316), and ISlateViewport::OnCursorQuery is non-const while SWidget's is const. Copy-pasting between the two paths will silently produce a non-overriding method.
- SViewport::OnPaint (Slate/Private/Widgets/SViewport.cpp:164-176) draws a BLACK BOX unless ISlateViewport::GetViewportRenderTargetTexture() returns a live FSlateShaderResource. VaCuus renders through FSlateDrawElement::MakeCustom + ICustomSlateElement (VaCuusSlateElement), so adopting SViewport/ISlateViewport for the screen-space path would require inventing an FSlateShaderResource you do not have. Stay on SLeafWidget.
- FReply::Handled()/Unhandled() are [[nodiscard]] (Reply.h:233, :241) — `FReply::Handled();` as a bare statement will not compile clean.
- FPointerEvent::GetScreenSpacePosition() and FGeometry::AbsoluteToLocal() return UE::Slate::FDeprecateVector2DResult, not FVector2D (Events.h:1066, Geometry.h:433). Binding them to `const FVector2D&` takes a reference to a temporary; bind by value or to FVector2f.
- Rml wheel sign is inverted relative to Slate: RmlUi documents positive wheel_delta as 'right and down' (Context.h:198), Slate's GetWheelDelta() is positive for wheel-up. Also Context::ProcessMouseWheel(float, int) at Context.h:196 is explicitly @deprecated — use the Vector2f overload.
- FCommonAnalogCursor cannot be reused: its constructor is protected and takes a `const UCommonUIActionRouterBase&` stored as a member reference, and CommonUI only registers it when GEngine->GameViewportClientClass derives from UCommonGameViewportClient (CommonUIActionRouterBase.cpp:355). Subclass plain FAnalogCursor instead — but note FAnalogCursor::UpdateCursorPosition(..., TSharedRef<ICursor>, ...) is UE_DEPRECATED(4.24); use the TSharedRef<FSlateUser> overload (AnalogCursor.h:81).
- FNavigationConfig::GetNavigationActionForKey(const FKey&) is UE_DEPRECATED(4.24) (NavigationConfig.h:86) and FSlateApplication::SetNavigationConfigFactory is UE_DEPRECATED(4.20) (SlateApplication.h:1433). Use GetNavigationActionFromKey(const FKeyEvent&) and SetNavigationConfig(TSharedRef<FNavigationConfig>). If RmlUi's nav-* properties own directional navigation, install FNullNavigationConfig while a document has focus or Slate will consume arrow/analog input before OnKeyDown.
- FSlateApplication::FindInputPreProcessor(TSharedPtr<IInputProcessor>) single-arg form is UE_DEPRECATED(5.5) (SlateApplication.h:1581). Use the (processor, EInputPreProcessorType) overload.
- IInputProcessor has no character/text hook — HandleKeyDownEvent only. IME and Unicode text must come through SWidget::OnKeyChar or ITextInputMethodContext; a preprocessor alone cannot capture typing.
- Every ITextInputMethodContext method is pure virtual, non-const, and called synchronously on the game thread (ITextInputMethodSystem.h:32-138). GetTextInRange/GetTextLength/GetTextBounds cannot be answered by asking the UI thread. Budget a game-thread mirror of the focused element's text and caret rect, published alongside the interactive-region snapshot. Module is ApplicationCore, and FSlateApplication::GetTextInputMethodSystem() can return nullptr.
- UWidget::RebuildWidget must not return SNullWidget — TakeWidget_Private ensureMsgf's on it (Widget.cpp:996) because it mutates the returned widget's state. Return SNew(SSpacer) or an SBox wrapper for the design-time/no-op branch.
- SynchronizeProperties() and OnWidgetRebuilt() only run when the Slate widget was newly created inside TakeWidget_Private (Widget.cpp:1081-1097). Anything that must re-apply on every TakeWidget() call has to be pushed by an explicit setter, not assumed.
- ReleaseSlateResources is declared on UVisual (Visual.h:17), not UWidget — and UWidget::SynchronizeProperties already stomps Enabled/Visibility/Clipping/RenderOpacity on the cached widget (Widget.cpp:1466-1503). Call Super::SynchronizeProperties() FIRST, then apply VaCuus-specific state, or the base will overwrite you.
- For input to reach a viewport-overlay widget at all in a shipped game, the player controller's input mode must be FInputModeGameAndUI or FInputModeUIOnly; under FInputModeGameOnly the game viewport holds mouse capture and Slate never routes pointer events to overlays. Flag this in the M2 demo setup.
