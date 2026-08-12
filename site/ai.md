---
title: Built for AI agents
description: VaCuus screens are .rml/.rcss text on disk, so an AI coding agent can diff, review and write them — and the plugin ships the instrumentation an agent needs to check its own work on a screen it cannot see.
prev: false
next: false
---

# Built for AI agents

If a coding agent is going to write or maintain your game's interface, the format that
interface lives in is most of the decision. This page is the argument that VaCuus is a
reasonable answer to that, and — because it would not be worth reading otherwise — the
part of the argument that is against us.

## A screen is text on disk

A UMG screen is a binary asset. An agent can create one through the editor's automation
surface, but it cannot read the widget graph in a way that means anything, cannot review a
change to it, and cannot show you a diff of what it did. The artifact is opaque to the tool
you hired to work on it.

A VaCuus screen is `myhud.rml` plus `myhud.rcss` — markup and styles, plain text, in your
project's `Content/DevUI`. There is no asset to open and no graph to wire, which has three
consequences that matter more than they sound:

- **The agent's edit is complete when the file is saved.** Not when an editor round-trip
  finishes, not when a cook runs. The editor watches `Content/DevUI` and reloads a changed
  document during PIE; `vacuus.ReloadUI` does the same at runtime. A file save is the loop
  the product is built around.
- **Changes review like web code.** `git diff` on a HUD change shows you the selector that
  moved and the value that changed. You can read a pull request from your agent.
- **The agent is working in its native medium.** Markup and a cascading style language are
  the most heavily represented thing in any model's training data.

That last point is also the source of the problem, which is the next section.

## The honest problem: the wrong screen still renders

Most authoring mistakes here do not produce an error. They produce **a screen that renders**
— just not the one you asked for. A human notices in half a second. An agent cannot see the
screen at all, and a model that reaches for a CSS feature it remembers from the web will
sometimes be reaching for something RCSS does not have.

VaCuus's answer to that is not "trust the model". It is instrumentation, four pieces of it,
all of which an agent can operate on its own.

### 1. Log-first diagnostics with a file and a line

A declaration RmlUi cannot parse produces a Warning naming the **file and the line**
(`StyleSheetParser.cpp:1068`), and those lines reach the Unreal log in *every*
configuration, **including Shipping** — asserts are compiled out of a shipping build, log
lines are not. So `LogVaCuus` is not best-effort instrumentation that thins out as you get
closer to release: a clean log is real evidence, and most "why does my style do nothing"
questions are already answered in it, with the location, before the agent forms a theory.

This does not cover everything, and the shipped docs are explicit about which parts it
misses. A property that parses and this renderer does not draw warns differently or not at
all (`box-shadow` warns once per view and names its substitute; `mask-image` parses, does
not mask, and paints its artwork over your element). Two failures are genuinely silent: a
tween keyword with a capital letter, and `var()` inside a `transition` value. Telling those
three classes apart is [§1 of the AI guide](/docs/ai-guide).

### 2. Ground truth that outranks the model's memory

[`rcss-matrix.md`](/docs/rcss-matrix) is **generated** — parsed mechanically out of
`StyleSheetSpecification::RegisterDefaultProperties` in the exact RmlUi tree vendored into
the package, and pinned to that tree's SHA. It lists every property, its default, whether it
inherits, whether it affects layout, and the value grammar its parser chain actually
accepts.

The rule the guide gives the agent is one line: **if it is not in that file, it does not
exist.** A generated authority is the one thing that reliably beats a plausible memory,
because it can be checked in a second and it cannot be argued with.

### 3. A way to look at the actual pixels

The agent can render your screen headlessly and inspect the frame:

```bash
<Engine>/Binaries/<Platform>/UnrealEditor <Project>.uproject \
  -game -RenderOffscreen -ForceRes -resx=1920 -resy=1080 \
  -ExecCmds="<your console command that shows the UI>,"
```

Two traps in that line cost more agent time than anything else, so both are written down
with the engine source that causes them in [`setup.md` §4](/docs/setup): `-ExecCmds` splits
on **commas**, not semicolons, and its value swallows every argument to its right — so it
goes last and ends with a comma. And `-RenderOffscreen` **ignores `-resx`/`-resy` without
`-ForceRes`**, which quietly makes every pixel claim the agent then reports be about the
wrong frame.

