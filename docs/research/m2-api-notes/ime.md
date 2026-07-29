# M2 API notes: IME (ITextInputMethodContext <-> RmlUi TextInputContext)

## SUMMARY
UE 5.8.1 still exposes the classic 3-interface IME stack unchanged and non-deprecated: `ITextInputMethodSystem` (Register/Unregister/Activate/Deactivate/IsActiveContext/ApplyDefaults), `ITextInputMethodContext` (14 pure virtuals, all pull-style and synchronous), and `ITextInputMethodChangeNotifier` (4 push methods). Reach it via `FSlateApplication::Get().GetTextInputMethodSystem()` (SlateApplication.h:362, a thin forward to `PlatformApplication->GetTextInputMethodSystem()`). Two full reference implementations exist in-tree: the CEF one (`CEFImeHandler` + `FCEFTextInputMethodContext`) which is the exact shape VaCuus must replicate because it too proxies an out-of-process/off-thread text model, and the Slate one (`FSlateEditableTextLayout::FTextInputMethodContext`) which shows the "real text field" semantics. On the RmlUi side at SHA 0ae381e, RmlUi 6.x already ships the embedder hook: `Rml::TextInputHandler` (OnActivate/OnDeactivate/OnDestroy) hands you a `Rml::TextInputContext*` with GetBoundingBox/Get+SetSelectionRange/SetCursorPosition/SetText/SetCompositionRange/CommitComposition — a near 1:1 match for the engine callbacks, and `Backends/RmlUi_Platform_Win32.cpp`'s `TextInputMethodEditor_Win32` is a complete working IME editor to copy the state machine from. Two hard gaps to plan around: RmlUi's `TextInputContext` has **no** way to read the text or its length (needed by `GetTextLength`/`GetTextInRange`), and its only spatial API is the element's border box, not a caret rect — the caret comes instead from `SystemInterface::ActivateKeyboard(caret_position, line_height)`, which RmlUi calls on every `ShowCursor(true)`. Avoid: calling any RmlUi API from the game thread (all of the above run on the VaCuus UI thread per spec §4), and assuming Linux has an IME system at all — `FLinuxApplication` does not override `GetTextInputMethodSystem()`, so it returns NULL there.

## APIS (22)

### FSlateApplication::GetTextInputMethodSystem
```
ITextInputMethodSystem* GetTextInputMethodSystem() const { return PlatformApplication->GetTextInputMethodSystem(); }
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Slate/Public/Framework/Application/SlateApplication.h:362
NOTES: Non-deprecated, inline, const. Game thread only. Returns raw pointer owned by the platform application; may be nullptr. Guard with FSlateApplication::IsInitialized() when called from a destructor (precedent: SlateEditableTextLayout.cpp:249). Equivalent lower-level call: FSlateApplication::Get().GetPlatformApplication()->GetTextInputMethodSystem().

### GenericApplication::GetTextInputMethodSystem (default = no IME)
```
virtual ITextInputMethodSystem *GetTextInputMethodSystem() { return NULL; }
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/ApplicationCore/Public/GenericPlatform/GenericApplication.h:550
NOTES: Only FWindowsApplication (WindowsApplication.h:390) and FMacApplication (MacApplication.h:198) override this. FLinuxApplication does NOT — grep over ApplicationCore/{Public,Private}/Linux returns zero hits for TextInputMethodSystem. So on Linux the whole IME path is inert and must degrade to the plain OnKeyChar -> Context::ProcessTextInput route.

### ITextInputMethodSystem (full interface)
```
virtual void ApplyDefaults(const TSharedRef<FGenericWindow>& InWindow) = 0;
virtual TSharedPtr<ITextInputMethodChangeNotifier> RegisterContext(const TSharedRef<ITextInputMethodContext>& Context) = 0;
virtual void UnregisterContext(const TSharedRef<ITextInputMethodContext>& Context) = 0;
virtual void ActivateContext(const TSharedRef<ITextInputMethodContext>& Context) = 0;
virtual void DeactivateContext(const TSharedRef<ITextInputMethodContext>& Context) = 0;
virtual bool IsActiveContext(const TSharedRef<ITextInputMethodContext>& Context) const = 0;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/ApplicationCore/Public/GenericPlatform/ITextInputMethodSystem.h:176-218
NOTES: ApplyDefaults is called by Slate itself on window creation (SlateApplication.cpp:2052) — VaCuus never calls it. Register returns the notifier (may be null). IsActiveContext is const. Contexts are held by TSharedRef, so the context must be a TSharedFromThis-friendly heap object; both engine references use a private ctor + static Create() returning TSharedRef. Windows impl declared at Windows/WindowsTextInputMethodSystem.h:80-85.

### ITextInputMethodContext (every pure virtual, 5.8.1)
```
enum class ECaretPosition { Beginning, Ending };
virtual bool IsComposing() = 0;
virtual bool IsReadOnly() = 0;
virtual uint32 GetTextLength() = 0;
virtual void GetSelectionRange(uint32& OutBeginIndex, uint32& OutLength, ECaretPosition& OutCaretPosition) = 0;
virtual void SetSelectionRange(const uint32 InBeginIndex, const uint32 InLength, const ECaretPosition InCaretPosition) = 0;
virtual void GetTextInRange(const uint32 InBeginIndex, const uint32 InLength, FString& OutString) = 0;
virtual void SetTextInRange(const uint32 InBeginIndex, const uint32 InLength, const FString& InString) = 0;
virtual int32 GetCharacterIndexFromPoint(const FVector2D& InPoint) = 0;
virtual bool GetTextBounds(const uint32 InBeginIndex, const uint32 InLength, FVector2D& OutPosition, FVector2D& OutSize) = 0;
virtual void GetScreenBounds(FVector2D& OutPosition, FVector2D& OutSize) = 0;
virtual TSharedPtr<FGenericWindow> GetWindow() = 0;
virtual void BeginComposition() = 0;
virtual void UpdateCompositionRange(const int32 InBeginIndex, const uint32 InLength) = 0;
virtual void EndComposition() = 0;
virtual ~ITextInputMethodContext() = default;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/ApplicationCore/Public/GenericPlatform/ITextInputMethodSystem.h:14-139
NOTES: 14 pure virtuals, none deprecated in 5.8.1. Indices are CODE POINT indices in the platform's ACP model (Slate normalizes line breaks to a single '\n' via GIMETSFLineTerminator, SlateEditableTextLayout.cpp:28). GetTextBounds/GetScreenBounds are SCREEN(absolute-Slate)-space, and GetTextBounds returns true==clipped (CEF returns false for 'not clipped', CEFTextInputMethodContext.cpp:208). GetCharacterIndexFromPoint takes a screen-space point and returns INDEX_NONE on miss. Every one of these is a synchronous pull invoked from the platform message pump on the game thread — none of them may block on the VaCuus UI thread.

