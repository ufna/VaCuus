# M2 API notes: RmlUi input, navigation, hit-testing

## SUMMARY
At SHA 0ae381e the RmlUi input surface is exactly the RmlUi 6.x shape: `Rml::Context::Process*` for mouse/key/text/touch, `Rml::Input::KeyIdentifier` (0..176 defined, 177..250 reserved as KI_FIRST_CUSTOM_KEY..KI_LAST_CUSTOM_KEY) and a 7-bit `Rml::Input::KeyModifier` mask. Only ONE API is deprecated: `ProcessMouseWheel(float, int)` — use the `Vector2f` overload (positive = right/down, 1.0 unit == 80 dp, `UNIT_SCROLL_LENGTH` Context.cpp:28). Return values are NOT uniform: key/text/wheel return "event was NOT consumed" (i.e. false == RmlUi handled it), while mouse move/down/up/leave and all touch entry points return `!IsMouseInteracting()` — a hover/active-state hint, not consumption. For the spec §4/§8 interactive-region snapshot there is **no** RmlUi API that enumerates interactive elements: the only hit-test entry points are `Context::GetElementAtPoint` (point query, mutates private stacking caches, UI-thread only) plus `GetHoverElement`/`GetFocusElement`/`GetRootElement`. The cheapest correct snapshot is one O(N) DFS per published UI frame over `GetDocument(i)` reading the already-cached `GetAbsoluteOffset(Border)` + `GetBox().GetSize(Border)`, maintaining the clip rect inline, pruning invisible subtrees exactly as `Element::AddToStackingContext` does, and pruning any non-pass-through element that clips its own overflow — that collapses a typical HUD to tens of rects. `pointer-events`, `focus`, `tab-index` and all four `nav-*` properties plus the `nav` shorthand exist at this SHA; spatial nav is opt-in per element (nav-* is NOT inherited and the handler reads `GetLocalProperty`), lives in `ElementDocument::ProcessDefaultAction` (bubble phase), and requires something inside a document to hold focus. RmlUi core has zero gamepad support (grep returns nothing) — the embedder must synthesize KI_UP/DOWN/LEFT/RIGHT + KI_RETURN/KI_SPACE.

## APIS (36)

### Rml::Context::ProcessMouseMove
```
bool ProcessMouseMove(int x, int y, int key_modifier_state);
```
SRC: /w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/Include/RmlUi/Core/Context.h:181 (impl Source/Core/Context.cpp:580)
NOTES: x/y are integer WINDOW pixel coordinates (0,0 = top-left of the context). Updates the hover chain, dispatches mousemove/dragmove. Returns `!IsMouseInteracting()` — TRUE means the mouse is NOT over/activating any element. This is a state hint, NOT event consumption. Also implicitly re-arms the context after ProcessMouseLeave (`mouse_active = true`).

### Rml::Context::ProcessMouseButtonDown
```
bool ProcessMouseButtonDown(int button_index, int key_modifier_state);
```
SRC: Include/RmlUi/Core/Context.h:187 (impl Source/Core/Context.cpp:621)
NOTES: button_index: Left=0, Right=1, Middle=2. Only index 0 does focus/active/drag/dblclick processing. index 2 starts autoscroll if the element doesn't block it. Returns `!IsMouseInteracting()` evaluated AFTER dispatch (Context.cpp:721).

### Rml::Context::ProcessMouseButtonUp
```
bool ProcessMouseButtonUp(int button_index, int key_modifier_state);
```
SRC: Include/RmlUi/Core/Context.h:193 (impl Source/Core/Context.cpp:725)
NOTES: Captures `const bool result = !IsMouseInteracting();` at Context.cpp:732, BEFORE dispatching mouseup/click — deliberate, so a released active element still counts as capturing. Also fires `click` only if `active == FindFocusElement(hover)`; a drag end re-enters ProcessMouseMove internally.

### Rml::Context::ProcessMouseWheel (Vector2f) — PREFERRED
```
bool ProcessMouseWheel(Vector2f wheel_delta, int key_modifier_state);
```
SRC: Include/RmlUi/Core/Context.h:202 (impl Source/Core/Context.cpp:804)
NOTES: Positive values = right and DOWN. 1.0 unit == UNIT_SCROLL_LENGTH (80.f) * density_independent_pixel_ratio px (Context.cpp:28, 827). Return value: false if autoscroll was active (and gets reset), true if there is no hover element, false if an element stopped Mousescroll propagation, otherwise `target == nullptr` (true when nothing scrollable was found). Treat false as 'RmlUi consumed the wheel'.

### Rml::Context::ProcessMouseWheel (float) — DEPRECATED
```
bool ProcessMouseWheel(float wheel_delta, int key_modifier_state);
```
SRC: Include/RmlUi/Core/Context.h:196 (marked '@deprecated Please use the Vector2f version') (impl Source/Core/Context.cpp:799)
NOTES: THE ONLY @deprecated API in Context.h/Element.h/ElementDocument.h at this SHA. Simply forwards to ProcessMouseWheel(Vector2f{0.f, wheel_delta}, ...). Do not use.

### Rml::Context::ProcessMouseLeave
```
bool ProcessMouseLeave();
```
SRC: Include/RmlUi/Core/Context.h:207 (impl Source/Core/Context.cpp:839)
NOTES: Sets mouse_active=false and clears all hover state; Update() will not re-hover until the next ProcessMouseMove. Returns `!IsMouseInteracting()`. Must be sent from Slate OnMouseLeave or hover styling sticks forever.

### Rml::Context::ProcessKeyDown / ProcessKeyUp
```
bool ProcessKeyDown(Input::KeyIdentifier key_identifier, int key_modifier_state);
bool ProcessKeyUp(Input::KeyIdentifier key_identifier, int key_modifier_state);
```
SRC: Include/RmlUi/Core/Context.h:156,162 (impl Source/Core/Context.cpp:527,540)
NOTES: Dispatched to `focus` if set, else to the context root (Context.cpp:533-537). Returns 'event was NOT consumed' — i.e. FALSE means an element called StopPropagation (RmlUi handled it). Tab/arrow/enter navigation happens in ElementDocument::ProcessDefaultAction during the bubble phase, so it only runs when focus is inside a document. Note there is NO auto key repeat: send a fresh ProcessKeyDown per repeat.

### Rml::Context::ProcessTextInput (3 overloads)
```
bool ProcessTextInput(Character character);
bool ProcessTextInput(char character);
bool ProcessTextInput(const String& string);
```
SRC: Include/RmlUi/Core/Context.h:167,169,173 (impl Source/Core/Context.cpp:561,553,568)
NOTES: `Character` is `enum class Character : char32_t` (Types.h:16). The `char` overload SILENTLY RETURNS FALSE for any byte > 127 (Context.cpp:555-557). The String overload takes UTF-8 and is the one to use for UE (convert FString/TCHAR via StringCast<UTF8CHAR>). All three dispatch EventId::Textinput with parameters["text"] to focus (or root) and return 'not consumed'.

