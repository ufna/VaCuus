# Working on VaCuus UI with an AI coding agent

**Audience: the agent, and whoever is briefing it.** This page is about *using* the plugin
to build a game's interface — not about developing the plugin. It exists because the
failure modes here are unusually hostile to a model working from web knowledge: most
mistakes produce **no error, no warning and a screen that renders**, just not the screen
you asked for. A human notices. An agent, which cannot see the screen, does not.

Point your assistant at this file. If you use Claude Code, Codex, Cursor or Copilot, the
cheapest way is one line in your project's own `CLAUDE.md` / `AGENTS.md`:

```markdown
UI is VaCuus (HTML/CSS off the game thread). Before touching anything under
Content/DevUI, read Plugins/VaCuus/docs/buyer/ai-guide.md and
Plugins/VaCuus/docs/buyer/rcss-matrix.md.
```

---

## 1. The five facts that change how you write

1. **RCSS is not CSS, and it fails silently.** The style language is RmlUi's, a deliberate
   subset with its own additions. An unsupported property is **dropped without a
   diagnostic** — no console line, no red text, nothing. Code that "looks right" and does
   nothing is the normal shape of a mistake here.
2. **`rcss-matrix.md` is generated from the exact engine in this package.** It is the
   authority, not your training data and not this page. Check a property there *before*
   you write it. If it is not in that file, it does not exist.
3. **Documents are plain text on disk.** `.rml` is the markup, `.rcss` the styles. There is
   no editor asset to open, no graph to wire. This is the property that makes the whole
   thing agent-friendly — and it means your edit is complete when the file is saved.
4. **The UI runs on its own thread.** You never touch it directly. Everything goes through
   `UVaCuusView` from the game thread; the plugin marshals it. If you find yourself wanting
   to call into the UI from a worker thread, stop — that is not an API you are missing, it
   is one that deliberately does not exist.
5. **There is no browser.** No `<a href>` navigation, no CSS Grid, no `fetch`, no DOM
   library you remember. Navigation is the host calling `LoadDocument`; layout is flexbox
   and absolute positioning; data comes from bound `UPROPERTY`s.

## 2. Read these first, in this order

| When | Read |
| --- | --- |
| Before the first document | [`setup.md`](setup.md) §2 — where files live, and the two host classes |
| Before writing any style | [`rcss-matrix.md`](rcss-matrix.md) — the supported surface, generated |
| Before the first authoring session | [`gotchas.md`](gotchas.md) — 20 numbered recorded findings |
| Before optimising anything | [`perf-guide.md`](perf-guide.md) — measured budgets, not guesses |

`gotchas.md` is the highest-value page for an agent and it is worth reading in full once.
The entries most likely to bite a model working from web habits:

- **#1** — your first document collapses into one line. There is no user-agent stylesheet;
  link `vacuus-base.rcss` first or nothing has `display: block`.
- **#2 / #2b** — `box-shadow` and `mask-image` parse and then do not do what you expect.
- **#3 / #3b** — a `transition` shorthand with an unsupported timing function drops the
  **entire declaration**; there is no partial application.
- **#5** — `@font-face src` is **root-relative**, the opposite of every other path.
- **#9** — bind data models **before** `LoadDocument`, or the model binds to nothing.
- **#13** — there is no CSS Grid.

## 3. Where files go

Relative paths resolve against two ordered roots, **plugin first**:

1. `<Plugin>/Content/DevUI` — ships the demos; do not put your work here.
2. `<Project>/Content/DevUI` — **yours**.

It is an extension point, not an override point: a project file **cannot shadow** a
same-named plugin file. If your document mysteriously renders someone else's content,
you picked a name the plugin already uses.

So a screen is `<Project>/Content/DevUI/MyHud/myhud.rml` plus `myhud.rcss`, and the host
refers to it as `MyHud/myhud.rml`.

## 4. Hosting a document

Two classes, both usable from Blueprint and from C++:

- **`UVaCuusWidget`** ("VaCuus View" in the UMG palette) — screen space.
- **`UVaCuusWorldComponent`** ("VaCuus World Panel") — a pixel panel on a quad in the world.

Both take a `Document` path and `bAutoLoadDocument`, and both hand back a `UVaCuusView*`
from `GetView()`. That view is the entire runtime surface: `LoadDocument`, `Close`, the
data-model bindings, input and status.

If you add C++ that touches either, your module's `.Build.cs` needs **`VaCuus`,
`VaCuusRender` and `UMG`**. Omitting `UMG` compiles fine and fails at link with
`undefined symbol: UWidget::TakeWidget()` — `setup.md` §2 explains why UBT does not supply
it for you. The full runnable sequence, including teardown order, is in that section; the
plugin also ships it as a live reference (`vacuus.UMGDemo`).

## 5. How to know it worked — you cannot see the screen, so use these

**Every RmlUi diagnostic reaches the Unreal log in every configuration**, including
Shipping. Silence in the log is real evidence, not missing instrumentation. Filter on
`LogVaCuus`.

Run the shipped suite — it is the fastest way to know the plugin itself is healthy on this
machine before you go blaming your document:

```bash
<Engine>/Binaries/<Platform>/UnrealEditor-Cmd <Project>.uproject \
  -ExecCmds="Automation RunTests VaCuus, Quit" -unattended -nullrhi -nosplash
```

Render your own screen headlessly and look at the pixels:

```bash
<Engine>/Binaries/<Platform>/UnrealEditor <Project>.uproject \
  -game -RenderOffscreen -ForceRes -resx=1920 -resy=1080 \
  -ExecCmds="<your console command that shows the UI>,"
```

`setup.md` §4 has the four traps in that pipeline, each with the engine line that causes
it. The two that waste the most agent time: **`-ExecCmds` splits on commas, not
semicolons**, and its value swallows every argument to its right — so it goes last and its
value ends with a comma. And **`-RenderOffscreen` without `-ForceRes` ignores your
`-resx`/`-resy`**, so every pixel assertion you make is about the wrong frame.

Iterate without restarting: the editor watches `Content/DevUI` and reloads a changed
document during PIE; `vacuus.ReloadUI` does the same at runtime. This is the loop the
product is built around — a file save, not a compile.

## 6. Rules for an agent, specifically

**Do not invent RCSS.** If you cannot find the property in `rcss-matrix.md`, it does not
exist, and writing it produces a document that renders and is wrong. Reaching for a CSS
feature you remember is the single most likely way to waste a session here.

**Do not report success from "it compiled" or "the file was written".** Neither is
evidence about a UI. Run something that renders, or read a log, and say which one you did.

**When the screen is wrong and the log is clean, suspect the sheet, not the plugin.** The
renderer places boxes exactly where the styles ask. Two boxes drawn on top of each other is
almost always two rules that both claim the same anchor.

**Prefer measuring to reasoning about layout.** Guessing coordinates from a stylesheet is
how overlaps ship. The demos expose measured geometry at runtime; use it.

**Do not weaken the thread rule.** Every call into the UI is from the game thread through
`UVaCuusView`. There is no supported path around it, and code that tries will assert.

**Say when you have not verified something.** A claim about how a screen looks, made
without rendering it, is a guess — mark it as one.

## 7. If you are stuck

- Style has no effect → it is almost certainly not supported. Check `rcss-matrix.md`.
- Document renders as one line → `gotchas.md` #1.
- Text is invisible, log repeats "No font face defined" → `gotchas.md` #5, and remember
  `@font-face src` is root-relative.
- Data binding shows nothing, one Error at load → `gotchas.md` #9, bind before load.
- Works uncooked, breaks packaged → `gotchas.md` #16 and #19.
- Editor dies at startup with `exit 127` and no callstack → `gotchas.md` #20; it is a stale
  module binary in a source-built engine, not your UI.
