# M2 Task 10 — editor live reload, measured verification

Step 10.6 of the M2 plan required: PIE running with the HUD, edit `m1_hud.rcss` (change a
colour), the running UI updates within ~200 ms without leaving PIE, screenshot before/after.

This directory is that evidence. It exists because the original evidence lived only in
`/w/Unreal/VcHost/Saved/` — untracked, and in fact overwritten by the next task's automation
runs within the hour. The commit that implemented the step (`9e9ca23`) describes the harness but
records no result, so without this file the step's verification would have been unreproducible.

## Result

**191 ms** from the first inotify event to the reloaded document, in a live PIE session that was
never exited.

Log excerpt from the run (`VcHost.log`, since rotated):

```
:2599 [07:55:15:770] VACUUS_PROOF_EDIT_NOW
:2601 [07:55:16:755] Live reload saw 4 file change event(s), tracked 1; 1 distinct path(s) pending
:2604 [07:55:16:945] Live reload flushed 1 changed path(s) after 190 ms and reloaded 1 view(s):
                     .../Plugins/VaCuus/Content/DevUI/m1_hud.rcss
:2612 [07:55:16:946] View 1 loaded the VFS ('m1_hud.rml') document (1297x892)
:2617 [07:55:31:813] BeginTearingDown for /Temp/UEDPIE_0_Untitled_1
```

### The 9 ms of "headroom", decided (bead `VaCuus-akj.6.24`)

**Nothing changed, and that is the decision.** 191 vs "~200" is not a margin about to be lost;
it is a number chasing itself:

- The reported figure is `Now - FirstChangeSeconds`, and the earliest a batch may flush is
  `LastChange + QuietSeconds`. This save's **four** events spanned ~40 ms, so **190 ms is the
  floor** for it — `0.150 + 0.040` — however fast the machine is. Trimming `QuietSeconds` buys
  1:1 against the 150 and nothing against the 40.
- The remainder is sampling noise **wider than the headroom**: the armed ticker polls every
  50 ms and `FTSTicker` only fires from the engine loop, so the same code legitimately reports
  across a ~50 ms band plus a frame.
- A shorter window does not fail "slightly early"; it flushes **between the writes of one save**
  and reloads a half-written file.
- And "~200 ms" was never a budget — it is the plan step's own wording, tilde included. No spec
  row, passport row or test states a live-reload latency.

`VaCuus.LiveReload.Debounce` case **(e)** now pins that arithmetic to the constants, so the
190 stops being an observation and becomes a derivation: change `QuietSeconds` and it fails,
pointing back here.

Three things this proves beyond the latency:

- **The burst is real and the debounce works.** One editor save produced **4** file-change
  events, collapsed to **1** distinct path. The engine provides no debounce of its own.
- **PIE was not left.** Teardown happens at `:2617`, ~15 s after the reload, and only after the
  harness signalled completion. The reload happened inside a live session.
- **The plugin root is the one being watched and read.** The flushed path and the resolved
  document both sit under `Plugins/VaCuus/Content/DevUI`, matching the `c536261` decision that
  the plugin's content dir is the first VFS root.

## Screenshots

`m2t10_before.png` → `m2t10_after.png`: the `INTERACT` button goes dark-teal → bright green.
Everything else — scoreboard, compass, hotbar, editor chrome, PIE outliner — is pixel-identical,
so the delta is the edited rule and nothing else.

An earlier, tighter pixel measurement of the same mechanism during implementation: 9010 px of the
old colour became 9010 px of the new one, with the changed region's bounding box exactly 200×48 —
the button's authored size.

## The bug the reviews found afterwards, and its restore-the-bug proof

The verification above was real, but it only exercised the case where a view was live. The
code-quality review found a **critical** hole in the case where none is: the RmlUi cache clear rode
as a payload (`bClearAssetCaches`) on the *per-view load command*, handled after the host-lookup
gate — so a flush that reached zero live views enqueued zero commands and cleared nothing, while
RmlUi's caches are process-global statics keyed on file name that outlive the PIE session (the
subsystem's `Deinitialize` deliberately does not stop the UI thread).