### Rml::Context::ProcessTouchStart / Move / End / Cancel
```
bool ProcessTouchMove(const TouchList& touches, int key_modifier_state);
bool ProcessTouchStart(const TouchList& touches, int key_modifier_state);
bool ProcessTouchEnd(const TouchList& touches, int key_modifier_state);
bool ProcessTouchCancel(const TouchList& touches);
```
SRC: Include/RmlUi/Core/Context.h:214,220,226,230 (impl Source/Core/Context.cpp:860,868,876,884)
NOTES: `struct Touch { TouchId identifier; Vector2f position; };` with `using TouchId = uintptr_t;` and `using TouchList = Vector<Touch>;` (Types.h:75,92,96). The list forms AND-fold the per-touch results. Internally every touch is translated to ProcessMouseMove + ProcessMouseButtonDown/Up(0) (Context.cpp:917-919, 1013-1015) plus inertia scrolling; there is NO independent touch event model. ProcessTouchCancel returns false for an unknown touch id, the others return true.

### Rml::Context::IsMouseInteracting
```
bool IsMouseInteracting() const;
```
SRC: Include/RmlUi/Core/Context.h:237 (impl Source/Core/Context.cpp:849)
NOTES: `return (hover && hover != root) || (active && active != root) || scroll_controller mode == Autoscroll;`. This is the authoritative 'UI wants the mouse' bit but is a UI-thread state derived from the LAST processed mouse position — it cannot answer an arbitrary point. Interaction ignores background/opacity; only `pointer-events: none` opts out.

### Rml::Input::KeyIdentifier
```
enum KeyIdentifier : unsigned char { KI_UNKNOWN=0, ... KI_RMETA=176, KI_FIRST_CUSTOM_KEY /*=177*/, KI_LAST_CUSTOM_KEY=250 };
```
SRC: Include/RmlUi/Core/Input.h:10-231
NOTES: Full defined list (values are stable, Win32-VK-derived): KI_UNKNOWN 0; KI_SPACE 1; KI_0..KI_9 2-11; KI_A..KI_Z 12-37; KI_OEM_1 38 (';:'), KI_OEM_PLUS 39 ('=+'), KI_OEM_COMMA 40, KI_OEM_MINUS 41, KI_OEM_PERIOD 42, KI_OEM_2 43 ('/?'), KI_OEM_3 44 ('`~'), KI_OEM_4 45 ('[{'), KI_OEM_5 46 ('\\|'), KI_OEM_6 47 (']}'), KI_OEM_7 48 ("'\""), KI_OEM_8 49, KI_OEM_102 50; KI_NUMPAD0..9 51-60, KI_NUMPADENTER 61, KI_MULTIPLY 62, KI_ADD 63, KI_SEPARATOR 64, KI_SUBTRACT 65, KI_DECIMAL 66, KI_DIVIDE 67, KI_OEM_NEC_EQUAL 68; KI_BACK 69, KI_TAB 70, KI_CLEAR 71, KI_RETURN 72, KI_PAUSE 73, KI_CAPITAL 74; IME: KI_KANA 75, KI_HANGUL 76, KI_JUNJA 77, KI_FINAL 78, KI_HANJA 79, KI_KANJI 80; KI_ESCAPE 81, KI_CONVERT 82, KI_NONCONVERT 83, KI_ACCEPT 84, KI_MODECHANGE 85; KI_PRIOR 86 (PgUp), KI_NEXT 87 (PgDn), KI_END 88, KI_HOME 89, KI_LEFT 90, KI_UP 91, KI_RIGHT 92, KI_DOWN 93, KI_SELECT 94, KI_PRINT 95, KI_EXECUTE 96, KI_SNAPSHOT 97, KI_INSERT 98, KI_DELETE 99, KI_HELP 100; KI_LWIN 101, KI_RWIN 102, KI_APPS 103, KI_POWER 104, KI_SLEEP 105, KI_WAKE 106; KI_F1..KI_F24 107-130; KI_NUMLOCK 131, KI_SCROLL 132; Fujitsu KI_OEM_FJ_JISHO 133, _MASSHOU 134, _TOUROKU 135, _LOYA 136, _ROYA 137; KI_LSHIFT 138, KI_RSHIFT 139, KI_LCONTROL 140, KI_RCONTROL 141, KI_LMENU 142 (LAlt), KI_RMENU 143 (RAlt); KI_BROWSER_BACK 144, _FORWARD 145, _REFRESH 146, _STOP 147, _SEARCH 148, _FAVORITES 149, _HOME 150; KI_VOLUME_MUTE 151, _DOWN 152, _UP 153, KI_MEDIA_NEXT_TRACK 154, _PREV_TRACK 155, _STOP 156, _PLAY_PAUSE 157, KI_LAUNCH_MAIL 158, KI_LAUNCH_MEDIA_SELECT 159, KI_LAUNCH_APP1 160, KI_LAUNCH_APP2 161; KI_OEM_AX 162, KI_ICO_HELP 163, KI_ICO_00 164, KI_PROCESSKEY 165, KI_ICO_CLEAR 166, KI_ATTN 167, KI_CRSEL 168, KI_EXSEL 169, KI_EREOF 170, KI_PLAY 171, KI_ZOOM 172, KI_PA1 173, KI_OEM_CLEAR 174, KI_LMETA 175, KI_RMETA 176. 177..250 are free for embedder-defined keys (use for gamepad buttons).

### Rml::Input::KeyModifier
```
enum KeyModifier : unsigned char { KM_CTRL = 1<<0, KM_SHIFT = 1<<1, KM_ALT = 1<<2, KM_META = 1<<3, KM_CAPSLOCK = 1<<4, KM_NUMLOCK = 1<<5, KM_SCROLLLOCK = 1<<6 };
```
SRC: Include/RmlUi/Core/Input.h:233-241
NOTES: Passed as `int key_modifier_state` (OR of the flags) to every Process* call. Context expands them into event parameters named exactly: "ctrl_key", "shift_key", "alt_key", "meta_key", "caps_lock_key", "num_lock_key", "scroll_lock_key" as int 0/1 (Context.cpp:1544-1550). Shift-Tab back-navigation reads "shift_key" (ElementDocument.cpp:581), so KM_SHIFT MUST be supplied or reverse tabbing silently breaks.