### ITextInputMethodChangeNotifier (full interface)
```
enum class ELayoutChangeType { Created, Changed, Destroyed };
virtual void NotifyLayoutChanged(const ELayoutChangeType ChangeType) = 0;
virtual void NotifySelectionChanged() = 0;
virtual void NotifyTextChanged(const uint32 BeginIndex, const uint32 OldLength, const uint32 NewLength) = 0;
virtual void CancelComposition() = 0;
virtual ~ITextInputMethodChangeNotifier() = default;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/ApplicationCore/Public/GenericPlatform/ITextInputMethodSystem.h:146-163
NOTES: Obtained from RegisterContext(); TSharedPtr, always .IsValid()-check. NotifyTextChanged takes a HALF-OPEN changed span in the same index space as GetTextLength/GetSelectionRange (see SlateEditableTextLayout.cpp:3269-3292 comment). Must NOT be called while IsComposing() is true — TSF already owns composition edits and re-reporting them double-counts indices (explicit 5.8.1 comment, SlateEditableTextLayout.cpp:3268-3272). CancelComposition is what you call before DeactivateContext when tearing down mid-composition.

### FCEFImeHandler (reference: context lifecycle owner)
```
void BindInputMethodSystem(ITextInputMethodSystem* InTextInputMethodSystem);
void UnbindInputMethodSystem();
void CacheBrowserSlateInfo(const TSharedRef<SWidget>& Widget);
void SetFocus(bool bInFocus);
void UpdateCachedGeometry(const FGeometry& AllottedGeometry);
void CEFCompositionRangeChanged(const CefRange& SelectionRange, const CefRenderHandler::RectList& CharacterBounds);
void CEFTextSelectionChanged(const CefString& SelectedText, const CefRange& SelectionRange);
private: void ActivateContext(); void DeactivateContext(); void DestroyContext();
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/WebBrowser/Private/CEF/CEFImeHandler.h:39-124
NOTES: class FCEFImeHandler : public TSharedFromThis<FCEFImeHandler>. Entire file is compiled under `#if WITH_CEF3 && !PLATFORM_LINUX` (CEFImeHandler.h:7) — Epic's own admission that Linux has no ITextInputMethodSystem. The handler owns TSharedPtr<FCEFTextInputMethodContext> + TSharedPtr<ITextInputMethodChangeNotifier>, plus two bools: bHasFocus and bContextWasActiveWhenFocusLost. This is the exact class shape VaCuus should mirror as FVaCuusImeHandler.

### FCEFTextInputMethodContext (reference: proxy context)
```
static TSharedRef<FCEFTextInputMethodContext> Create(const TSharedRef<FCEFImeHandler>& InOwner);
void AbortComposition();
bool UpdateCachedGeometry(const FGeometry& AllottedGeometry);
bool UpdateCachedSlateWindow();
bool CEFCompositionRangeChanged(const CefRange& SelectionRange, const CefRenderHandler::RectList& CharacterBounds);
bool CEFTextSelectionChanged(const CefString& SelectedText, const CefRange& SelectionRange);
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/WebBrowser/Private/CEF/CEFTextInputMethodContext.h:42-98
NOTES: Key structural lesson: it does NOT mirror the whole document. It keeps a shadow FString CompositionSequence (everything typed since the last click in the field) + FString CompositionString (last IME chunk) + int32 CompositionBeginIndex, and reports GetTextLength()==CompositionSequence.Len() (cpp:89-92). GetSelectionRange converts field-space -> composition-space by subtracting CompositionBeginIndex (cpp:96). Only IsComposing() is public; all 13 other overrides are private. Caret geometry comes from a cached std::vector<CefRect> CefCompositionBounds pushed asynchronously by the renderer process.

### FSlateEditableTextLayout::FTextInputMethodContext (reference: real text field)
```
static TSharedRef<FTextInputMethodContext> Create(FSlateEditableTextLayout& InOwnerLayout);
void CacheWindow();
inline void KillContext() { OwnerLayout = nullptr; bIsComposing = false; }
inline FTextRange GetCompositionRange() const;
bool UpdateCachedGeometry(const FGeometry& InAllottedGeometry);   // returns true if changed
// + all 14 ITextInputMethodContext overrides
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Slate/Public/Widgets/Text/SlateEditableTextLayout.h:593-645
NOTES: Holds a raw FSlateEditableTextLayout* OwnerLayout that is nulled by KillContext(); every override early-outs on !OwnerLayout. This is the correct pattern for VaCuus because the RmlUi context can die (TextInputHandler::OnDestroy) while the engine still holds the TSharedRef. Also caches TWeakPtr<SWindow> CachedParentWindow and FGeometry CachedGeometry.

