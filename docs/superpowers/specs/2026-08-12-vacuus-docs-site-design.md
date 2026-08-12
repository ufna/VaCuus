# VaCuus docs site — vacuus.ufna.dev

**Date:** 2026-08-12 · **Status:** approved (owner picked stack + host in session)

## Purpose

A public site potential **Fab buyers** open before purchase. Two jobs: (1) republish the
`docs/buyer/` pages — the product documentation — without duplicating them, and (2) sell the
plugin's differentiators, with **"built for AI agents" as the headline accent**. The Fab
listing itself (store copy, thumbnail, category) stays bead VaCuus-jne; this site is what
`DocsURL` points at.

## Decisions made

- **Stack: VitePress** (owner's pick). Markdown-first, one config, sidebar/search/dark mode
  built in, custom landing layout. npm workflow already exists in `Web/`.
- **Host: `vacuus.ufna.dev`** (owner's pick). GitHub Pages + CNAME; DNS record is the
  owner's step. `base: '/'`.
- **Location: `site/` at repo root.** Repo-only, like `docs/dev/` — must never ship in the
  Fab package (verify against `Tools/fab_package.sh` clone/filter logic; add an exclusion
  if anything would pick it up).
- **Zero duplication:** a `sync-docs` prebuild script copies the six public buyer pages
  into the VitePress source tree (copies are gitignored). `docs/buyer/*.md` stays the
  single source of truth; the site rebuilds from it.

## Pages

| Route | Content |
| --- | --- |
| `/` | Landing. Hero: *your game's UI in HTML/CSS, rendered off the game thread*. Honest status banner reusing README wording verbatim in substance: pre-release/beta, UE 5.6 + 5.8 built and tested from one tree, verified on Linux, 5.7 untested, API not yet stable. Pillars: off-thread performance (measured numbers from perf-guide), plain-text workflow + live reload, **built for AI**, UPROPERTY data binding + optional JS/TS. Numbers strip: 227 automation tests, 99 RCSS properties (generated matrix), 0.50 ms reference budget, ~13 000:1 idle publish gate. Real screenshots. CTA: GitHub (issues) + "Get started"; Fab button appears only when the listing URL exists. |
| `/ai` | **Built for AI agents** — the accent page. Why plain-text UI suits agents (no binary assets, no graph; an edit is a file save). What ships in the box *for the agent*: `AGENTS.md` at package root, `ai-guide.md`, `rcss-matrix.md` generated from the exact vendored engine. The verification loop for an agent that cannot see the screen: log-first diagnostics (file:line warnings reach the log in every config incl. Shipping), headless 1920×1080 render recipe, the shipped 227-test suite. The one-line `CLAUDE.md`/`AGENTS.md` snippet as a copy-paste block. Tone: factual, engineering-grade — the site's docs voice, not ad copy. |
| `/docs/*` | The six buyer pages republished as-is: `setup`, `gotchas`, `rcss-matrix`, `perf-guide`, `ai-guide`, `localization`. No rewriting — relative `.md` links between them resolve to routes. `owner-handoff.md` (internal hardware handoff) and `agents-root.md` (materialised into the package as AGENTS.md; carries an HTML comment header) are **not** published. |

Nav: Get Started (`/docs/setup`) · Docs · Built for AI (`/ai`) · GitHub. No FAQ page:
license/price are undecided (owner's open call) and the site must not invent them.

## Honesty constraints

The site claims nothing the repo cannot back: no "production-ready", no platform claims
beyond the descriptor + README status, no invented pricing/license/roadmap. Every number on
the landing traces to a repo source (perf passport, rcss-matrix header, README).

## Screenshots

Honest source per bead VaCuus-jne: headless renders of `vacuus.RefHud` (1,732-node
reference HUD), `vacuus.M5Deco`, `vacuus.M5Glass` at 1920×1080 via the VcHost recipe
(CLAUDE.md hazards apply: comma-separated `-ExecCmds`, trailing comma, `-ForceRes`,
AutoShot ≥3-frame floor, kill by PID). Fallback if the run fails:
`docs/research/hud-demo/hud_shot_*.png`.

## Theme

Custom brand CSS over the default VitePress theme + custom home layout. Dark-first,
game-UI aesthetic consistent with the RefHud shots; light mode still legible. Distinctive,
not template-default (frontend-design quality bar).

## Deploy

`site/.github` is wrong — workflow goes to repo `.github/workflows/site.yml`: build on push
touching `site/**` or `docs/buyer/**`, deploy to GitHub Pages, `CNAME vacuus.ufna.dev`.
Repo currently has no confirmed GitHub remote for Pages — workflow ships ready; enabling
Pages + DNS is the owner's step and is reported at handoff.

## Implementation plan (opus subagents)

1. **A — scaffold**: `site/` VitePress skeleton, config (nav/sidebar/search), sync-docs
   script, gitignore, GH Actions workflow, fab-package exclusion check. Verify: `npm run
   build` green with the six docs pages in.
2. **B — screenshots** (parallel with A): headless renders into `site/public/shots/`.
3. **C — landing + theme** (after A): `index.md` + theme CSS + home components.
4. **D — /ai page** (after A, parallel with C): `ai.md` content.
5. **Integrate & verify** (main session): build, preview, Playwright screenshot review of
   both themes, link check, fix.