### Rml::Context::GetElementAtPoint
```
Element* GetElementAtPoint(Vector2f point, const Element* ignore_element = nullptr, Element* element = nullptr) const;
```
SRC: Include/RmlUi/Core/Context.h:128 (impl Source/Core/Context.cpp:1382)
NOTES: THE authoritative hit test. Walks `element->stacking_context` in REVERSE (front-to-back), honours modal documents, skips elements with `pointer_events() == None` (Context.cpp:1442) but only AFTER descending into children (so a `pointer-events:auto` descendant of a `none` parent IS hit), inverse-projects the point through transforms via `Element::Project`, then intersects with `ElementUtilities::GetClippingRegion`. Despite `const` it lazily rebuilds the private stacking_context cache — UI-thread only, never from the game thread.

### Rml::Context::GetHoverElement / GetFocusElement / GetRootElement
```
Element* GetHoverElement();
Element* GetFocusElement();
Element* GetRootElement();
```
SRC: Include/RmlUi/Core/Context.h:115,118,121
NOTES: Hover is the top-most element under the LAST processed mouse position, not a query. GetRootElement returns the context root (the parent of all documents) — root is deliberately excluded from IsMouseInteracting. These are the only element-level introspection entry points on Context; there is no interactive-element enumerator.

### Rml::Context::GetNumDocuments / GetDocument
```
int GetNumDocuments() const;
ElementDocument* GetDocument(int index);
ElementDocument* GetDocument(const String& id);
```
SRC: Include/RmlUi/Core/Context.h:111,109,105 (impl Source/Core/Context.cpp:439)
NOTES: GetDocument(int) == root->GetChild(index)->GetOwnerDocument(). Document stack order: HIGHER index == closer to front (PullDocumentToFront moves the child to the END of root->children, Context.cpp:468-481). Iterate index N-1 downto 0 for front-to-back snapshot order.

### Rml::Element::GetAbsoluteOffset
```
Vector2f GetAbsoluteOffset(BoxArea area = BoxArea::Content);
```
SRC: Include/RmlUi/Core/Element.h:102 (impl Source/Core/Element.cpp:359)
NOTES: Returns window-space position of the given box area, in UNTRANSFORMED space. Cached in `absolute_offset` and only recomputed when `absolute_offset_dirty` (Element.cpp:365-389), so after Context::Update() it is an O(1) field read — this is what makes the per-frame rect DFS cheap. `enum class BoxArea { Margin, Border, Padding, Content, Auto };` (Types.h:17). Non-const method.

### Rml::Element::GetBox / GetBox(index) / GetNumBoxes
```
const Box& GetBox();
const Box& GetBox(int index, Vector2f& offset);
int GetNumBoxes();
```
SRC: Include/RmlUi/Core/Element.h:125,130,~137 (impl Source/Core/Element.cpp:464,469,524)
NOTES: GetBox() is a plain `return main_box;` — no layout is triggered. `Box::GetSize(BoxArea area)` (Box.h:37) gives the size. GetNumBoxes() == 1 + additional_boxes.size(); inline/fragmented elements have >1 box, and RmlUi's own hit test unions ALL of them (Element.cpp:546-565). For block-level HUD elements the main box is sufficient.

### Rml::Element::IsPointWithinElement
```
virtual bool IsPointWithinElement(Vector2f point);
```
SRC: Include/RmlUi/Core/Element.h:156 (impl Source/Core/Element.cpp:546)
NOTES: Tests the point against the BORDER box of every one of the element's boxes (inclusive bounds on both edges). Expects a point ALREADY inverse-projected through the element's transform (Context.cpp:1447 calls Element::Project first). Virtual — custom elements may override.

### Rml::Element::IsVisible
```
bool IsVisible(bool include_ancestors = false) const;
```
SRC: Include/RmlUi/Core/Element.h:161 (impl Source/Core/Element.cpp:567)
NOTES: Default (false) returns the element's own cached `visible` flag == (display != None && visibility == Visible), recomputed on property change (Element.cpp:1856-1861). `Element::AddToStackingContext` returns early on `!IsVisible()` (Element.cpp:2390) — so pruning the whole subtree on !IsVisible() in your own DFS matches RmlUi's render/hit behaviour exactly.

### Rml::Element::GetNumChildren / GetChild
```
int GetNumChildren(bool include_non_dom_elements = false) const;
Element* GetChild(int index) const;
```
SRC: Include/RmlUi/Core/Element.h:438,433 (impl Source/Core/Element.cpp:1147,1139)
NOTES: Non-DOM children (scrollbars, form-control internals) are stored LAST in the children array; GetChild() indexes the FULL array, so with the default GetNumChildren(false) you simply never reach them. For the interactive-region snapshot pass `true` — scrollbars are genuinely interactive.

### Rml::Element::Focus / Blur / Click
```
bool Focus(bool focus_visible = false);
void Blur();
void Click();
```
SRC: Include/RmlUi/Core/Element.h:459,461,463
NOTES: Focus(true) additionally sets the `:focus-visible` pseudo class — RmlUi's own tab/nav handlers always use Focus(true) (ElementDocument.cpp:584,626+). Focus() returns false if the element is not focusable (needs computed `tab-index: auto` and no `focus: none` ancestor). Click() synthesizes a click event — this is what Enter/Space navigation uses (ElementDocument.cpp:646).

### Rml::Element attribute + class queries
```
bool HasAttribute(const String& name) const;
template<typename T> T GetAttribute(const String& name, const T& default_value) const;
const ElementAttributes& GetAttributes() const;
bool IsClassSet(const String& class_name) const;
```
SRC: Include/RmlUi/Core/Element.h:302,298,311,69 (ElementStyle::IsClassSet impl Source/Core/ElementStyle.cpp:575)
NOTES: `using ElementAttributes = Dictionary = SmallUnorderedMap<String, Variant>` (Types.h:110-111) — HasAttribute is a small-map find. IsClassSet is a linear std::find over the class vector. Both are fine per-element per-frame; attribute lookup is marginally cheaper for a single well-known marker.

### Rml::Element::GetComputedValues (relevant accessors)
```
const ComputedValues& GetComputedValues() const;
// Style::PointerEvents pointer_events() const;  Style::Focus focus() const;
// Style::TabIndex tab_index() const;            String cursor() const;
// Style::Overflow overflow_x()/overflow_y() const; Style::Display display(); Style::Visibility visibility();
```
SRC: Include/RmlUi/Core/Element.h:591; accessors Include/RmlUi/Core/ComputedValues.h:238,239,287,230,218,219,214,220
NOTES: `enum class PointerEvents : uint8_t { None, Auto };` (StyleTypes.h:119), `enum class TabIndex : uint8_t { None, Auto };` and `enum class Focus : uint8_t { None, Auto };` (StyleTypes.h:116-117), `enum class Overflow : uint8_t { Visible, Hidden, Auto, Scroll };` (StyleTypes.h:86). All are already-resolved bitfields — O(1) reads after Context::Update().