### SlateEditableTextLayoutIME::ComputeMinimalChangedRange (5.8.1 helper worth copying)
```
static bool ComputeMinimalChangedRange(const FString& OldString, const FString& NewString, int32& OutBeginIndex, int32& OutOldLength, int32& OutNewLength)
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/Slate/Private/Widgets/Text/SlateEditableTextLayout.cpp:46-73
NOTES: File-static in an anonymous-ish namespace, not exported — copy it, don't link it. Common-prefix/common-suffix diff; returns false when the strings are identical so the caller can skip a degenerate NotifyTextChanged. VaCuus needs exactly this to turn 'RmlUi element value changed' into the (BeginIndex, OldLength, NewLength) triple TSF expects.

### Rml::TextInputHandler (embedder hook, vendored)
```
class RMLUICORE_API TextInputHandler : public NonCopyMoveable {
public:
  virtual ~TextInputHandler() {}
  virtual void OnActivate(TextInputContext* input_context) {}
  virtual void OnDeactivate(TextInputContext* input_context) {}
  virtual void OnDestroy(TextInputContext* input_context) {}
};
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Include/RmlUi/Core/TextInputHandler.h:18-33
NOTES: All three are non-pure no-ops. Called from WidgetTextInput::ProcessEvent on EventId::Focus (WidgetTextInput.cpp:670-677) and EventId::Blur (:685-686), and from ~WidgetTextInputContext (:80-83) — i.e. always on the RmlUi/UI thread, inside Context::Update or Context::ProcessMouse*. HEADER HAS ZERO #includes: it uses RMLUICORE_API and NonCopyMoveable without pulling Header.h/Traits.h, so it must be included after <RmlUi/Core/Types.h> or <RmlUi/Core/TextInputContext.h>.

### Rml::TextInputContext (every pure virtual, vendored)
```
virtual bool GetBoundingBox(Rectanglef& out_rectangle) const = 0;
virtual void GetSelectionRange(int& start, int& end) const = 0;
virtual void SetSelectionRange(int start, int end) = 0;
virtual void SetCursorPosition(int position) = 0;
virtual void SetText(StringView text, int start, int end) = 0;
virtual void SetCompositionRange(int start, int end) = 0;
virtual void CommitComposition(StringView composition) = 0;
virtual ~TextInputContext() {}
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Include/RmlUi/Core/TextInputContext.h:21-60
NOTES: VaCuus IMPLEMENTS TextInputHandler and CONSUMES TextInputContext (RmlUi implements it as WidgetTextInputContext). All int offsets are UTF-8 CHARACTER offsets, half-open [start, end) where end is 'the first character *after*'. SetText explicitly does NOT respect max-length; CommitComposition does. Lifetime == the element's lifetime; ends at OnDestroy(). No GetText/GetTextLength and no caret rect — see pitfalls.

### Rml::Context text-input entry points (vendored)
```
Context(const String& name, RenderManager* render_manager, TextInputHandler* text_input_handler);
bool ProcessTextInput(Character character);
bool ProcessTextInput(char character);
bool ProcessTextInput(const String& string);
TextInputHandler* GetTextInputHandler() const;
Element* GetFocusElement();
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Include/RmlUi/Core/Context.h:34,167,169,173,248,118
NOTES: Three ProcessTextInput overloads; the String one takes UTF-8 (Rml::String == std::string, Config/Config.h:108). Rml::Character is `enum class Character : char32_t` (Types.h:16) — a single Unicode code point, so a UTF-16 surrogate pair from FCharacterEvent must be recombined first (see the Win32 backend WM_CHAR case, RmlUi_Platform_Win32.cpp:258-291). All return true if NOT consumed. Context::GetFocusElement() is how you recover the ElementFormControl* behind an active TextInputContext (the context itself does not expose it).

### Rml::SetTextInputHandler / CreateContext (vendored)
```
RMLUICORE_API void SetTextInputHandler(TextInputHandler* text_input_handler);
RMLUICORE_API TextInputHandler* GetTextInputHandler();
RMLUICORE_API Context* CreateContext(const String& name, Vector2i dimensions, RenderInterface* render_interface = nullptr, TextInputHandler* text_input_handler = nullptr);
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Include/RmlUi/Core/Core.h:69,71,82-83
NOTES: Two ways to install: globally via SetTextInputHandler (must outlive Rml::Shutdown; overrides any backend handler) or per-context via the 4th CreateContext arg. VaCuus should use the per-context form so each UVaCuusSubsystem/Context owns its own handler. If null, RmlUi installs a default no-op handler (Core.cpp:123).

### Rml::SystemInterface::ActivateKeyboard / DeactivateKeyboard (the caret-rect source)
```
virtual void ActivateKeyboard(Rml::Vector2f caret_position, float line_height);
virtual void DeactivateKeyboard();
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Include/RmlUi/Core/SystemInterface.h:59,62
NOTES: Non-pure, default no-op. caret_position is in ABSOLUTE RMLUI CONTEXT PIXELS: computed as (parent->GetAbsoluteOffset() - {GetScrollLeft(), GetScrollTop()}) + cursor_position (WidgetTextInput.cpp:1613-1617). Called from WidgetTextInput::SetKeyboardActive, itself called by ShowCursor(bool) (WidgetTextInput.cpp:1159-1175), which fires on focus, every arrow key, every click, and every text edit (12 call sites, :222-:921). This is the ONLY per-caret spatial signal RmlUi gives an embedder, and VaCuus's FVaCuusSystemInterface (/w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuus/Private/VaCuusSystemInterface.h:10) currently does not override it.

