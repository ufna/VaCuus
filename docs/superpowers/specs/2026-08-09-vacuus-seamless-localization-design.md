# Seamless language switching

**Date:** 2026-08-09
**Status:** design approved, implementation pending
**Beads:** VaCuus-cii (buyer doc, filed 2026-08-09)

## The problem

Localization ships and is tested (`VaCuus.Js.Translate`), but switching language costs a
full document reload, and reload re-mounts JS from module top level — fresh context, fresh
state (`VaCuusJsWebDxTest.cpp`, case E-P7). Open tab, scroll position, timers: gone. A
player changes language in a settings menu, which is exactly the screen that must survive
the change.

Everything about switching sits on the project side today: build the table, decide when to
switch, re-push models carrying `FText`, ship a font, repaint. The plugin owns only the
table registry and its two readers.

## The dividing line

`FVaCuusUIThread::Enqueue(FVaCuusUICommand&&)` is at `VaCuusUIThread.h:419`, below
`private:` at `:372`. Only the named `Enqueue*` doors are public. Game code has no way —
and must never have a way — to run arbitrary code on the UI thread.

That draws the line without judgement calls:

> **Anything that requires the UI thread is the plugin's job, because no project can build
> it. Anything that lives on the game thread out of the project's own data stays the
> project's job.**

Owned by the plugin after this work: the live re-translation route, the two change signals,
the runtime font door. Left to the project: which strings, which fonts, when to switch, and
re-pushing its own models.

## Non-goals, decided rather than deferred

- **Retroactive re-translation of already-parsed markup.** `Factory::InstanceElementText`
  (`Factory.cpp:330-336`) translates and discards the key; the DOM holds only the result.
  Reversing translated→key is ambiguous and falls apart on data expressions. Rejected.
- **`{n}` params on the live route.** `{{ t.key }}` is a data expression; parameters live
  naturally beside it in the model (`{{ t.kills_label }} {{ hud.kills }}`). Real
  interpolation stays with `vacuus.translate(key, params)` from JS.
- **Plugin-side subscription to `FInternationalization::OnCultureChanged`**, except the
  explicitly opt-in config bridge below. When to switch is game policy.
- **Replacing `@font-face`.** The runtime door adds to it; RCSS stays the authoring route.

## 1. The live route

### Forced shape

The obvious design — a separate `i18n` data model — does not work. `Element.cpp:2203-2218`:
an element inherits `data_model` from its parent unless it declares `data-model="name"`,
which switches the **whole subtree** to that model. One model per subtree. Inside
`<body data-model="hud">`, `{{ i18n.x }}` resolves against `hud` and finds nothing.

So translation is **an extra top-level variable in every data model VaCuus creates**, not a
model of its own.

### Mechanism

- A custom `VariableDefinition` (`DataVariable.h:46-57`; `Child()` at `:55` is virtual)
  registered through `DataModelConstructor::RegisterCustomDataVariableDefinition`
  (`DataModelHandle.h:136`). It resolves **any** child name at lookup time, so keys need no
  pre-registration and the table stays the single source of truth.
- Bound under the reserved name **`t`** in every model VaCuus creates, plus a standalone
  per-context model named **`vacuus`** carrying only `t`, for documents with no game model
  (without a `data-model` attribute in scope, `{{ }}` does nothing at all).
- Updates ride the existing path: `DataExpression::GetVariableNameList` returns
  `address[0].name` — the top-level name only (`DataExpression.cpp:1145-1154`) — so one
  `DirtyVariable("t")` re-evaluates every `{{ t.* }}` in that model. VaCuus already dirties
  models exactly this way (`VaCuusBoundModel.cpp:345,387`); no new update machinery.
- On snapshot install (UI thread), every VaCuus-created model is dirtied on `t`.

### Dotted keys

`ParseAddress` splits on `.` (`DataModel.cpp:9-42`), so `{{ t.menu.settings.title }}` calls
`Child()` three times. Supported by accumulating the prefix across calls and looking up the
joined key at the leaf: hierarchical keys are the dominant style in real projects, and
banning dots would be worse than the character restriction below.

### Key shape is restricted, and that is why live is opt-in

`IsVariableCharacter` (`DataExpression.cpp:315-330`): first character `a-zA-Z`, then
`a-zA-Z0-9`, `_`, `.`. No hyphens, no spaces, no colons, no leading digit. `{{ t.hud_health }}`
works; `{{ t.hud-health }}` and `{{ t.2p_mode }}` cannot.

A key that does not fit simply stays parse-time and translates the old way. This is the
argument for per-string opt-in rather than a global live mode: as a global mode, that
character table would become a constraint on the entire project's key naming.

### Name collision

`t` is reserved. A bound struct whose top-level field is named `t` gets a named refusal at
bind time, not a silent overwrite — enforcement by shape, per the project convention.

