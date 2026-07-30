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

## Regression coverage (what actually guards this going forward)

The screenshots are a one-time acceptance artifact, not a test. The automated coverage is:

- `VaCuus.LiveReload.Filter` — the debounce and the filename filters, including a
  `#if PLATFORM_LINUX` case asserting the real watcher's documented pitfall (a registration on a
  non-existent directory returns a valid handle and then silently delivers nothing).
- `VaCuus.LiveReload.Dispatch` — a real game instance; the view's reload serial advances by
  exactly one on the file-backed document view and by zero on every other view.
- `Proof.LiveReload.PIE` — a **harness, not a test**: it queues latent commands, asserts nothing,
  and waits on an external edit. Labelled as such in its own header comment. It is also why
  `VaCuus-akj.6.20` exists (it stalls a wildcard automation run for ~28 s).