### RCSS property registration table (verified at this SHA)
```
RegisterProperty(PropertyId::PointerEvents, "pointer-events", "auto", /*inherited*/true,  /*forces_layout*/false).AddParser("keyword", "none, auto");
RegisterProperty(PropertyId::Focus,         "focus",          "auto", true,  false).AddParser("keyword", "none, auto");
RegisterProperty(PropertyId::TabIndex,      "tab-index",      "none", false, false).AddParser("keyword", "none, auto");
RegisterProperty(PropertyId::Cursor,        "cursor",         "",     true,  false).AddParser("string");
```
SRC: Source/Core/StyleSheetSpecification.cpp:386, 375, 374, 370
NOTES: pointer-events IS inherited (4th arg true) — but see the pitfall: RmlUi still evaluates it per-element after descending into children, so you cannot prune a subtree on it. `cursor` is inherited and is a free-form string.

### nav-* RCSS properties + `nav` shorthand — CONFIRMED PRESENT
```
RegisterProperty(PropertyId::NavUp,    "nav-up",    "none", /*inherited*/false, false).AddParser("keyword", "none, auto, horizontal, vertical, tree-order").AddParser("string");
// identical for nav-right / nav-down / nav-left
RegisterShorthand(ShorthandId::Nav, "nav", "nav-up, nav-right, nav-down, nav-left", ShorthandType::Box);
```
SRC: Source/Core/StyleSheetSpecification.cpp:378-382; enum Include/RmlUi/Core/StyleTypes.h:135 `enum class Nav : uint8_t { None, Auto, Horizontal, Vertical, TreeOrder };`; PropertyIds Include/RmlUi/Core/ID.h:152-155, ShorthandId::Nav ID.h:34
NOTES: Values: keyword (none|auto|horizontal|vertical|tree-order) OR a string "#element-id" (leading '#' REQUIRED, else a warning is logged and nav does nothing — ElementDocument.cpp:760-767). The `nav` shorthand is ShorthandType::Box, so CSS box order applies: `nav: <up> <right> <down> <left>`. CRITICAL: inherited == false, and the handler reads `focus_node->GetLocalProperty(property_id)` (ElementDocument.cpp:626), so nav-* must be matched by an RCSS rule on each focusable element (e.g. `button, input { nav: auto; }`). Setting it only on `body` does nothing.

### Arrow/Tab/Enter handling — Rml::ElementDocument::ProcessDefaultAction
```
void ProcessDefaultAction(Event& event) override;  // protected
```
SRC: Include/RmlUi/Core/ElementDocument.h:125 (impl Source/Core/ElementDocument.cpp:571-650)
NOTES: Runs in the BUBBLE phase of EventId::Keydown on the document. KI_TAB -> FindNextTabElement(target, !shift_key) -> Focus(true) + ScrollIntoView(Adaptive) + StopPropagation. KI_LEFT/RIGHT/UP/DOWN -> maps to PropertyId::NavLeft/NavRight/NavUp/NavDown, walks up from GetFocusLeafNode() to the nearest focusable, reads the LOCAL nav property, then FindNextNavigationElement. KI_RETURN / KI_NUMPADENTER / KI_SPACE -> if the focus leaf has computed tab-index:auto, calls focus_node->Click() and StopPropagation. Everything is gated on the document (or a descendant) holding focus.

### Rml::ElementDocument::FindNextTabElement
```
Element* FindNextTabElement(Element* current_element, bool forward, bool wrap_around = true);
```
SRC: Include/RmlUi/Core/ElementDocument.h:104 (impl Source/Core/ElementDocument.cpp:669)
NOTES: Public — usable to implement custom gamepad shoulder-button cycling without synthesizing KI_TAB. Focusability rule (private CanFocusElement, ElementDocument.cpp:30-43): visible AND computed focus() != None (else the whole subtree is skipped) AND computed tab_index() == Auto.

### Rml::ElementDocument::Show / FocusFlag
```
void Show(ModalFlag modal_flag = ModalFlag::None, FocusFlag focus_flag = FocusFlag::Auto, ScrollFlag scroll_flag = ScrollFlag::Auto);
enum class FocusFlag { None, Document, Keep, Auto };
```
SRC: Include/RmlUi/Core/ElementDocument.h:81, 22-27 (impl Source/Core/ElementDocument.cpp:334)
NOTES: FocusFlag::Auto focuses the first tab element carrying the `autofocus` attribute, else the document. Needed at load time or keyboard/gamepad nav is dead (Context::ProcessKeyDown falls back to the context root, which is not an ElementDocument and has no default action).

### Rml::ElementUtilities::GetBoundingBox
```
static bool GetBoundingBox(Rectanglef& out_rectangle, Element* element, BoxArea area);
```
SRC: Include/RmlUi/Core/ElementUtilities.h:79 (impl Source/Core/ElementUtilities.cpp:235)
NOTES: Window-space AABB that DOES project through the element's transform (early-outs to plain bounds when there is no transform, ElementUtilities.cpp:272-275). BoxArea::Auto == border box extended by outer box-shadow. Use this instead of raw GetAbsoluteOffset when the document uses `transform`; otherwise prefer the cheaper raw path.

### Rml::ElementUtilities::GetClippingRegion
```
static bool GetClippingRegion(Element* element, Rectanglei& clip_region, ClipMaskGeometryList* clip_mask_list = nullptr, bool force_clip_self = false);
```
SRC: Include/RmlUi/Core/ElementUtilities.h:66 (impl Source/Core/ElementUtilities.cpp:118)
NOTES: Walks ALL ancestors of the element on every call — O(depth) each, so calling it per element is O(N*depth). For a per-frame snapshot maintain the clip rect inline during your own top-down DFS instead (O(N)).

### Rml::Rectangle<T>
```
static Rectangle FromPositionSize(Vector2Type pos, Vector2Type size);
Vector2Type Position()/Size()/TopLeft()/BottomLeft() const; Type Left()/Top()/Width()/Height() const;
Rectangle Join(Rectangle) const; Rectangle Intersect(Rectangle) const; Rectangle IntersectIfValid(Rectangle) const;
bool Intersects(Rectangle) const; bool Contains(Vector2Type point) const;
```
SRC: Include/RmlUi/Core/Rectangle.h:19,24-39,47-65 (Rectanglef/Rectanglei aliases in Types.h)
NOTES: Members are public `p0` (min) / `p1` (max). Contains() uses inclusive bounds on both edges — matches IsPointWithinElement. `MakeInvalid()` exists for accumulating joins.