## 2. Change signals

### JS: `vacuus.onLanguageChanged`

Exactly the `vacuus.onUnload` shape: initialised to `JS_NULL`
(`VaCuusJsViewContext.cpp:186`), read at dispatch time under `FVaCuusJsEntryGuard`
(`:271-290`), so `=== null` feature-tests cleanly and reassignment works.

**Ordering is part of the contract:** the callback fires *after* the snapshot is installed
and *after* `DirtyVariable`, so `vacuus.translate` inside it already sees the new table.

Its argument is the **tag** the game passed at push time —
`SetTranslationTable(Table, TEXT("ru"))`. One `FString` on the snapshot turns the callback
from "something changed" into "the language is now ru", which is what a UI needs to swap a
flag icon or a font class. The plugin never interprets the tag; it only carries it.

### Game thread: a dynamic multicast on the subsystem

Broadcast synchronously inside `SetTranslationTable`. This is the single hanging point for
re-pushing models that carry `FText` — instead of every buyer independently discovering
`FInternationalization::OnCultureChanged`. Delegates in this style already exist
(`VaCuusSubsystem.h:26`, `VaCuusView.h:97`).

## 3. Fonts

### The door

`UVaCuusSubsystem::LoadFontFace(VfsPath, bFallbackFace)` → command queue →
`Rml::LoadFontFace(path, fallback_face)` (`Core.h:108`).

### Why a registry, not just a door

Fallback faces are consulted **in registration order**
(`FontFaceHandleDefault.cpp:367-383`), and the font provider is process-global state inside
`Rml::Initialise/Shutdown`. So this needs the same shape as the style and translation
registries: an ordered, deduped list plus `PublishToUIThread` for replay after a UI-thread
restart (`VaCuusTranslation.h:83` is the precedent). Without it a restart silently loses the
Cyrillic face.

### The silent failure, and the fifth patch

When no face — primary or fallback — has the glyph, RmlUi substitutes
`Character::Replacement` and **logs nothing at all** (`FontFaceHandleDefault.cpp:386-393`).
Russian text under the shipped LatoLatin is a screen full of `U+FFFD` with a clean log.

Measured coverage of the shipped face (`fc-query` on
`Content/DevUI/fonts/LatoLatin-Regular.ttf`): `20-7e`, `a0-17f` plus symbols. No Cyrillic,
no CJK.

An invariant with no observable cannot be tested and will rot, so this gets a substitution
**counter** plus one latched Warning naming the first missing character — a per-glyph log
would flood at frame rate. That is a fifth vendored RmlUi patch, on top of the four
`VaCuus-7ym.1` is offering upstream; it is independently useful and goes up in the same
batch.

## 4. Bridge to UE localization

`SetTranslationTableFromStringTable(UStringTable*, Tag)`:
`FStringTable::EnumerateKeysAndSourceStrings` (`StringTableCore.h:157`) yields the keys, and
`FText::FromStringTable(TableId, Key).ToString()` (`Text.h:510`) yields the string already
resolved against the current culture. The engine's `.locres` pipeline then works through
VaCuus with no glue written by the buyer.

Opt-in and off by default: config `[VaCuus] TranslationStringTable=<asset path>` plus an
automatic re-push on `OnCultureChanged`. This is the piece to cut first if it reads as
hidden magic; nothing else depends on it.

## 5. Observables and tests

Per the project standard — break it deliberately, watch the specific test fail, restore.

| Test | Restore-the-bug |
|---|---|
| `{{ t.key }}` changes with no reload after a push | drop `DirtyVariable` → text stays stale |
| dotted `{{ t.menu.settings.title }}` resolves | — |
| a hyphenated key is diagnosed, not silent | — |
| a struct field named `t` gets a named refusal | drop the check → silent overwrite |
| `vacuus.translate` inside the callback sees the new table | fire the callback before install → sees the old one |
| the font registry survives a UI-thread restart | drop `PublishToUIThread` → faces gone |
| the replacement-glyph counter rises on Cyrillic with no face | — |

New counters needed as observables: model dirty count on `t`, callback fire count (the
`GetNumListenerRefs` precedent), font faces replayed, replacement glyphs substituted.

## Implementation order

1. **Foundation** — snapshot tag, `SetTranslationTable(Table, Tag)`, game-thread delegate,
   `UStringTable` bridge.
2. **Live route** — the custom variable definition, injection into every model, the
   standalone `vacuus` model, dirty-on-install, reserved-name refusal.
3. **JS signal** — `vacuus.onLanguageChanged` and its ordering contract.
4. **Fonts** — registry, door, replay, the vendored counter patch.
5. **Docs** — close out VaCuus-cii against the shipped surface.
