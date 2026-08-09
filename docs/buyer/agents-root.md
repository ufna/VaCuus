<!--
  THIS FILE IS MATERIALISED AS THE PACKAGE ROOT `AGENTS.md` by Tools/fab_package.sh.
  It does not ship at this path -- Config/FilterPlugin.ini excludes it -- because the
  repository root already has an AGENTS.md and that one is about DEVELOPING the plugin
  (beads, the two-tree build). A buyer's agent must never be handed those instructions.
  Keep this file short: it is a front door, and everything it points at is the real page.
-->
# VaCuus — instructions for AI coding agents

You are looking at **VaCuus**, a plugin that renders a game's user interface from HTML and
CSS documents, off the game thread. If you have been asked to build or change UI in this
project, read this page first and then
[`docs/buyer/ai-guide.md`](docs/buyer/ai-guide.md), which is the full version.

## The five things that will otherwise cost you the session

1. **The style language is RCSS, not CSS, and unsupported properties are dropped with no
   diagnostic at all.** No warning, no error, a screen that renders and is wrong. Check
   [`docs/buyer/rcss-matrix.md`](docs/buyer/rcss-matrix.md) — generated from the exact
   engine in this package — *before* writing a property. If it is not there, it does not
   exist. Do not write CSS you remember from the web.
2. **Your documents go in `<Project>/Content/DevUI`,** not in this plugin's `Content/DevUI`.
   Paths resolve plugin-first, and a project file **cannot** shadow a same-named plugin
   file — if your screen shows someone else's content, rename it.
3. **Bind data models before calling `LoadDocument`,** or they bind to nothing and you get
   exactly one Error line at load.
4. **Everything crosses into the UI through `UVaCuusView`, from the game thread.** There is
   no supported path around that; code that tries will assert.
5. **You cannot see the screen, so do not report success from a successful compile.** Run
   something that renders and read the log (`LogVaCuus`), or say plainly that you did not.

## Verify like this

```bash
# the plugin's own suite -- ships with the package, no RHI needed
<Engine>/Binaries/<Platform>/UnrealEditor-Cmd <Project>.uproject \
  -ExecCmds="Automation RunTests VaCuus, Quit" -unattended -nullrhi -nosplash
```

`-ExecCmds` splits on **commas**, not semicolons, and swallows every argument to its
right — put it last and end its value with a comma.
[`docs/buyer/setup.md`](docs/buyer/setup.md) §4 has the rest of that pipeline.

## Where the answers are

| Question | Page |
| --- | --- |
| How do I install it, host a document, ship it? | [`docs/buyer/setup.md`](docs/buyer/setup.md) |
| Is this style supported? | [`docs/buyer/rcss-matrix.md`](docs/buyer/rcss-matrix.md) |
| Why is this behaving strangely? | [`docs/buyer/gotchas.md`](docs/buyer/gotchas.md) — 20 numbered findings |
| What does it cost, and what is the budget? | [`docs/buyer/perf-guide.md`](docs/buyer/perf-guide.md) |
| The full agent briefing | [`docs/buyer/ai-guide.md`](docs/buyer/ai-guide.md) |