### 4. A suite that clears the plugin before it blames itself

The 227-test automation suite ships **in the package** — test source and fixtures, on
purpose — and needs no RHI:

```bash
<Engine>/Binaries/<Platform>/UnrealEditor-Cmd <Project>.uproject \
  -ExecCmds="Automation RunTests VaCuus, Quit" -unattended -nullrhi -nosplash
```

Green means the plugin is healthy on this machine, and the agent can stop considering that
branch. Debugging is mostly the business of eliminating halves, and an agent that can
eliminate one on its own is a much cheaper agent to supervise.

## What ships in the box, for the agent

| File | What it is |
| --- | --- |
| `AGENTS.md` at the package root | The front door. Five things that would otherwise cost the session, the verify command, and a table of where the answers are. Agents find it without being told. |
| [`docs/buyer/ai-guide.md`](/docs/ai-guide) | The full briefing: the three RCSS failure modes, where files go, how to host a document, how to know it worked, and **Rules for an agent**. |
| [`docs/buyer/rcss-matrix.md`](/docs/rcss-matrix) | The generated surface, above. |
| [`docs/buyer/gotchas.md`](/docs/gotchas) | 22 numbered recorded findings — symptom, cause with the source that proves it, what to do. Every one is a finding from building the plugin's own demos, none is speculative. |
| [`docs/buyer/perf-guide.md`](/docs/perf-guide) | Measured budgets, so an optimisation pass has a target instead of an opinion. |

The "Rules for an agent" section is the part worth quoting, because it is written at the
model rather than at you:

> **Do not invent RCSS.** If you cannot find the property in `rcss-matrix.md`, it does not
> exist, and writing it produces a document that renders and is wrong.

> **Read the log before forming a theory.** Most of what you would otherwise deduce is
> already written down there with a file and a line number.

> **Do not report success from "it compiled" or "the file was written".** Neither is
> evidence about a UI. Run something that renders, or read a log, and say which one you did.

## Onboarding is one line

The guide is in the package, so briefing your agent is a pointer, not a paste. Add this to
your own project's `CLAUDE.md` or `AGENTS.md` and Claude Code, Codex, Cursor or Copilot
will pick up the rest on its own:

```markdown
UI is VaCuus (HTML/CSS off the game thread). Before touching anything under
Content/DevUI, read Plugins/VaCuus/docs/buyer/ai-guide.md and
Plugins/VaCuus/docs/buyer/rcss-matrix.md.
```

## The limits, stated plainly

**RCSS is not CSS.** It is RmlUi's language: a deliberate subset with its own additions.
There is no CSS Grid. `box-shadow` parses and does not render. A `transition` shorthand
containing one unsupported timing function discards the **entire declaration** — there is no
partial application, and `ease-in-out` is not a keyword here (`cubic-in-out` is the
substitution). `@font-face src` resolves root-relative, the opposite of every other path in
the system.

**There is no browser.** No `<a href>` navigation, no `fetch`, no DOM library. Navigation is
your host code calling `LoadDocument`; layout is flexbox and absolute positioning; data
comes from bound `UPROPERTY`s, and models must be bound **before** the document loads or
they bind to nothing.

**There is one thread rule and it does not bend.** Every call into the UI goes through
`UVaCuusView` from the game thread. That is not an API surface we forgot to add; code that
routes around it will assert.

Every one of those limits is also the reason this works for agents at all: the surface is
small, closed and generated, so it fits in a context window as ground truth rather than being
recalled from the whole web platform. The landing page puts that beside what a real browser
costs in a game — payload, RAM, processes, input latency — in
[Why not a browser?](/#why-not-a-browser).

An agent arriving with web habits will be wrong in each of these ways, and it will be wrong
*confidently*, because each of them is a thing that works everywhere else it has read about.
That is precisely why the shipped documentation exists in the shape it does: the failures are
enumerated, numbered, and attributed to the source line that causes them, so the agent can
look the answer up instead of deriving it from a screen it cannot see.

VaCuus is **pre-release** and its API is not yet stable — expect renames between versions.
If your agent is going to write your UI, read [the full agent guide](/docs/ai-guide) and
[the gotchas](/docs/gotchas) before you decide; both are the same text that ships in the
package, published here so you can judge them before you buy.