### Rml::StyleSheetSpecification::RegisterProperty (custom RCSS property)
```
static PropertyDefinition& RegisterProperty(const String& property_name, const String& default_value, bool inherited, bool forces_layout = false);
static PropertyId GetPropertyId(const String& property_name);
```
SRC: Include/RmlUi/Core/StyleSheetSpecification.h:36,70 (impl Source/Core/StyleSheetSpecification.cpp:121)
NOTES: Public API for adding an embedder property (e.g. an inherited `vacuus-input: capture|passthrough`). Asserts if the name collides with a built-in (`< PropertyId::FirstCustomId`). Custom-property values are NOT in ComputedValues; read them with `Element::GetLocalProperty(PropertyId)` (Element.h:205 -> ElementStyle.cpp:680, a flat inline+definition lookup, O(1)) and propagate inheritance yourself in the DFS. Must be called after Rml::Initialise and before loading documents.

### Rml::SystemInterface cursor / clipboard / IME hooks
```
virtual void SetMouseCursor(const String& cursor_name);
virtual void SetClipboardText(const String& text);
virtual void GetClipboardText(String& text);
virtual void ActivateKeyboard(Rml::Vector2f caret_position, float line_height);
virtual void DeactivateKeyboard();
```
SRC: Include/RmlUi/Core/SystemInterface.h:46,50,54,59,62
NOTES: SetMouseCursor is pushed from inside Context::Update's hover-chain update (Context.cpp:1315-1327): autoscroll cursor > drag element's computed cursor() > hover element's computed cursor(); only called when the name changes; gated by Context::EnableMouseCursor (Context.h:91, default on; initial name is ":reset:"). Capture it in FVaCuusSystemInterface (currently only overrides GetElapsedTime/LogMessage at /w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuus/Private/VaCuusSystemInterface.h:16-17) and put the string into the published snapshot.

### Rml::TextInputHandler / Rml::TextInputContext (IME)
```
class TextInputHandler { virtual void OnActivate(TextInputContext*); virtual void OnDeactivate(TextInputContext*); virtual void OnDestroy(TextInputContext*); };
class TextInputContext { virtual bool GetBoundingBox(Rectanglef& out) const = 0; virtual void GetSelectionRange(int& start, int& end) const = 0; virtual void SetSelectionRange(int,int) = 0; virtual void SetCursorPosition(int) = 0; virtual void SetText(StringView, int start, int end) = 0; virtual void SetCompositionRange(int,int) = 0; virtual void CommitComposition(StringView) = 0; };
```
SRC: Include/RmlUi/Core/TextInputHandler.h:18-33; Include/RmlUi/Core/TextInputContext.h:23-59
NOTES: Install per-context via `Rml::CreateContext(name, dimensions, render_interface, TextInputHandler*)` (Core.h:82-83) or globally via `Rml::SetTextInputHandler` (Core.h:69). GetBoundingBox gives the caret rect that ITextInputMethodContext::GetTextBounds needs. All callbacks fire on the UI thread — they must marshal to the game thread.

### UE 5.8.1 FKeyEvent / FCharacterEvent / FInputEvent
```
FKey GetKey() const;            // Events.h:471
uint32 GetCharacter() const;    // Events.h:481 (FKeyEvent)
uint32 GetKeyCode() const;      // Events.h:491 (platform virtual key code)
bool IsRepeat() const;          // Events.h:210
bool IsShiftDown()/IsControlDown()/IsAltDown()/IsCommandDown()/AreCapsLocked() const; // Events.h:220,244,268,292,316
const FModifierKeysState& GetModifierKeys() const; // Events.h:324
TCHAR GetCharacter() const;     // Events.h:636 (FCharacterEvent)
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Input/Events.h:210,220,244,268,292,316,324,471,481,491,636
NOTES: Map from FKeyEvent::GetKey() (FKey, stable across platforms) rather than GetKeyCode() (platform VK, differs on Linux/Mac). FCharacterEvent::GetCharacter() is a single TCHAR (UTF-16 unit on Win) — accumulate surrogate pairs or convert via FString before Rml::Context::ProcessTextInput(const String&). Note FInputEvent has no NumLock/ScrollLock accessor, so KM_NUMLOCK/KM_SCROLLLOCK are best left unset.

### UE 5.8.1 EKeys (FKey table to map from)
```
struct EKeys { static INPUTCORE_API const FKey AnyKey, MouseX, MouseY, LeftMouseButton, RightMouseButton, MiddleMouseButton, ThumbMouseButton, ThumbMouseButton2, BackSpace, Tab, Enter, Pause, CapsLock, Escape, SpaceBar, PageUp, PageDown, End, Home, Left, Up, Right, Down, Insert, Delete, Zero..Nine, A..Z, NumPadZero..NumPadNine, Multiply, Add, Subtract, Decimal, Divide, F1..F12, NumLock, ScrollLock, LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt, LeftCommand, RightCommand, Semicolon, Equals, Comma, Underscore, Hyphen, Period, Slash, Tilde, LeftBracket, Backslash, RightBracket, Apostrophe, Ampersand, Asterix, Caret, Colon, Dollar, Exclamation, LeftParantheses, RightParantheses, Quote, A_AccentGrave, E_AccentGrave, E_AccentAigu, C_Cedille, Section, Platform_Delete, Gamepad_*; };
```
SRC: /w/Unreal/UnrealEngine/Engine/Source/Runtime/InputCore/Classes/InputCoreTypes.h:345-545
NOTES: Gaps vs KI_*: UE has no F13..F24, no Print/Snapshot/Apps/Win-key entries in the base set (LeftCommand/RightCommand -> KI_LMETA/KI_RMETA), and its IME keys are absent. UE-only keys with no KI_ equivalent (Ampersand, Asterix, Caret, Colon, Dollar, Exclamation, parens, Quote, accented keys, Section) come from AZERTY/localized layouts and should map to KI_UNKNOWN with the character delivered via ProcessTextInput instead. Gamepad_* keys have NO RmlUi equivalent — synthesize KI_UP/DOWN/LEFT/RIGHT + KI_RETURN/KI_SPACE, or use KI_FIRST_CUSTOM_KEY+n (177..250).

## PATTERNS (6)

### Interactive-region snapshot: the recommended O(N) per-frame DFS (UI thread, after Context::Update(), before publish). Emits ONLY interactive rects; absence of coverage == pass-through.
```cpp
// Clip is window-space; bPassThrough is inherited down the tree.
void CollectRegions(Rml::Element* E, FIntRect Clip, bool bPassThrough, TArray<FIntRect>& Out)
{
    if (!E->IsVisible()) return;                                   // == AddToStackingContext (Element.cpp:2390)
    if (E->HasAttribute("vacuus-passthrough")) bPassThrough = true;
    else if (E->HasAttribute("vacuus-capture")) bPassThrough = false;

    const Rml::Vector2f P = E->GetAbsoluteOffset(Rml::BoxArea::Border);   // cached field read
    const Rml::Vector2f S = E->GetBox().GetSize(Rml::BoxArea::Border);    // `return main_box;`
    const FIntRect R = Clip.Intersect(FIntRect(FMath::FloorToInt(P.x), FMath::FloorToInt(P.y),
                                               FMath::CeilToInt(P.x+S.x), FMath::CeilToInt(P.y+S.y)));
    if (R.Area() <= 0) return;                                     // fully clipped -> whole subtree gone

    const auto& CV = E->GetComputedValues();
    const bool bHit = !bPassThrough && CV.pointer_events() != Rml::Style::PointerEvents::None;
    const bool bClips = CV.overflow_x() != Rml::Style::Overflow::Visible
                     || CV.overflow_y() != Rml::Style::Overflow::Visible;
    if (bHit) Out.Add(R);
    if (bHit && bClips) return;                                    // descendants are a subset of R -> prune

    const FIntRect ChildClip = bClips ? R : Clip;
    for (int i = 0, N = E->GetNumChildren(/*include_non_dom_elements*/ true); i < N; ++i)
        CollectRegions(E->GetChild(i), ChildClip, bPassThrough, Out);
}
// Driver: for (int i = Ctx->GetNumDocuments()-1; i >= 0; --i) CollectRegions(Ctx->GetDocument(i), Viewport, /*body default*/ true, Rects);
```
(precedent: Mirrors Element::AddToStackingContext visibility pruning (Source/Core/Element.cpp:2386-2391), Context::GetElementAtPoint's pointer-events check (Source/Core/Context.cpp:1442) and its clip-region intersection (Context.cpp:1452-1456), but replaces the O(depth) ElementUtilities::GetClippingRegion ancestor walk (ElementUtilities.cpp:118-150) with an inline accumulated clip. Cost: one pass over the live element count, all reads O(1) post-Update (absolute_offset cached at Element.cpp:365-389, main_box a plain member at Element.cpp:464). Rect count after the clip-container prune is typically tens for a HUD; game-thread test is a linear scan (a few hundred ns) guarded by a union-bounds early-out.)

### Game-thread synchronous Handled/Unhandled against the published (stale-by-one-UI-frame) snapshot
```cpp
struct FVaCuusRegionSnapshot   // POD, triple-buffered, published with the command buffer
{
    TArray<FIntRect> Rects;    // interactive only, window px, pre-clipped, front-to-back doc order
    FIntRect          Bounds;  // union of Rects, early-out
    uint32            CursorNameId = 0;   // from SystemInterface::SetMouseCursor
    uint64            UIFrame = 0;
};

