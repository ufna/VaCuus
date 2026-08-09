# Localization — shipping a game in more than one language

VaCuus does not read your `.locres` and does not know what a culture is. It holds **one
table of strings** that your game pushes, and it answers from that table in two places.
Everything else on this page follows from that one sentence.

Two things will cost you a day each if you skip them. **Text already in the markup does
not re-translate** unless you opted that string into the live route — §3 is the whole
story, with a picture. And **the font in the box covers Latin only**, so Russian or
Chinese renders as boxes until you bring your own face — §6.

To see all of it working before you read another word:

```
vacuus.LocDemo
vacuus.LocDemo.Lang fr
vacuus.LocDemo.Lang ru
```

## 1. The one call

```cpp
UVaCuusSubsystem* Subsystem = GetGameInstance()->GetSubsystem<UVaCuusSubsystem>();

TMap<FString, FString> Table;
Table.Add(TEXT("hud_health"), TEXT("Santé"));
Table.Add(TEXT("menu.settings.title"), TEXT("Paramètres"));

Subsystem->SetTranslationTable(Table, TEXT("fr"));   // also a Blueprint node
```

- **Whole-table replacement, never a merge.** The table you push is the table there is; a
  key you dropped stops answering. There is no "add one entry".
- **Process-wide.** One table serves every view of every game instance, including multiple
  PIE clients. That is not a limitation to work around — RmlUi has one system interface per
  process and the table lives beside it.
- **The tag is yours.** `"fr"`, a culture name, a build id — the plugin never looks at it.
  It is handed back to you by both change signals in §4 so a handler knows *which* language
  it is reacting to.
- **A key with no entry renders as itself**, everywhere. That is deliberate: a missing
  translation shows up on screen as `hud_health` instead of blanking the label.

## 2. Two readers, and they do not have the same timing

This is the table to remember. Almost every localization surprise is a consequence of it.

| Where the text is | Translated | Re-translates on a new table? |
| --- | --- | --- |
| `<div>hud_health</div>` — plain markup | once, when the document is parsed | **No.** Reload the document. |
| `{{ t.hud_health }}` — inside a data model | every time the model is dirtied | **Yes**, on the next UI frame |
| `vacuus.translate('hud_health')` in JS | at the call | **Yes**, the call always reads the newest table |

Plain markup is the fast path and it stays the default. You opt individual strings into
the live route; you do not switch the plugin into a mode.

## 3. Live text: `{{ t.key }}`

Write the key as a data expression against the reserved variable `t`:

```html
<body data-model="vacuus">
    <div>{{ t.hud_health }}</div>
    <div>{{ t.menu.settings.title }}</div>
</body>
```

`data-model="vacuus"` is a model the plugin creates for every view, holding `t` and nothing
else. Use it when your document binds no USTRUCT of its own — a settings screen, which is
exactly where a language gets changed, very often binds nothing.

If your document already binds a model, **use your own model's name** and `t` is there too:

```html
<body data-model="hud">
    <div>{{ t.hud_health }}: {{ Health }}</div>
</body>
```

`t` is reserved. A struct field named exactly `t` is refused at bind time with an Error
naming the model and the field; every other field of that struct binds normally. (A field
named `T` is a different variable and does not collide — data addresses are matched
byte-for-byte.)

### What a switch looks like

Same document, two pushes apart, nothing reloaded:

| `vacuus.LocDemo.Lang en` | `vacuus.LocDemo.Lang fr` |
| --- | --- |
| LIVE · translated live<br>**Health**<br>**Settings** | LIVE · traduit en direct<br>**Sante**<br>**Parametres** |
| PARSE-TIME · the same key<br>**Health** | PARSE-TIME · the same key<br>**Health** |

The second panel uses the *same key* as the first and did not move. That is not a bug to
report — it is §2's first row, and the demo exists to make it visible.

### Keys usable in the live route are restricted

An RmlUi data expression accepts a leading `a-z`/`A-Z`, then letters, digits, `_` and `.`:

- `{{ t.hud_health }}` — fine
- `{{ t.menu.settings.title }}` — fine, dots nest and the whole path is the key
- `{{ t.hud-health }}` — **impossible**, hyphens end the token
- `{{ t.2p_mode }}` — **impossible**, no leading digit

A key that does not fit simply stays parse-time and translates the old way. This
restriction is exactly why live is opt-in per string: as a global mode it would become a
naming rule for every key in your project.

The live route has no `{n}` parameter substitution. Put values beside it from your model
(`{{ t.kills_label }} {{ hud.kills }}`), or build the string in JS with
`vacuus.translate(key, params)`.

## 4. Knowing that the language changed

**In JS**, when your UI builds strings itself:

```js
vacuus.onLanguageChanged = (tag) => {
    document.getElementById('kills').innerRML =
        vacuus.translate('kills_label', { n: kills });
};
```

The new table is already installed when this runs, so `translate` inside the handler
answers in the new language. Assign `null` to stop receiving it.

**On the game thread**, and this one you almost certainly need — see §5:

```cpp
Subsystem->OnTranslationTableChanged.AddDynamic(this, &AMyHud::HandleLanguageChanged);
```

