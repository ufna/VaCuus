# Materials, focus and measured layout

VaCuus 1.4 adds scalable material frames, script-controlled focus and bulk element
measurements. It also lets fingers and a physical mouse operate independently. These
recipes show where each feature fits in a game's interface.

## Keep a material's corners at the same size

Use `material-box` for inventory slots, dialogue frames and buttons whose dimensions
change with their content. It draws an Unreal UI material with Slate's nine-slice
layout: the corners keep their size while the middle stretches. `shader(key)` remains
useful when you want the material stretched over the whole element.

1. Author a material with **Material Domain = User Interface**. Put it in a
   `UVaCuusStyleSet` asset's `Materials` map under a key such as `dialogue-frame`.
2. On the game thread, call `UVaCuusSubsystem::RegisterStyleSet(StyleSet)` **before
   loading the document**. It is also available in Blueprint. Registration returns
   the number of accepted entries; check the log if an entry is refused.
3. Use that exact key in RCSS:

```css
.dialogue {
    display: block;
    box-sizing: border-box;
    width: 480dp;
    padding: 24dp;
    decorator: material-box(dialogue-frame, 48dp, 0.5);
}

.frame-only {
    decorator: material-box(dialogue-frame, 48dp, 0.5, border);
}
```

The arguments are the style key, the source brush's image size and its margins.
`48dp, 0.5` gives each corner a 24dp extent. The image size takes one length for
both axes or two lengths for width and height. Margins are fractions from 0 to 1,
with one to four numbers in CSS top/right/bottom/left order. `box` is the default;
`border` repeats the edge strips and leaves the centre undrawn. If the element is
smaller than two corners, the corners shrink to fit.

Use `dp` for both the layout and image size when they should scale together. Use
`px` for image size when the corners must stay fixed in view pixels. `border-radius`
clips the material to its paint area. `opacity` fades both `shader` and
`material-box` decorators with their element.

An existing UMG material can use the same **GetUserInterfaceUV** outputs here.
The ordinary texture coordinates carry the sliced or repeated UVs; **Normalized UV**
(slot 4) stays continuous from 0 to 1 over the whole paint box, so a fill gradient
does not restart in each slice. Customized UVs receive the sliced coordinates.

Keep the style set referenced by a cooked asset, or explicitly include its directory
in `DirectoriesToAlwaysCook` if you load it only by a config string or path.
Unregister it with `UnregisterStyleSet` when its lifetime ends. Materials must be
authored and cooked assets; a packaged game cannot compile a new `UMaterial`.

**See it:** run `vacuus.M5MatSpike` in the editor. Its lower-right group draws the
same rounded-box material as `shader`, `material-box` and `border`, on wide, tall
and small elements. The material ships at
`/VaCuus/Demo/M_VaCuusDemo_RoundedBox`. The [RCSS matrix](rcss-matrix.md#decorators-18-registered-names)
records the full argument and refusal rules, and `vacuus lint` checks declarations.

## Move the keyboard or gamepad highlight from JavaScript

Use `element.focus()` when opening a menu, restoring a selected row or moving focus
after replacing a list. It returns whether RmlUi accepted the target:

```html
<button id="resume">Resume</button>
```

```javascript
const resume = document.getElementById('resume');
if (resume) resume.focus({ focusVisible: true });
```

```css
button { display: block; tab-index: auto; }
button:focus-visible { border-color: #ffb43c; }
```

The target and its ancestors must be visible; its computed `focus` must allow focus
and its `tab-index` must be `auto`. Set it explicitly, as above: RmlUi has no browser
user-agent stylesheet to make buttons tabbable for you. For a custom row, set
`focus: auto; tab-index: auto`. A hidden, detached, destroyed or
otherwise ineligible target returns `false` and leaves the current focus in place.
The options are optional: `focus()` defaults `focusVisible` to `false`. Focusing the
already-focused element succeeds without changing its existing pseudo classes.

This moves **RmlUi's** focus. The host's keyboard routing still applies; a UMG HUD
with `bTakesKeyboardFocus = false` keeps keyboard focus in the game. Use the host's
`NavigateBack` operation to release UI focus. The exact typed JS surface is in
[`vacuus.d.ts`](../../Web/packages/cli/types/vacuus.d.ts).

## Read wrapped label sizes on the game thread

Use `vacuus-measure` when the host places nameplates, speech bubbles or other labels
and needs their dimensions after text wrapping. Give each marked element an integer
key:

```html
<div vacuus-measure="42" style="display: block; width: 180px;">
    A label long enough to wrap onto several lines
</div>
```

Read the published result from C++ on the game thread. `View` below is your
`UVaCuusView*`:

```cpp
#include "VaCuusView.h"
#include "VaCuusInteractiveSnapshot.h"

const FVaCuusInteractiveSnapshot& Snapshot = View->GetSnapshot();
if (const FIntRect* Rect = Snapshot.FindMeasuredRect(42))
{
    const FIntPoint LabelSize = Rect->Size();
    // Use LabelSize in your host's placement calculation.
}
```

For a bound list, use `data-attr-vacuus-measure="row.MeasureId"`. The snapshot carries
all marked elements in `MeasuredRects`, so a layout pass can iterate that array
instead of searching for each key.

The rectangle is the unclipped border box in **view pixels**, rounded outwards.
Wrapped inline elements include all their fragments; transformed fragments produce
an axis-aligned union. Measurements arrive one UI frame late. Give each live element
a distinct key, change its key when its content changes, and wait for the new key
before using the size. Copy a result you need to retain beyond the current snapshot.

Invisible elements, `vacuus-passthrough` subtrees and descendants of an ancestor with
an empty clip are absent. A missing key therefore means there is no result to use
yet. This API reads the published snapshot; it does not call RmlUi from the game
thread. See [gotcha 27](gotchas.md) for the distinction from `QueryElementBounds`.

## Let several fingers use the HUD at once

Buttons and `drag: clone` elements can receive independent fingers, while a connected
mouse keeps its own gesture. Each touch event carries `event.params.touch_id`,
including the first finger; mouse events omit it. A JS handler can distinguish them:

```javascript
function onClick(event) {
    const id = event.params.touch_id;
    if (id === undefined) {
        console.log('mouse click');
    } else {
        console.log('finger', id);
    }
}
```

Releasing one finger leaves another finger's press or drag active. Multiple
`drag: clone` gestures get separate visual copies. For a HUD that should leave
keyboard input in the game, set `UVaCuusWidget::bTakesKeyboardFocus` to `false`
**before its Slate widget is built**. This policy also governs a connected mouse.

Run `vacuus.DragDemo` for the shipped drag example; its source is
`Content/DevUI/drag_demo.rml` and `drag_demo.js`. For mobile setup and the remaining
device-specific input limitations, see [setup](setup.md#platforms-and-what-refusal-looks-like).