### Rml::TextInputContext bounding-box source
```
static bool ElementUtilities::GetBoundingBox(Rectanglef& out_rectangle, Element* element, BoxArea area);
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Include/RmlUi/Core/ElementUtilities.h:79 (impl ElementUtilities.cpp:235-275)
NOTES: WidgetTextInputContext::GetBoundingBox calls this with BoxArea::Border (WidgetTextInput.cpp:85-88). Returns element bounds in RMLUI CONTEXT SPACE (built from element->GetAbsoluteOffset(), transform-projected), NOT screen space — VaCuus must map context px -> Slate absolute px itself. Rectanglef accessors: Position(), Size(), TopLeft(), Left/Right/Top/Bottom(), Width(), Height() (Rectangle.h:24-39). Rectanglef = Rectangle<float> (Types.h:44).

### RmlUi UTF-8 offset conversion helpers
```
RMLUICORE_API size_t StringUtilities::LengthUTF8(StringView string_view);
RMLUICORE_API int StringUtilities::ConvertCharacterOffsetToByteOffset(StringView string, int character_offset);
RMLUICORE_API int StringUtilities::ConvertByteOffsetToCharacterOffset(StringView string, int byte_offset);
RMLUICORE_API Character StringUtilities::ToCharacter(const char* p, const char* p_end);
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Include/RmlUi/Core/StringUtilities.h:97,115,118,85
NOTES: Needed because the engine speaks UTF-16 TCHAR code points and RmlUi's public offsets are UTF-8 character offsets while its internals are byte offsets. StringView ctors: (const char*,const char*), (const String&), (const String&,offset), (const String&,offset,count), plus a literal template (StringUtilities.h:127-158). StringView is non-owning — never hand it an FString's TCHAR data; convert via TCHAR_TO_UTF8 into a persisted Rml::String first.

### Rml::ElementFormControlInput selection/composition API (public, element-level)
```
String GetValue() const override;
void SetValue(const String& value) override;
void Select();
void SetSelectionRange(int selection_start, int selection_end);
void GetSelection(int* selection_start, int* selection_end, String* selected_text) const;
void SetCompositionRange(int range_start, int range_end);
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Include/RmlUi/Core/Elements/ElementFormControlInput.h:26,29,36,41,47,53
NOTES: Same character-offset semantics as TextInputContext; only applies to text/password input types. This is the escape hatch for the missing GetText: from Context::GetFocusElement() do rmlui_dynamic_cast<ElementFormControlInput*> and call GetValue()/GetSelection(). ElementFormControl::GetValue() is the pure-virtual base (ElementFormControl.h:32). Note WidgetTextInput::SetSelectionRange EARLY-RETURNS if !IsFocused() (WidgetTextInput.cpp:330-331), where IsFocused() == cursor_timer > 0 (:504-507).

### RmlUi composition rendering (visual feedback already exists)
```
void WidgetTextInput::SetCompositionRange(int range_start, int range_end);   // char offsets in, byte offsets stored
void WidgetTextInput::GetCompositionRange(int& range_start, int& range_end) const;  // returns BYTE offsets
void WidgetTextInput::GetLineIMEComposition(StringView& pre_composition, StringView& ime_composition, const String& line, int line_begin) const;
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Source/Core/Elements/WidgetTextInput.cpp:371-395, 1585-1601
NOTES: Asymmetric units: SetCompositionRange takes CHARACTER offsets and converts to bytes; GetCompositionRange returns raw BYTE offsets (documented as such in WidgetTextInput.h:64). SetCompositionRange(0,0) clears. FormatText() is called on every SetCompositionRange, so RmlUi already draws the underline for the composing span — VaCuus does not need to render composition highlighting itself.