bool FVaCuusRegionSnapshot::HitTest(FIntPoint P) const
{
    if (!Bounds.Contains(P)) return false;
    for (const FIntRect& R : Rects) if (R.Contains(P)) return true;
    return false;
}

// SVaCuusWidget::OnMouseButtonDown
FReply SVaCuusWidget::OnMouseButtonDown(const FGeometry& G, const FPointerEvent& E)
{
    const FIntPoint Local = ToContextPx(G, E.GetScreenSpacePosition());
    Queue.EnqueueMouseDown(Local, ToRmlButton(E.GetEffectingButton()), ToRmlModifiers(E));
    return Snapshot->HitTest(Local) ? FReply::Handled().CaptureMouse(SharedThis(this))
                                    : FReply::Unhandled();
}
```
(precedent: Spec §4 (UI thread -> game thread interactive-region snapshot) and §8 (Slate FReply route). The event is ALWAYS queued regardless of the FReply, because the authoritative hit test is Context::GetElementAtPoint on the UI thread (Source/Core/Context.cpp:1382).)

### Modifier + button mapping from Slate to Rml::Input
```cpp
static int ToRmlModifiers(const FInputEvent& E)
{
    int M = 0;
    if (E.IsControlDown()) M |= Rml::Input::KM_CTRL;
    if (E.IsShiftDown())   M |= Rml::Input::KM_SHIFT;   // required for shift-Tab
    if (E.IsAltDown())     M |= Rml::Input::KM_ALT;
    if (E.IsCommandDown()) M |= Rml::Input::KM_META;
    if (E.AreCapsLocked()) M |= Rml::Input::KM_CAPSLOCK;
    return M;   // FInputEvent exposes no NumLock/ScrollLock state -> leave those bits clear
}

static int ToRmlButton(const FKey& K)
{
    if (K == EKeys::LeftMouseButton)   return 0;
    if (K == EKeys::RightMouseButton)  return 1;
    if (K == EKeys::MiddleMouseButton) return 2;
    return -1;   // ThumbMouseButton*: RmlUi has no mapping; drop or use 3+ (ignored by Context)
}
```
(precedent: Backends/RmlUi_Platform_SDL.cpp:486 (GetKeyModifierState) and RmlUi_Platform_Win32.cpp; UE side /w/Unreal/UnrealEngine/Engine/Source/Runtime/SlateCore/Public/Input/Events.h:220-324. Modifier names consumed by Context::GenerateKeyModifierEventParameters (Source/Core/Context.cpp:1544-1550).)

### FKey -> Rml::Input::KeyIdentifier map (build once as a TMap<FKey, Rml::Input::KeyIdentifier>)
```cpp
// exhaustive for the KI_ set UE can produce; everything else -> KI_UNKNOWN + ProcessTextInput
{EKeys::BackSpace,KI_BACK},{EKeys::Tab,KI_TAB},{EKeys::Enter,KI_RETURN},{EKeys::Pause,KI_PAUSE},
{EKeys::CapsLock,KI_CAPITAL},{EKeys::Escape,KI_ESCAPE},{EKeys::SpaceBar,KI_SPACE},
{EKeys::PageUp,KI_PRIOR},{EKeys::PageDown,KI_NEXT},{EKeys::End,KI_END},{EKeys::Home,KI_HOME},
{EKeys::Left,KI_LEFT},{EKeys::Up,KI_UP},{EKeys::Right,KI_RIGHT},{EKeys::Down,KI_DOWN},
{EKeys::Insert,KI_INSERT},{EKeys::Delete,KI_DELETE},{EKeys::Platform_Delete,KI_DELETE},
{EKeys::Zero,KI_0}/*..Nine->KI_9*/, {EKeys::A,KI_A}/*..Z->KI_Z*/,
{EKeys::NumPadZero,KI_NUMPAD0}/*..NumPadNine->KI_NUMPAD9*/,
{EKeys::Multiply,KI_MULTIPLY},{EKeys::Add,KI_ADD},{EKeys::Subtract,KI_SUBTRACT},
{EKeys::Decimal,KI_DECIMAL},{EKeys::Divide,KI_DIVIDE},
{EKeys::F1,KI_F1}/*..F12->KI_F12*/,{EKeys::NumLock,KI_NUMLOCK},{EKeys::ScrollLock,KI_SCROLL},
{EKeys::LeftShift,KI_LSHIFT},{EKeys::RightShift,KI_RSHIFT},
{EKeys::LeftControl,KI_LCONTROL},{EKeys::RightControl,KI_RCONTROL},
{EKeys::LeftAlt,KI_LMENU},{EKeys::RightAlt,KI_RMENU},
{EKeys::LeftCommand,KI_LMETA},{EKeys::RightCommand,KI_RMETA},
{EKeys::Semicolon,KI_OEM_1},{EKeys::Equals,KI_OEM_PLUS},{EKeys::Comma,KI_OEM_COMMA},
{EKeys::Hyphen,KI_OEM_MINUS},{EKeys::Underscore,KI_OEM_MINUS},{EKeys::Period,KI_OEM_PERIOD},
{EKeys::Slash,KI_OEM_2},{EKeys::Tilde,KI_OEM_3},{EKeys::LeftBracket,KI_OEM_4},
{EKeys::Backslash,KI_OEM_5},{EKeys::RightBracket,KI_OEM_6},{EKeys::Apostrophe,KI_OEM_7},
```
(precedent: Value-for-value consistent with Backends/RmlUi_Platform_Win32.cpp:397+ RmlWin32::ConvertKey (Win32 VK -> KI_), which is the canonical mapping RmlUi ships. UE FKey names from /w/Unreal/UnrealEngine/Engine/Source/Runtime/InputCore/Classes/InputCoreTypes.h:345-545.)

### Text input + the Enter/newline special case (RmlUi does NOT synthesize it)
```cpp
// SVaCuusWidget::OnKeyChar -> queue; UI thread does:
Ctx->ProcessTextInput(Rml::String(TCHAR_TO_UTF8(*CharsAsFString)));   // UTF-8 String overload