Failure sequence: play, stop PIE, edit an `.rcss`, press Play. RmlUi re-parses the RML **from disk**
but serves the **cached** RCSS from the previous session. Old colour, no error, no warning. Nastily
asymmetric — RML edits survived it, RCSS edits did not — and "edit the CSS, then hit Play" is a
mainstream workflow, not a corner. `vacuus.ReloadUI` did not help either, which made its own help
text ("dropping RmlUi's stylesheet/template caches first") false in exactly the case a user would
reach for it.

Fixed by hoisting the clear into its own command kind handled **before** the host lookup, enqueued
once per flush. That was a net deletion: the payload flag came out of the command struct and the
enqueue signature.

The proof standard on this milestone is to restore the bug and watch the new test fail. Both
properties failed, which is what makes the test load-bearing rather than decorative:

```
Test Completed. Result={Fail} Path={VaCuus.LiveReload.AssetCaches}
  (1) ...and the RmlUi asset caches are dropped anyway: The two values are not equal.
  (2) ...at the cost of exactly one cache clear, not one per view: The two values are not equal.
```

Then with the fix restored: `Result={Success}`.

Property (2) is the second bug the same change fixed: the old code cleared once **per command**, so
three reloaded views cleared three times, and clears 2 and 3 discarded the stylesheet view 1 had
just re-parsed. The comment claiming "one clear serves every view reloaded in this drain" only
became true after the hoist.

Note what made this testable at all: RmlUi exposes no way to observe its caches, so the fix added an
observable clear count. Without one the invariant stays unassertable — which is how the bug survived
the original implementation and its first review.

## Regression coverage (what actually guards this going forward)

The screenshots are a one-time acceptance artifact, not a test. The automated coverage is:

- `VaCuus.LiveReload.Filter` — the filename filters, including a `#if PLATFORM_LINUX` case
  asserting the real watcher's documented pitfall (a registration on a non-existent directory
  returns a valid handle and then silently delivers nothing).
- `VaCuus.LiveReload.Debounce` — the debounce *arithmetic*, via an injected clock: not-yet-quiet
  keeps ticking without flushing, quiet flushes exactly once, the hard cap fires while changes are
  still streaming, and a change after a flush is judged against a fresh batch start. Added by the
  review fixes; the original had no coverage of the only function in the feature containing
  arithmetic.
- `VaCuus.LiveReload.WatcherEvent` — writes a real file under the watched root and pumps the
  watcher synchronously with `Tick(-1.0f)`, so the inotify link itself is covered. The original
  implementation asserted in four places that this was impossible; the project's own API notes
  documented the technique, with engine precedent.
- `VaCuus.LiveReload.AssetCaches` — the C1 regression above.
- `VaCuus.LiveReload.Rearm` — a view that fell back to the inline document because its file was
  missing reloads once the file appears. That path was previously dead: the fallback cleared the
  remembered path, so live reload did nothing in the one configuration where you most need it
  (iterating on a broken document).
- `VaCuus.LiveReload.Dispatch` — a real game instance; the view's reload serial advances by
  exactly one on the file-backed document view and by zero on every other view.
- `Proof.LiveReload.PIE` — a **harness, not a test**: it drives a PIE session and waits on an
  external edit. Labelled as such in its own header comment. Originally it was also a trap — it was
  discovered by "Run All", started PIE, blocked ~28 s waiting for an edit that never came, and then
  passed while asserting nothing. Now gated on an explicit `-vacuusproof` opt-in: without the switch
  it logs how to run it and returns in milliseconds without touching PIE; with it, it asserts both
  screenshots exist. (`EAutomationTestFlags::Disabled` would have been the obvious lever and is the
  wrong one — it removes the test from `GetValidTestNames`, so the explicit invocation documented in
  the harness's own docstring would stop resolving.)