### Rml::TextInputHandler reference implementation to copy
```
class TextInputMethodEditor_Win32 final : public Rml::TextInputHandler {
  void OnActivate/OnDeactivate/OnDestroy(Rml::TextInputContext*) override;
  bool IsComposing() const;
  void StartComposition(); void CancelComposition(); void EndComposition();
  void SetComposition(Rml::StringView); void ConfirmComposition(Rml::StringView);
  void SetCursorPosition(int cursor_pos, bool update);
 private: Rml::TextInputContext* input_context; bool composing; int cursor_pos; int composition_range_start, composition_range_end; };
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Backends/RmlUi_Platform_Win32.h:73-118 (impl RmlUi_Platform_Win32.cpp:599-730)
NOTES: The complete, working RmlUi-side IME state machine — 130 lines, copy it wholesale and swap the WM_IME_* driver for the engine's ITextInputMethodContext driver. Note its lazy composition-range capture: if (start==0 && end==0) input_context->GetSelectionRange(start,end) (cpp:705-706), and that cursor_pos == -1 means 'no cursor reported' -> select the whole composition (cpp:726-728).

### SWidget keyboard entry points (non-IME text path)
```
SLATECORE_API virtual FReply OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& InCharacterEvent);
SLATECORE_API virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);
SLATECORE_API virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);
SLATECORE_API virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent);
SLATECORE_API virtual void OnFocusLost(const FFocusEvent& InFocusEvent);
SLATECORE_API virtual bool SupportsKeyboardFocus() const;
TCHAR FCharacterEvent::GetCharacter() const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Widgets/SWidget.h:335,357,366,316,323,929 ; Input/Events.h:636
NOTES: FCharacterEvent carries a single TCHAR (UTF-16 code unit on Windows, per the struct's own doc comment at Events.h:601) — surrogate pairs arrive as two OnKeyChar calls and must be recombined into one Rml::Character. OnFocusReceived/OnFocusLost are where ActivateContext/DeactivateContext hang (Slate precedent: FSlateEditableTextLayout::EnableTextInputMethodContext at SlateEditableTextLayout.cpp:611, HandleFocusLost at :945).

### FGeometry members used by both IME references
```
const FVector2f AbsolutePosition;   // public member
inline UE::Slate::FDeprecateVector2DResult GetAbsolutePosition() const;
inline UE::Slate::FDeprecateVector2DResult GetDrawSize() const;
FORCEINLINE_DEBUGGABLE UE::Slate::FDeprecateVector2DResult AbsoluteToLocal(UE::Slate::FDeprecateVector2DParameter AbsoluteCoordinate) const;
inline UE::Slate::FDeprecateVector2DResult LocalToAbsolute(UE::Slate::FDeprecateVector2DParameter LocalCoordinate) const;
FORCEINLINE_DEBUGGABLE FSlateRect GetRenderBoundingRect() const;
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Layout/Geometry.h:618,540,504,433,446,480
NOTES: AbsolutePosition is FVector2f, so ITextInputMethodContext's FVector2D& out-params need an explicit FVector2D(...) wrap — exactly what Slate does at SlateEditableTextLayout.cpp:4217 and :4231. FDeprecateVector2DResult implicitly converts both ways; the name refers to the FVector2D->FVector2f migration, not to an actual deprecated API. VaCuus already uses GetRenderBoundingRect for its window-space rect (SVaCuusHUDWidget.cpp:60-66) — reuse that same rect as the IME coordinate basis so caret math and render compositing agree.

### FPlatformApplicationMisc::RequiresVirtualKeyboard (mobile branch)
```
static APPLICATIONCORE_API bool RequiresVirtualKeyboard();
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/ApplicationCore/Public/GenericPlatform/GenericPlatformApplicationMisc.h:127
NOTES: Slate branches on this before touching the IME system at all: if true it calls FSlateApplication::Get().ShowVirtualKeyboard(bShow, UserIndex) instead of Activate/DeactivateContext (SlateEditableTextLayout.cpp:945-957). VaCuus's handler must replicate the branch or it will register a TSF context on platforms that have none.

## PATTERNS (6)

### Engine IME callback -> RmlUi call mapping table (the deliverable). All right-hand-side calls are UI-thread-only and must be queued; all left-hand-side calls are synchronous on the game thread.
```cpp
// ENGINE (ITextInputMethodContext, game thread)   ->  RMLUI (UI thread, queued)
// IsComposing()                -> (local bool bIsComposing; no RmlUi call)
// IsReadOnly()                 -> mirrored flag from snapshot (RmlUi: element 'disabled'/'readonly' attr)
// GetTextLength()              -> mirrored: LengthUTF8(ElementFormControl::GetValue())
// GetTextInRange(b,len,&Out)   -> mirrored: substring of the shadow value (UTF16<-UTF8)
// GetSelectionRange(&b,&l,&cp) -> mirrored: TextInputContext::GetSelectionRange(start,end)
// SetSelectionRange(b,l,cp)    -> QUEUE TextInputContext::SetSelectionRange(b, b+l)
//                                 (cp==Beginning ? also SetCursorPosition(b) : SetCursorPosition(b+l))
// SetTextInRange(b,len,Str)    -> QUEUE TextInputContext::SetText(utf8(Str), b, b+len)
// BeginComposition()           -> bIsComposing=true; capture CompositionStart from mirrored selection
// UpdateCompositionRange(b,len)-> QUEUE TextInputContext::SetCompositionRange(b, b+len)
// EndComposition()             -> QUEUE TextInputContext::CommitComposition(utf8(LastCompositionStr));
//                                 then SetCompositionRange(0,0); bIsComposing=false
// (abort path)                 -> QUEUE SetText(StringView(), cstart, cend) + SetCursorPosition(cstart)
// GetTextBounds(b,len,&P,&S)   -> mirrored caret rect from SystemInterface::ActivateKeyboard(pos,h),
//                                 mapped context-px -> Slate absolute; fall back to GetBoundingBox()
// GetScreenBounds(&P,&S)       -> mirrored TextInputContext::GetBoundingBox() -> Slate absolute
// GetCharacterIndexFromPoint(P)-> INDEX_NONE (no RmlUi API; CEF also degrades, see cpp:141-157)
// GetWindow()                  -> FSlateApplication::Get().FindWidgetWindow(Widget)->GetNativeWindow()
//
// RMLUI (UI thread)                        ->  ENGINE (game thread, marshalled)
// TextInputHandler::OnActivate(ctx)        -> RegisterContext (once) + NotifyLayoutChanged(Created)
//                                             + ActivateContext
// TextInputHandler::OnDeactivate(ctx)      -> CancelComposition (if composing) + DeactivateContext
// TextInputHandler::OnDestroy(ctx)         -> DeactivateContext + UnregisterContext + KillContext
// SystemInterface::ActivateKeyboard(p,h)   -> update caret snapshot + NotifyLayoutChanged(Changed)
// element value changed (non-composing)    -> NotifyTextChanged(minimal diff via
//                                             ComputeMinimalChangedRange)
// selection changed (non-composing)        -> NotifySelectionChanged()
```
(precedent: Left column from ITextInputMethodSystem.h:14-139; right column from TextInputContext.h:28-59 + TextInputHandler.h:24-32 + SystemInterface.h:59. Degradation of GetCharacterIndexFromPoint mirrors CEFTextInputMethodContext.cpp:141-157.)

### Context lifecycle skeleton — copy verbatim from the CEF handler, swapping the CEF trigger for RmlUi's OnActivate/OnDeactivate/OnDestroy
```cpp
void FVaCuusImeHandler::ActivateContext()   // called from OnActivate marshalled to GT
{
    if (!TextInputMethodSystem) { return; }
    if (!TextInputMethodContext.IsValid())
    {
        TextInputMethodContext = FVaCuusTextInputMethodContext::Create(SharedThis(this));
        TextInputMethodChangeNotifier = TextInputMethodSystem->RegisterContext(TextInputMethodContext.ToSharedRef());
        if (TextInputMethodChangeNotifier.IsValid())
        {
            TextInputMethodChangeNotifier->NotifyLayoutChanged(ITextInputMethodChangeNotifier::ELayoutChangeType::Created);
        }
    }
    TextInputMethodContext->UpdateCachedSlateWindow();

    TSharedRef<FVaCuusTextInputMethodContext> Ref = TextInputMethodContext.ToSharedRef();
    if (!TextInputMethodSystem->IsActiveContext(Ref)) { TextInputMethodSystem->ActivateContext(Ref); }
}

void FVaCuusImeHandler::DeactivateContext()
{
    if (!TextInputMethodSystem || !TextInputMethodContext.IsValid()) { return; }
    TSharedRef<FVaCuusTextInputMethodContext> Ref = TextInputMethodContext.ToSharedRef();
    if (TextInputMethodSystem->IsActiveContext(Ref))
    {
        if (Ref->IsComposing())   // abort BEFORE deactivating, else IME calls EndComposition on a dying owner
        {
            Ref->AbortComposition();
            if (TextInputMethodChangeNotifier.IsValid()) { TextInputMethodChangeNotifier->CancelComposition(); }
        }
        TextInputMethodSystem->DeactivateContext(Ref);
    }
}
```
(precedent: Verbatim structure of FCEFImeHandler::ActivateContext / DeactivateContext, /w/Unreal/UnrealEngine/Engine/Source/Runtime/WebBrowser/Private/CEF/CEFImeHandler.cpp:56-103. DestroyContext (:105-120) adds UnregisterContext + Reset() of both pointers.)

### RmlUi-side handler: install per-context and cache the focused element for the shadow value
```cpp
class FVaCuusTextInputHandler final : public Rml::TextInputHandler
{
public:
    void OnActivate(Rml::TextInputContext* InCtx) override
    {
        ActiveContext = InCtx;
        // TextInputContext exposes no element; recover it for GetValue()/GetSelection().
        FocusedControl = rmlui_dynamic_cast<Rml::ElementFormControl*>(RmlContext->GetFocusElement());
        PublishFocusGained();          // -> game thread: RegisterContext + ActivateContext
    }
    void OnDeactivate(Rml::TextInputContext* InCtx) override
    {
        if (ActiveContext == InCtx) { ActiveContext = nullptr; FocusedControl = nullptr; PublishFocusLost(); }
    }
    void OnDestroy(Rml::TextInputContext* InCtx) override
    {
        if (ActiveContext == InCtx) { ActiveContext = nullptr; FocusedControl = nullptr; PublishContextDestroyed(); }
    }
private:
    Rml::Context* RmlContext = nullptr;              // owning VaCuus context
    Rml::TextInputContext* ActiveContext = nullptr;  // non-owning, UI thread only
    Rml::ElementFormControl* FocusedControl = nullptr;
};

// Install per context (preferred over the global Rml::SetTextInputHandler):
// Rml::Context* Ctx = Rml::CreateContext(Name, Dims, RenderInterface, &TextInputHandler);
```
(precedent: OnActivate/OnDeactivate/OnDestroy body shape from TextInputMethodEditor_Win32, RmlUi_Platform_Win32.cpp:603-618. Per-context install signature from Core.h:82-83. GetFocusElement from Context.h:118.)

### Caret rect capture — override ActivateKeyboard on the existing FVaCuusSystemInterface
```cpp
// FVaCuusSystemInterface (/w/.../VaCuus/Private/VaCuusSystemInterface.h) — add:
virtual void ActivateKeyboard(Rml::Vector2f CaretPosition, float LineHeight) override
{
    // CaretPosition is in absolute RmlUi CONTEXT pixels (element abs offset - scroll + cursor_position).
    FVaCuusCaretSnapshot Snap;
    Snap.ContextCaret = FVector2f(CaretPosition.x, CaretPosition.y);
    Snap.LineHeight   = LineHeight;
    PublishCaretSnapshot(Snap);   // -> game thread; GT maps context px -> Slate absolute px
                                  //    and fires NotifyLayoutChanged(Changed) if it moved
}
virtual void DeactivateKeyboard() override { PublishCaretHidden(); }
```
(precedent: RmlUi computes and passes exactly this at WidgetTextInput.cpp:1613-1617; it is emitted on every ShowCursor(true) (WidgetTextInput.cpp:1159-1175, 12 call sites). The GT side mirrors FCEFTextInputMethodContext::UpdateCachedGeometry -> NotifyLayoutChanged(Changed) (CEFImeHandler.cpp:199-209).)

### GetTextBounds / GetScreenBounds: context-px -> Slate absolute px, matching the widget's own rect
```cpp
bool FVaCuusTextInputMethodContext::GetTextBounds(const uint32 BeginIndex, const uint32 Length,
                                                  FVector2D& Position, FVector2D& Size)
{
    if (!CaretSnapshot.bValid)
    {
        GetScreenBounds(Position, Size);
        return true;                      // true == "clipped", i.e. we have nothing better
    }
    // CachedGeometry is the SVaCuusWidget geometry; RmlUi context px == widget local px.
    Position = FVector2D(CachedGeometry.LocalToAbsolute(FVector2D(CaretSnapshot.ContextCaret)));
    Size     = FVector2D(1.0, CaretSnapshot.LineHeight * CachedGeometry.Scale);
    return false;                          // false == not clipped
}

void FVaCuusTextInputMethodContext::GetScreenBounds(FVector2D& Position, FVector2D& Size)
{
    Position = FVector2D(CachedGeometry.AbsolutePosition);   // FVector2f -> FVector2D
    Size     = FVector2D(CachedGeometry.GetDrawSize());
}
```
(precedent: GetScreenBounds is verbatim FSlateEditableTextLayout::FTextInputMethodContext::GetScreenBounds (SlateEditableTextLayout.cpp:4222-4233). The clipped-fallback branch mirrors FCEFTextInputMethodContext::GetTextBounds (CEFTextInputMethodContext.cpp:159-209), which returns true only when it has no bounds at all. LocalToAbsolute at Geometry.h:446.)

### Never notify TSF while composing (5.8.1 makes this explicit)
```cpp
if (TextInputMethodChangeNotifier.IsValid() && TextInputMethodContext.IsValid())
{
    // TSF already drives and knows about its own composition edits; reporting them back as an
    // "external" change double-counts indices and corrupts the position model.
    if (!TextInputMethodContext->IsComposing())
    {
        int32 B = 0, OldLen = 0, NewLen = 0;
        if (VaCuusIME::ComputeMinimalChangedRange(OldShadowValue, NewShadowValue, B, OldLen, NewLen))
        {
            TextInputMethodChangeNotifier->NotifyTextChanged(B, OldLen, NewLen);
        }
    }
}
```
(precedent: SlateEditableTextLayout.cpp:3265-3292 including the 5.8.1 comment block at :3268-3272 and ComputeMinimalChangedRange at :46-73. Both strings must be in the same index space (Slate normalizes CRLF -> \n; VaCuus should compare UTF-8-decoded code point strings).)

## PITFALLS
- Linux has NO ITextInputMethodSystem. FLinuxApplication never overrides GenericApplication::GetTextInputMethodSystem(), so it returns NULL (GenericApplication.h:550; grep over ApplicationCore/*/Linux finds zero TextInputMethodSystem references). Epic's own CEF IME handler is gated `#if WITH_CEF3 && !PLATFORM_LINUX` (CEFImeHandler.h:7). Since VaCuus is being developed on Arch Linux, the entire IME path is untestable locally — plan a Windows validation step and make the Linux path degrade cleanly to OnKeyChar -> Context::ProcessTextInput.
- ITextInputMethodContext is a synchronous PULL API on the game thread; RmlUi state lives on the VaCuus UI thread (spec §4: 'No RmlUi or QuickJS API is ever called from any other thread'). GetTextLength/GetTextInRange/GetSelectionRange/GetTextBounds/GetScreenBounds/IsReadOnly must answer immediately and CANNOT block on the UI thread. They must be served from a game-thread shadow snapshot published by the UI thread; only the mutating calls (SetTextInRange, SetSelectionRange, UpdateCompositionRange, EndComposition) may be queued. This is exactly why CEF keeps its own CompositionSequence/CompositionString rather than querying the renderer (CEFTextInputMethodContext.cpp:89-139).
- Rml::TextInputContext has NO way to read the text or its length — there is no GetText/GetTextLength/GetLength on the interface (TextInputContext.h:21-60). GetTextLength() and GetTextInRange() therefore need either (a) the CEF trick of shadowing only the composition sequence, or (b) recovering the element via Context::GetFocusElement() -> ElementFormControl::GetValue() on the UI thread and publishing it. Option (b) is more correct but publishes the full field value every frame it changes.
- Rml::TextInputContext gives NO caret rect — GetBoundingBox returns the whole element BORDER box (WidgetTextInput.cpp:85-88 -> ElementUtilities::GetBoundingBox with BoxArea::Border). The only per-caret signal is SystemInterface::ActivateKeyboard(caret_position, line_height) (SystemInterface.h:59), which fires from ShowCursor(true) (WidgetTextInput.cpp:1174). If FVaCuusSystemInterface does not override ActivateKeyboard, the IME candidate window will be pinned to the element's top-left rather than following the caret.
- Coordinate spaces do not match. RmlUi's GetBoundingBox and ActivateKeyboard are in RMLUI CONTEXT PIXELS (element absolute offset, scroll-adjusted, transform-projected); ITextInputMethodContext's GetTextBounds/GetScreenBounds/GetCharacterIndexFromPoint are in SLATE ABSOLUTE (desktop) pixels. VaCuus must cache the SVaCuusWidget FGeometry (as CEF and Slate both do) and LocalToAbsolute/AbsoluteToLocal through it. For world-space UVaCuusWorldComponent there is no valid Slate-absolute mapping at all — the plan should explicitly decide to disable IME for world-space surfaces or project through the interaction ray.
- Unit mismatch inside RmlUi itself: TextInputContext offsets and WidgetTextInput::SetCompositionRange/GetSelection/SetSelectionRange take UTF-8 CHARACTER offsets, but WidgetTextInput::GetCompositionRange returns raw BYTE offsets (WidgetTextInput.h:64 says so; cpp:391-395). And the engine speaks UTF-16 TCHAR code points. Three index spaces. Use StringUtilities::ConvertCharacterOffsetToByteOffset / ConvertByteOffsetToCharacterOffset / LengthUTF8 (StringUtilities.h:115,118,97) at every boundary; a single missed conversion silently corrupts composition placement for any non-ASCII text — which is the only text IME is used for.
- Never call NotifyTextChanged/NotifySelectionChanged while IsComposing() is true. 5.8.1 added an explicit comment about this at SlateEditableTextLayout.cpp:3268-3272: TSF already owns composition edits, and reporting them back double-counts indices and corrupts the position model. Note EndComposition() runs its transaction while bIsComposing is still true, so the final commit is suppressed too — deliberately.
- Rml::TextInputContext* is a raw non-owning pointer whose lifetime ends at TextInputHandler::OnDestroy() (destructor of WidgetTextInputContext, WidgetTextInput.cpp:80-83, fired when the element is removed). The engine meanwhile holds a TSharedRef<ITextInputMethodContext> that outlives it. Follow FSlateEditableTextLayout::FTextInputMethodContext::KillContext() (SlateEditableTextLayout.h:600-604): null the back-pointer and early-out of every override, then Deactivate + Unregister. Failing to abort a live composition before teardown lets the IME call EndComposition() on a dead owner (the CEF comment at CEFImeHandler.cpp:92).
- WidgetTextInput::SetSelectionRange early-returns when !IsFocused() (WidgetTextInput.cpp:330-331), where IsFocused() is `cursor_timer > 0` (:504-507). A queued SetSelectionRange that lands on the UI thread after a blur is silently dropped. Queued IME mutations need a generation/focus token so stale ones are discarded rather than half-applied.
- Rml/Core/TextInputHandler.h has ZERO #includes yet uses RMLUICORE_API and NonCopyMoveable (Traits.h:10). It only compiles if something else was included first — include <RmlUi/Core/TextInputContext.h> (which pulls StringUtilities.h -> Header.h + Types.h) or <RmlUi/Core/Types.h> before it. The Win32 backend gets away with it only by alphabetical include order (RmlUi_Platform_Win32.h:4-8).
- TextInputContext::SetText explicitly does NOT respect the element's maxlength (documented note, TextInputContext.h:48); only CommitComposition does (WidgetTextInput.cpp:134-150). If VaCuus routes engine SetTextInRange through SetText, a maxlength'd input can be overflowed mid-composition. Both SetText and CommitComposition call ElementFormControl::SetValue, which dispatches an RmlUi change event and therefore re-enters JS — do not hold any lock across the queued call.
- Rml::Character is a single Unicode code point (`enum class Character : char32_t`, Types.h:16) but FCharacterEvent carries one UTF-16 TCHAR (Events.h:601,636). Surrogate pairs arrive as two OnKeyChar calls and must be recombined before Context::ProcessTextInput, exactly as the Win32 backend does with its `first_u16_code_unit` static (RmlUi_Platform_Win32.cpp:258-291) — which also converts '\r' to '\n' and filters non-printables (<32, ==127).
- FGeometry::AbsolutePosition is FVector2f (Geometry.h:618) while ITextInputMethodContext out-params are FVector2D&. An implicit narrowing/widening bug here is easy; both engine references write the explicit wrap `FVector2D(CachedGeometry.AbsolutePosition)` (SlateEditableTextLayout.cpp:4217,4231). The UE::Slate::FDeprecateVector2DResult return type on GetDrawSize/LocalToAbsolute is the FVector2D->FVector2f migration shim, not a deprecated API — it is the correct 5.8 form.
- On platforms where FPlatformApplicationMisc::RequiresVirtualKeyboard() is true (mobile), Slate bypasses the IME system entirely and calls FSlateApplication::Get().ShowVirtualKeyboard(bShow, UserIndex) instead (SlateEditableTextLayout.cpp:945-957). Registering a TSF context there is wrong. RmlUi's own SetKeyboardActive/ActivateKeyboard already signals the show/hide intent, so wire that to ShowVirtualKeyboard on those platforms.
- GetCharacterIndexFromPoint has no RmlUi equivalent — there is no public hit-test from a point to a character index in a text field at this SHA (ElementText exposes only GetLines() with per-line positions, ElementText.h:59, and ElementUtilities::GetStringWidth, ElementUtilities.h:58). CEF degrades by scanning its cached per-character rects (CEFTextInputMethodContext.cpp:141-157); with no such rects available, VaCuus should return INDEX_NONE and accept the (minor) loss of IME reconversion/mouse-in-candidate-window features.