// SVaCuusWidget::OnKeyDown -> queue; UI thread does:
bool bNotConsumed = Ctx->ProcessKeyDown(KI, Mods);
if (KI == Rml::Input::KI_RETURN || KI == Rml::Input::KI_NUMPADENTER)
    bNotConsumed &= Ctx->ProcessTextInput('\n');   // required for <textarea>-style multiline
// bNotConsumed == false  =>  an element called StopPropagation (RmlUi handled the key)
```
(precedent: Backends/RmlUi_Platform_SDL.cpp:207-219 does exactly this (ProcessKeyDown then `result &= context->ProcessTextInput('\n')` on Return). The char overload is safe here because '\n' <= 127; anything above 127 is silently dropped (Source/Core/Context.cpp:553-558).)

### Gamepad navigation: what the embedder must feed (RmlUi core has NO gamepad support)
```cpp
// Per-context, once at startup (nav-* is NOT inherited -> must match each focusable element):
//   RCSS:  button, input, select, .vc-focusable { nav: auto; tab-index: auto; }
//   or explicit graph:  #play { nav-down: "#options"; nav-up: "#quit"; }

// Repeat-throttled DPad / left-stick -> arrow keys; face buttons -> activate:
switch (GamepadKey) {
  case EKeys::Gamepad_DPad_Up:    Ctx->ProcessKeyDown(Rml::Input::KI_UP, 0);    break;
  case EKeys::Gamepad_DPad_Down:  Ctx->ProcessKeyDown(Rml::Input::KI_DOWN, 0);  break;
  case EKeys::Gamepad_DPad_Left:  Ctx->ProcessKeyDown(Rml::Input::KI_LEFT, 0);  break;
  case EKeys::Gamepad_DPad_Right: Ctx->ProcessKeyDown(Rml::Input::KI_RIGHT, 0); break;
  case EKeys::Gamepad_FaceButton_Bottom: Ctx->ProcessKeyDown(Rml::Input::KI_RETURN, 0); break;
  case EKeys::Gamepad_LeftShoulder:  if (auto* D = FocusedDoc()) D->FindNextTabElement(D->GetFocusLeafNode(), false)->Focus(true); break;
  case EKeys::Gamepad_RightShoulder: if (auto* D = FocusedDoc()) D->FindNextTabElement(D->GetFocusLeafNode(), true )->Focus(true); break;
  // optional: expose raw buttons to RCSS/JS as Rml::Input::KeyIdentifier(KI_FIRST_CUSTOM_KEY + n)
}
// Prerequisite once per document: Doc->Show(ModalFlag::None, FocusFlag::Document);
```
(precedent: Nav dispatch lives in Source/Core/ElementDocument.cpp:593-650 (arrows) and 581-591 (Tab); FindNextTabElement is public at Include/RmlUi/Core/ElementDocument.h:104; KI_FIRST_CUSTOM_KEY..KI_LAST_CUSTOM_KEY reserved at Include/RmlUi/Core/Input.h:229-230. Zero hits for 'gamepad|joystick' across Include/ and Source/Core/.)

## PITFALLS
- Return values are NOT uniform and must not be collapsed into one 'handled' concept. ProcessKeyDown/ProcessKeyUp/ProcessTextInput/ProcessMouseWheel(Vector2f) return 'the event was NOT consumed' (false == RmlUi handled it, via StopPropagation). ProcessMouseMove/ButtonDown/ButtonUp/MouseLeave and all four ProcessTouch* return `!IsMouseInteracting()` — a hover/active STATE hint, not consumption. Sources: Context.h:155,161,166,172,180,186,192,201,205,213 and Context.cpp:849.
- ProcessMouseWheel(float, int) is the ONLY @deprecated declaration in Context.h (line 195-196). Use the Vector2f overload. Sign convention: positive = right and DOWN; UE's FPointerEvent::GetWheelDelta() is positive = scroll UP, so pass Vector2f(0.f, -Delta). Magnitude: 1.0 == 80 dp (UNIT_SCROLL_LENGTH, Context.cpp:28,827) — do not pass raw pixels.
- ProcessTextInput(char) silently returns false for any byte > 127 (Context.cpp:553-557). Never route UE's TCHAR through it. Use the `const String&` (UTF-8) overload, or cast to `Rml::Character` (enum class : char32_t, Types.h:16) after combining UTF-16 surrogate pairs — a raw TCHAR cast on Windows corrupts anything above the BMP.
- RmlUi does NOT synthesize a newline text-input from KI_RETURN. Multiline text controls stay empty unless the embedder also calls ProcessTextInput('\n') after ProcessKeyDown(KI_RETURN/KI_NUMPADENTER) — exactly what the SDL backend does (Backends/RmlUi_Platform_SDL.cpp:207-210).
- There is NO document-level or context-level API to enumerate interactive elements at this SHA. Include/RmlUi/ contains exactly one hit-test entry point (Context::GetElementAtPoint, Context.h:128) plus GetHoverElement/GetFocusElement/GetRootElement. `Element::stacking_context` and BuildLocalStackingContext are PRIVATE (Element.h:671-674), so an exactly-RmlUi-ordered painter's rect list is not buildable from public API. Design pass-through as a subtree opt-out (absence of coverage), NOT as an occluder that hides interactive rects beneath it — a pass-through element floating on top of a button will still report the button as interactive.
- `pointer-events` IS an inherited property (StyleSheetSpecification.cpp:386, 4th arg true) but RmlUi checks it PER ELEMENT only AFTER recursing into children (Context.cpp:1442, after the stacking-context loop at 1405-1439). A descendant with `pointer-events: auto` inside a `pointer-events: none` parent IS still hit. Do NOT prune a subtree on pointer-events:none in the snapshot DFS — check it per element only.
- nav-* is NOT inherited (StyleSheetSpecification.cpp:378-381, inherited=false) and ElementDocument reads it with `GetLocalProperty(property_id)` (ElementDocument.cpp:626), which sees only inline styles + the element's own matched RCSS definition. `body { nav: auto; }` does nothing for descendants. Every focusable element needs a matching rule (e.g. `button, input { nav: auto; tab-index: auto; }`). String form requires a leading '#': `nav-down: "#options"` — without it a warning is logged and navigation is a no-op (ElementDocument.cpp:760-767).
- Keyboard/nav is dead unless focus is inside a document. Context::ProcessKeyDown dispatches to `focus` or falls back to the context ROOT (Context.cpp:533-537); the root is not an ElementDocument, so ElementDocument::ProcessDefaultAction (which owns Tab/arrow/Enter) never runs. Call `Doc->Show(ModalFlag::None, FocusFlag::Document)` or `Element::Focus(true)` at load. Focusability requires computed `tab-index: auto` AND no `focus: none` ancestor (CanFocusElement, ElementDocument.cpp:30-43) — `focus: none` prunes the entire subtree from tab and spatial nav.
- Shift-Tab reads the event parameter "shift_key" (ElementDocument.cpp:581), which is only populated if you OR KM_SHIFT into key_modifier_state. Forgetting the modifier bits silently breaks reverse tabbing while forward tabbing works.
- RmlUi has ZERO gamepad support at this SHA (grep for gamepad/joystick across Include/ and Source/Core/ returns nothing). All pad navigation must be synthesized by the embedder into KI_UP/DOWN/LEFT/RIGHT + KI_RETURN/KI_NUMPADENTER/KI_SPACE (the Enter/Space handler calls focus_node->Click(), ElementDocument.cpp:641-648) or driven directly via the public ElementDocument::FindNextTabElement + Element::Focus(true). Analog sticks need embedder-side repeat throttling and a dead zone — RmlUi does no key repeat of its own.
- The DFS must pass `GetNumChildren(/*include_non_dom_elements=*/true)`. Non-DOM children (scrollbars, form-control internals) are stored LAST in the children array and GetChild() indexes the full array (Element.cpp:1139-1149), so the default `false` silently drops genuinely interactive scrollbars from the snapshot.
- `Element::GetBox()` returns only the MAIN box. Inline / fragmented elements carry additional boxes and RmlUi's own IsPointWithinElement unions all of them (Element.cpp:546-565, GetNumBoxes at 524). Snapshotting only the main box under-reports text-heavy inline content; if that matters, loop `GetBox(i, offset)` for i in [0, GetNumBoxes()).
- GetAbsoluteOffset returns UNTRANSFORMED coordinates (Element.cpp:359-362). Any document using `transform` will produce wrong snapshot rects; RmlUi's hit test compensates by inverse-projecting the point (Element::Project, Element.h:236). For transformed subtrees switch to ElementUtilities::GetBoundingBox(out, element, BoxArea::Border) (ElementUtilities.cpp:235-275), which projects the 4 corners — noticeably more expensive, so gate it on `element->GetTransformState() != nullptr`.
- Do not call ElementUtilities::GetClippingRegion per element: it walks every ancestor on each call (ElementUtilities.cpp:128-150, plus GetClientWidth/GetScrollWidth per level), turning an O(N) sweep into O(N*depth). Accumulate the clip rect top-down in your own DFS instead.
- Everything here is UI-thread-only. Element::GetBox/GetAbsoluteOffset/GetNumChildren are NON-const and lazily refresh caches; Context::GetElementAtPoint is declared const yet rebuilds the private stacking_context (Context.cpp:1407-1408). The snapshot published to the game thread must be a fully detached value type (rects + cursor id), never Element*/Context* pointers. Spec §4's `check(IsInUIThread())` wrappers must cover these read-only-looking calls too.
- Slate must send ProcessMouseLeave on OnMouseLeave, otherwise hover state and :hover styling stick permanently; after it the context ignores hover until the next ProcessMouseMove re-arms `mouse_active` (Context.cpp:583-585, 839-846).
- `data-vacuus-passthrough` is inert but for a subtle reason worth knowing: ElementUtilities::ApplyDataViewsControllers parses any `data-<type>-<modifier>` attribute (ElementUtilities.cpp:407-424), so it would try type_name="vacuus" and get null from Factory::InstanceDataView/InstanceDataController — silently ignored, no warning. Safe, but a plain attribute (`vacuus-passthrough`) or a registered custom RCSS property (StyleSheetSpecification::RegisterProperty, StyleSheetSpecification.h:36 — asserts on collision with a built-in name, .cpp:124) is cleaner and, if registered inherited, gives selector-based authoring with the inheritance already resolved.
- Cursor shape is push-based, not pull-based: RmlUi calls SystemInterface::SetMouseCursor from inside Context::Update's hover-chain pass, and ONLY when the name changes (Context.cpp:1324-1327), gated by Context::EnableMouseCursor (default enabled, initial value ":reset:"). FVaCuusSystemInterface currently overrides only GetElapsedTime/LogMessage (/w/Unreal/VcHost/Plugins/VaCuus/Source/VaCuus/Private/VaCuusSystemInterface.h:16-17) — add SetMouseCursor and latch the name into the frame's snapshot, otherwise §8's 'cursor shape comes from the same snapshot' has no source.
- Touch is not an independent event model: every touch is internally converted into ProcessMouseMove + ProcessMouseButtonDown/Up(button 0) (Context.cpp:917-919, 1013-1015). Sending both real mouse events and touch events into the same context will fight over hover/active state. ProcessTouchStart asserts (RMLUI_ASSERTMSG) if a touch id is already active — the game thread ring buffer must guarantee start/end pairing across the thread hop.
- The spatial-nav heuristic truncates distances to int and multiplies cross-axis error by CrossAxisFactor = 10'000 (ElementDocument.cpp:50-70). Practically fine, but it means nav decisions are computed on border-box AABBs in untransformed px — transformed or rotated menus will navigate by their untransformed layout positions.
