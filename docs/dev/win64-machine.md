# The Win64 matrix machine — layout, venue rules, and what is left

Repo-internal, like `host-project.md` beside it: `Config/FilterPlugin.ini` ships `docs/buyer/`
and nothing else under `docs/`, so this page may name machine paths. It started life as
`C:\VaCuusSession\0-READ-ME-FIRST.md` on the machine itself and is kept here because a note
that exists only on one desktop is one disk failure from being rediscovered the hard way.

## Layout

| Path | What it is |
|---|---|
| `C:\VaCuus` | The repo — git, docs, source of truth on that machine. `origin` is `https://github.com/ufna/VaCuus.git`. |
| `C:\VaCuusWin64Test\VcHost` | The host project. Stock `TP_ThirdPerson` template; only the `.uproject` was renamed, so the targets are **`TP_ThirdPerson`** and **`TP_ThirdPersonEditor`**. The three plugin-scoped config lines are mirrored into its `Config`. |
| `C:\VaCuusWin64Test\VcHost\Plugins\VaCuus` | The **build clone** — a real `git clone` of `C:\VaCuus`, the same two-tree layout the Linux box uses, and never a symlink (UBA aborts on those; see `host-project.md`). |
| `C:\VaCuusWin64Test\{logs,rows,runs}` | The console pass's evidence. **Already copied off** to `/w/Unreal/VaCuusProofs/m6-win64-console/` on the Linux box (117 files, 16 screenshots, ~41 MB), deliberately outside the repository. |
| `C:\VaCuusControl\Ctl` | A stock template project with no VaCuus in it — the control leg that settles "is this the plugin or the machine". |

## The venue rule, which is the whole reason the matrix stalled for a day

**An SSH session on Windows has no interactive desktop.** A GUI process launched from one
dies at startup — exit 3, before engine init, zero VaCuus lines in the log. The morning pass
of 2026-08-03 fell back to `-RenderOffscreen`, which boots but cannot produce the rows that
exist to be *seen*.

**The answer, and it is the cheapest one on the list (bead `5fg`, closed):** run from the
**physical console session**. No PsExec, no scheduled task, no RDP, no autologon. The thing
that must be true is that the shell issuing the launch is itself in the console session —
`SESSIONNAME=Console`, `[Environment]::UserInteractive` true. That drove ~15 sessions with
zero venue failures.

Two operational lessons from that pass, both learned by doing it wrong first:

- **Close the window, do not kill the process.** `Process.CloseMainWindow()` gives exit 0 and
  the full teardown tail — UI thread in-band stop, VFS totals, RmlUi shutdown, zero
  unpublished resources — and that tail *is* matrix row 14's evidence. A hard kill destroys it.
- **Staged builds re-launch themselves.** The staged root `.exe` is a launcher stub that
  starts the real binary out of `<staged>\VcHost\Binaries\Win64\`, so killing the PID
  `Start-Process` handed back leaves the game running. Two such orphans locked `dbghelp.dll`
  and killed a later cook with `Error_FailedToDeleteStagingDirectory (102)`. Reap by
  executable **path** after every staged run.

## Machine prerequisites — one fixed, one to know

- **.NET Framework SDK: FIXED 2026-08-04 (bead `akj.10.9`).** It used to be absent, and
  `SwarmInterface.Build.cs:29-34` throws when `NetFxSdkDir` is null, which no editor target
  survives — and since `BuildCookRun` builds the editor target, packaging inherited the same
  wall. The SDK is now installed, **the AutoSDK shim under `C:\VaCuusWin64Test\autosdk` is
  deleted**, and `UE_SDKS_ROOT` is set in no scope. If you find a recipe anywhere that sets
  it, that recipe is stale: install `Microsoft.Net.Component.4.6.2.SDK`, do not rebuild the
  shim.
- **Git credentials were never established on that machine.** Its clone came from a bundle,
  so a non-interactive `git ls-remote` dies on the `wincredman` credential store. From a
  console terminal the credential manager can prompt, and one interactive `git pull` fixes it
  for good. Until then, moving a branch across is `git bundle create` on one side, `scp`, and
  `git fetch <bundle> master` on the other.

## The helpers, now in the repo

They used to live only in `C:\VaCuusSession`. `Tools/` is not part of the plugin package, so
they cost a buyer nothing:

```powershell
powershell -ExecutionPolicy Bypass -File C:\VaCuus\Tools\build_editor_win64.ps1
powershell -ExecutionPolicy Bypass -File C:\VaCuus\Tools\run_tests_win64.ps1
powershell -ExecutionPolicy Bypass -File C:\VaCuus\Tools\buildplugin_strict_win64.ps1
powershell -ExecutionPolicy Bypass -File C:\VaCuus\Tools\api_export_check_win64.ps1 `
    C:\VaCuusWin64Test\VcHost\Plugins\VaCuus
```

Each takes the project/engine/target as parameters and defaults to this machine's layout.
Read the counts from `Saved\Logs\VcHost.log`, never from stdout.

## What is left

`docs/passport/2026-08-vacuus-manual-matrix.md`, Win64 D3D12 column: **13 of 15 rows PASS**.
Two remain, and neither is blocked by anything this page can fix:

- **Row 5, IME composition** (bead `akj.6.19`) — needs a human at the keyboard with a
  Japanese or Chinese IME installed, in a console session. The precondition is established:
  the platform IME bridge reports the `present` branch on this machine, which it never could
  on Linux.
- **Row 13, live reload** (bead `akj.10.10`) — needs an interactive editor PIE session and a
  mid-run file edit. It has never been executed on **any** platform, so it is not a Win64 debt
  specifically; whichever machine has a desktop and an editor can close its own column.

## Dev-loop hazards that bit someone at least once

- **`-ExecCmds` splits on COMMAS, not semicolons**, and the value swallows every argument
  after it. End it with a comma: `-ExecCmds="vacuus.M2Demo,"`.
- **The editor often does not exit** after `Automation RunTests …, Quit` — `Quit` is
  dispatched at frame 0, deferred, and never fires. Kill by **PID** once
  `Sending StopTestSession` appears; never by a name pattern, which has matched the caller's
  own shell before.
- **No editor may be running while you build** — it holds the DLLs. The helper scripts check
  and refuse.
- **`-resx`/`-resy` are ignored offscreen without `-ForceRes`** (you get 888×500).
- **`vacuus.M1HUD.AutoShot N` fires after `max(N, 3)` *recorded* frames**, not published and
  not N. To photograph a transient state, make the state last longer.
- **A dropped SSH link does not kill a running build.** Measured 2026-08-04: the link died
  four minutes into a 45-minute `BuildPlugin`, the machine then left the network for over an
  hour, and the build still finished and wrote its whole log. Launching detached is still the
  better shape, but a lost connection costs the live output, not the work.