## 5. `FText` in a bound model must be re-pushed

If you bind a USTRUCT containing `FText`, this is a contract and not advice.

An `FText` field is resolved to a culture-invariant string **once**, on the game thread, at
the moment you call `UpdateModel`. That is what keeps the UI thread away from
`FTextLocalizationManager` and is what makes the whole design thread-safe. The consequence:
the projected text can never re-resolve itself, so **a culture change is invisible to a
bound model until the next `UpdateModel`** — silently, because nothing on that path is
wrong.

Either call `UpdateModel` every frame (everything else here assumes you do), or re-push
from the signal in §4.

You will not have to guess whether you got this right. A model carrying `FText` that sees
no update in the 60 UI frames after a table change gets one Warning naming the model and
its type; a model you push every frame never triggers it.

## 6. Fonts — the one that surprises everyone

**The face in the box is `fonts/LatoLatin-Regular.ttf`, and it is Latin-only.** Measured
coverage: `U+0020–007E`, `U+00A0–017F`, plus assorted symbols. No Cyrillic. No CJK. No
Greek beyond a few maths glyphs.

Push a Russian table without doing anything about fonts and you get this:

> LIVE · ▯▯▯▯▯ ▯▯▯▯▯▯▯
> **▯▯▯▯▯▯▯▯**

The plugin patches RmlUi so that the log at least tells you, once — upstream substitutes
the replacement character and says nothing at all:

```
LogVaCuus: Warning: [Rml] No glyph for U+0436 in the styled font face or any fallback
face; it renders as the replacement character. Load a font covering this script, and add
it as a fallback face for mixed-script text.
```

Two ways to bring your own face.

**In RCSS**, the authoring route:

```css
@font-face {
    src: fonts/NotoSans-Regular.ttf;
    font-family: NotoSans;
}
body { font-family: NotoSans; }
```

`src` is **root-relative**, not relative to the document — unlike `<link>` and
`<script src>`. See `gotchas.md` #5.

**At runtime**, when you only know which face you need once the language is chosen:

```cpp
Subsystem->LoadFontFace(TEXT("fonts/NotoSansSC-Regular.otf"), /*bFallbackFace=*/false);
Subsystem->LoadFontFace(TEXT("fonts/NotoSansSC-Regular.otf"), /*bFallbackFace=*/true);
```

A **fallback face** is consulted, in registration order, for any character the styled
family has no glyph for. That is what makes mixed-script text work without switching
`font-family` per run of characters — one Latin UI face plus a CJK fallback covers a
Chinese build with no markup changes.

Registering the same face twice is a no-op, and registered faces are reloaded if the UI
thread restarts, so calling this from your language-change handler is safe.

## 7. Using UE's own localization

The plugin does not read `.locres` by itself. One call bridges it:

```cpp
Subsystem->SetTranslationTableFromStringTable(MyStringTable, TEXT("fr"));
```

Every entry's value is resolved against the **current culture**, so your project's `.locres`
decides the answer. Call it again after a culture change — values are resolved at call
time and nothing re-resolves itself afterwards, for the same reason as §5.

Pass a saved String Table **asset**. A `UStringTable` built at runtime in the transient
package never registers itself, and the call refuses it with a named Error rather than
publishing a table in which every value is `<MISSING STRING TABLE ENTRY>`.

## 8. The whole switch, in one place

```cpp
void AMyGameMode::SetLanguage(const FString& CultureName)
{
    FInternationalization::Get().SetCurrentCulture(CultureName);

    // 1. The table. Live text and every later vacuus.translate() are now correct.
    Subsystem->SetTranslationTableFromStringTable(UITable, CultureName);

    // 2. A face that covers it, if the culture needs one. Idempotent.
    if (CultureName.StartsWith(TEXT("zh")))
    {
        Subsystem->LoadFontFace(TEXT("fonts/NotoSansSC-Regular.otf"), /*bFallbackFace=*/true);
    }

    // 3. Parse-time text in the markup — ONLY if you have any. Costs the document.
    UVaCuusSubsystem::ClearAssetCachesAndReloadAllViews(TEXT("language change"));
}
```

Step 3 is the one to design away. A reload re-mounts JS from the module's top level: fresh
context, fresh state. Open tab, scroll position, timers — gone, on the very screen the
player is looking at while changing the language. A settings screen whose translatable text
all goes through `{{ t.key }}` or `vacuus.translate` never needs it.

Step 5's `UpdateModel` re-push belongs here too if your models carry `FText` and you do not
already push them every frame.

## 9. Checklist before you ship a second language

- [ ] Every string a player sees comes from the table, not from the markup — or you accept
      a reload on switch and have tested what it resets.
- [ ] Strings that must change without a reload use `{{ t.key }}` or `vacuus.translate`,
      and their keys fit §3's character rule.
- [ ] A face covering every shipped script is loaded, as a fallback face if the UI mixes
      scripts.
- [ ] The log is clean of `No glyph for U+...` after a pass through every language.
- [ ] Models carrying `FText` are re-pushed on the change signal, or pushed every frame.
- [ ] Longest-language pass: German and Russian strings are frequently 30–40% wider than
      English, and RmlUi will not shrink your buttons for you.
