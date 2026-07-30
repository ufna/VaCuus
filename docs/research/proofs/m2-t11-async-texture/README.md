# M2 Task 11 — async texture decode, proven end to end

Task 11 made image loading asynchronous: `LoadTexture` probes dimensions synchronously (RmlUi
needs them immediately for layout), inserts a **1×1 transparent (0,0,0,0)** placeholder, and
launches a `UE::Tasks` decode. The finished payload travels back through the normal command-buffer
channel and replaces the placeholder — `TMap::Add` on an existing key *is* the swap.

The unit tests prove the contract. They cannot prove the picture. This directory is the picture.

## The problem with photographing a transient state

`vacuus.M1HUD.AutoShot 1` does **not** shoot UI frame 1 — it fires after **3** published UI frames,
reproducibly. `SVaCuusWidget::TickAutoShot` polls the published-frame count from the game thread's
Slate tick, and the UI thread has already published three frames by the time that first tick runs.
The counter is a floor, not a target, so "shoot frame 1 to catch the placeholder" cannot work.

The technique that does work: **make the transient state last longer.** A synthetic 6000×6000 PNG
(36M px, 144 MB decoded) that is only 150 KB on disk keeps the file read and the probe memcpy
trivial while the decode takes hundreds of milliseconds. Then two runs of the *same* asset with the
*same* command, differing only in which frame is captured:

| run | shot at | avatar box | crop mean / stddev |
|---|---|---|---|
| C | UI frame 3 | **empty** — plate background only, badge still drawn | 0.2654 / 0.2126 |
| D | UI frame 300 | real payload (magenta→purple gradient) | 0.5314 / 0.1925 |

`swap_comparison.png` is the one-image proof: placeholder | real payload | the genuine fast avatar.

Note the box renders **transparent, not missing** — that is the 1×1 `(0,0,0,0)` doing its job. A
missing map entry would instead have tripped the replayer's unknown-handle `ensureMsgf`.

## What this closes that no automated test could

The replayer's `ensureMsgf(false, "Draw references unknown texture handle")` is unreachable from
automation: no test instantiates `FVaCuusReplayRenderer`, and none could under `-nullrhi`, because
there is no RHI. The unit test proves only the *precondition* (the handle is present in
`NewTextures`, so the replayer's `Find` succeeds).

Run C closes that gap directly: a draw referenced a handle whose payload had not yet arrived, and
across all three runs there are **zero** `Ensure condition failed`, zero unknown-handle messages,
and zero `LogImageWrapper` errors. The only `LogVaCuus: Warning` present is the pre-existing Linux
IME line about this platform exposing no text-input-method system.

## The measurement, which is the most interesting result

Run C, `PerfLog 1`, the 5 s window containing the document load:

```
PerfLog window 5.0s frames=937 fps=187.3 draws/frame=100.0
PerfLog [win] Update    (UI) avg=0.014 p50=0.011 p99=0.053 max=0.095  ms (937)
PerfLog [win] Record    (UI) avg=0.042 p50=0.039 p99=0.105 max=0.438  ms (937)
PerfLog [win] Replay    (RT) avg=0.068 p50=0.028 p99=0.066 max=36.567 ms (933)
PerfLog [win] Composite (RT) avg=0.003 p50=0.003 p99=0.006 max=0.068  ms (934)
```

Decoding 36 million pixels cost the UI thread **nothing measurable** — `Update` max 0.095 ms,
indistinguishable from steady state (0.383 ms max over 6456 frames). That is the bead this task
existed to close, demonstrated on the hardest input we could construct.

The hitch moved, as designed and as documented at the drain site: `Replay max = 36.567 ms` is the
render thread's `UpdateTexture2D` memcpy of 144 MB, and it never recurs in any later window
(subsequent `Replay` max 0.149 ms). **Decode async, upload not.**

That gives the async-upload follow-up (`VaCuus-akj.6.25`) real sizing instead of a guess: roughly
**1 ms of render thread per 4 MB of texture**, so a realistic 2048² UI atlas costs about 4 ms once,
at load. Not worth the complexity yet; the number to beat is 4 ms per 16 MB.

## What is NOT proven here

The abandon path — recorder destroyed with a decode in flight — could not be reached from the
console. `-ExecCmds="vacuus.M1HUD, vacuus.M1HUD"` resolves both toggles in game frame 0, so the UI
thread drains `AddView` and `RemoveView` in one tick, no frame is recorded, and `LoadTexture` never
runs. Reaching it needs a *delayed* toggle, which is a console command that does not exist. So that
requirement rests on the structural argument (a refcounted sink the task holds by value, plus an
atomic abandon flag) rather than on live evidence. Recorded as a limit, not claimed as proof.

## Fixture note

Runs C/D/E temporarily replaced the tracked LFS file `Content/DevUI/img/avatar.png` with the
synthetic slow-decode PNG, then restored it. Verified afterwards: `md5sum` back to
`aa7d0ff166995b631d26e9b81aaeec3f`, `git diff a177662 --stat` empty. No production code was
modified and no build was performed for this proof.
