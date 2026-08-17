# Site: the licence, said out loud — free to build, $69.99 to ship

**Date:** 2026-08-17 · **Status:** approved in session (owner settled model, price, channel, depth)

## Purpose

`vacuus.ufna.dev` makes a strong technical argument and then **asks for nothing**. The landing
page runs seven sections of measured evidence, ends on "What it is not", and drops into the
footer. There is no commercial call to action anywhere on it, and the licence — the thing that
decides whether a visitor may use the plugin at all — appears exactly once, in the smallest
type on the page, in the footer's `License` column (`site/.vitepress/theme/Home.vue:836-847`).

Two jobs, in this order of value:

1. **State the offer where it is read.** For BUSL 1.1 "free until you ship commercially" is not
   a legal disclaimer, it is the *entry-barrier removal* — the strongest reason to try the plugin
   today. It belongs in the hero, not in a footnote.
2. **Close the page.** A licence section placed after the objection-handling section becomes the
   closing CTA the landing has never had.

## Owner decisions, this session

These are **new product facts.** Before today the price and the licensing model did not exist in
the repository at all; `COMMERCIAL.md` named only the two channels.

| Fact | Value |
| --- | --- |
| Price | **$69.99** |
| Unit | **per project** (one title, any team size), **perpetual** |
| Version scope | updates within the same major version — every **1.x** release — are included |
| Channel today | **direct** (Heleket); `BUY_URL` does not exist yet |
| Fab | listing in technical review (`VaCuus-bs3`); a **disabled `soon` button**, not a hidden one |
| Free download | `https://github.com/ufna/VaCuus/releases` |
| Depth | landing section **plus** a `/license` page |

## What this supersedes

`docs/superpowers/specs/2026-08-12-vacuus-docs-site-design.md:34-35` reads: *"No FAQ page:
license/price are undecided (owner's open call) and the site must not invent them"*, and line 30
says the Fab button *"appears only when the listing URL exists"*. Both were correct then. Both
are replaced here: the price is decided, and the owner explicitly wants the Fab button visible
and disabled rather than absent.

The memory `vacuus-license-busl11` ends with `Price still undecided` and must be updated in the
same pass, or the next session will re-derive the old answer.

## Placement — four touch points

| Where | Change | Why there |
| --- | --- | --- |
| Hero eyebrow (`Home.vue:58-62`) | second line becomes `Free to build · $69.99 per project to ship commercially` | The only licence message a non-scrolling visitor will ever see |
| Nav (`config.mjs:118-122`) | `+ { text: 'License', link: '/license' }`, last | A price entry in the nav is a baseline conversion element; there is none today |
| **New landing section `08 License`** | the Free \| Commercial readout + honest note | Sections 01–06 build the argument, 07 answers objections, **08 closes** |
| `/license` (new page) | the FAQ | Catches the long tail — "what actually counts as commercial" |
| Footer `License` column (`Home.vue:836-847`) | keep the prose, link it to `/license`, name the price | Stays the legal footnote it already is |

Section 08 goes **after 07 "What it is not", before the footer.** Earlier is worse: a price shown
before the argument lands is a price with nothing behind it.

The message is stated **twice** on the landing (hero eyebrow, section 08) and not three times.
The `Status — released (1.0)` block was considered as a third spot and rejected: it is about
engine and platform support, and a licence sentence there would make the page repeat itself
before section 08 does any work.

## Design language — the binding constraint

**No pricing cards.** The page's whole vocabulary is a technical readout: mono type, corner ticks
(`.vc-ticks`), `.vc-readout` tables, hazard stripes, one amber accent. A SaaS-template tariff card
would read as an imported foreign object and would cost the page the credibility that every
measured number on it depends on.

Section 08 therefore reuses **`.vc-vs`** — the existing two-column comparison built for "Why not a
browser?" (`style.css:1053-1142`) — and **`.vc-why-honest`** (`style.css:1201-1236`) for the note
below it. The commercial column takes the accent treatment `.vc-vs-us` already carries; the free
column stays neutral. Zero new visual idioms; one new modifier class for the disabled button.

## Section 08 — structure

```
08   License                          ─────────────────   LICENSE.md ↗

  [lede, two sentences]

┌╴FREE ─────────────────────────╴┐  ┌╴COMMERCIAL ───────────────────╴┐  ← accent
│  $0                            │  │  $69.99                        │
│  no signup, no licence key     │  │  per project · perpetual       │
│  ─────────────────────────     │  │  ─────────────────────────     │
│  · four items                  │  │  [when it is needed, 1 sent.]  │
│                                │  │  · three items                 │
│  [ DOWNLOAD — FREE → ]         │  │  [ BUY A LICENSE → ]           │
│                                │  │  [ FAB · SOON ]  disabled      │
│  [foot line]                   │  │  [foot line]                   │
└────────────────────────────────┘  └────────────────────────────────┘

⚑  Build now, buy before you release.  [the honest note]
```

## Section 08 — copy, verbatim

**Header:** `08` · `License` · rule · cite → `LICENSE.md` (github blob link)

**Lede:**

> VaCuus is source-available under the **Business Source License 1.1**. In one sentence:
> everything is free until you ship something commercial, and a commercial release is
> **$69.99 per project**. No signup, no licence key, nothing in the plugin phones home.

**Free column** — heading `FREE`, figure `$0`, sub `no signup, no licence key`

- Read, fork and modify the source
- Prototypes, evaluation, internal tools, game jams
- Ship a free, hobby or academic game
- Full source, all three engines, the 227-test suite

CTA `DOWNLOAD — FREE →` → GitHub Releases.
Foot: *GitHub Releases — one archive per engine version, with `SHA256SUMS.txt` beside them.*
Second foot: *Not a trial. It is the same plugin, without a time limit.*

**Commercial column** — heading `COMMERCIAL`, figure `$69.99`, sub `per project · perpetual`

> Needed when you ship or operate a product for commercial advantage or monetary compensation.

- One title, any team size
- Perpetual — it covers the release you ship, and every 1.x update
- Direct from the author; invoice and custom terms on request

CTA `BUY A LICENSE →`. Second, disabled: `FAB · SOON`.
Foot: *Consoles, source escrow or purchase-order terms — ufna@ufna.dev*

**The honest note** (`.vc-why-honest`, strong lead-in `Build now, buy before you release.`):

> Developing against the free copy and buying when the game goes commercial is the expected
> path, not a loophole — the licence you buy covers that release. And it expires in your favour:
> **each version of VaCuus becomes MIT four years after it ships**, so $69.99 buys the current
> four years, not a permanent dependency.

Three lines in that copy carry the conversion, and each is doing a specific job:

- **"Not a trial"** kills the default assumption that a free copy is crippled. It is not, and
  saying so is worth more than any feature bullet in that column.
- **"Build now, buy before you release"** turns an unfamiliar licence into an order of
  operations. `COMMERCIAL.md:20-22` already says it in as many words; the site repeats it rather
  than inventing it.
- **"expires in your favour"** reframes the Change Date from legal trivia into a buyer guarantee.
  At $69.99 the live objection is not the money, it is *"am I signing up to a proprietary
  dependency"* — and the answer is no, in writing, with a date.

## `/license` — the page

New `site/license.md`. Prose page on the stock re-skinned doc layout, following `ai.md` exactly
(`prev: false` / `next: false`, `title` + `description` frontmatter). `<LicenseBox />` at the top,
then the FAQ. Section order is objection order:

1. **What counts as commercial** — the licence's own test quoted first (`LICENSE.md`, Additional
   Use Grant), then two plain-English lists offered *as guidance, not as the licence*: needs a
   licence (a game you sell; F2P with IAP or ads; paid DLC; a client project you are paid to
   build; a product you sell that embeds VaCuus) · does not (a free release with no monetisation;
   coursework; a portfolio piece; anything that never leaves your studio). Closes with: if your
   case sits between the two, ask — a written one-line answer costs nothing and beats guessing.
2. **When to buy** — build free for as long as you like, buy before the commercial release goes
   out. No key to install; the purchase is a licence, not a DRM step.
3. **What one licence covers** — one title, any team size, every 1.x update, all supported engines
   (5.6–5.8) and platforms. A second title needs a second licence. Consoles on request.
4. **The Change Date** — each released version becomes MIT four years after it first ships
   publicly. 1.0, shipped 2026, is MIT in 2030, for everyone, with no action from the buyer.
5. **Third-party components** — RmlUi 6.x and QuickJS-ng are vendored under their own MIT
   licences (`THIRD-PARTY-NOTICES.md`) and are never subject to the BUSL terms, bought or not.
6. **How to buy** — direct (Heleket) · Fab when the listing clears review, where the Fab EULA
   covers commercial use of the purchased copy · email for invoice, PO, escrow or custom terms.
7. **The exact text** — links to `LICENSE.md` and `COMMERCIAL.md`, and the standing rule:
   **if this page and those files ever disagree, those files win.**

## One source of truth

Price, buy URL, download URL and contact address land in at least four places (hero, section 08,
`/license`, footer). Two new files stop that from drifting:

- **`site/.vitepress/theme/commerce.js`** — `PRICE`, `PRICE_NOTE`, `BUY_URL`, `DOWNLOAD_URL`,
  `CONTACT_EMAIL`, `FAB_URL`. `BUY_URL` is `null` until the Heleket page exists; `LicenseBox`
  falls back to `mailto:` with a pre-filled subject, so the button is **live from day one** — a
  working path, not a "coming soon". `FAB_URL` is `null`, which is what renders the Fab button
  disabled: filling it in later turns it into a real link with no markup change.
- **`site/.vitepress/theme/LicenseBox.vue`** — the two-column readout itself, registered globally
  in `theme/index.js` beside `Home`, used by both section 08 and `/license`.

## Repo consistency — required, same pass

The site must not be the only place a product fact exists.

- **`COMMERCIAL.md`** — the "Requires a commercial license" section gains the model and the
  price ($69.99, per project, perpetual, 1.x updates included) and the direct channel; the Fab
  bullet's "link appears here when the listing is live" stays true and unchanged.
- **memory `vacuus-license-busl11`** — replace `Price still undecided` with the settled terms.
- Beads: file the follow-up for `BUY_URL` (blocked on the Heleket page) and for `FAB_URL`
  (blocked on `VaCuus-bs3`). `VaCuus-684`'s other two halves — README's install paragraph and
  `docs/buyer/setup.md` §0 — are **out of scope here** and stay on that bead.

## Adjacent factual errors found while reading the page

Both are one-liners in the files this work already touches. Neither is licence work; both are
wrong on the live site today.

1. **`Home.vue:59` says `Pre-release`** while the status block 150 lines below says
   `Status — released (1.0)`. The descriptor settles it: `VaCuus.uplugin` has
   `IsBetaVersion: false`, `VersionName: "1.0.1"`. The eyebrow becomes
   `Unreal Engine 5.6 – 5.8 · full source included`.
2. **`Home.vue:210` says `released (1.0)`** — 1.0.1 shipped on 2026-08-16. One digit.

No version string goes into the download button, for the same reason `(1.0)` rotted: the button
reads `DOWNLOAD — FREE` and the Releases page names the version.

## Honesty constraints

The 2026-08-12 spec's rule stands and now applies to commercial copy: the site claims nothing the
repo cannot back. Specifically —

- The BUSL parameters quoted on `/license` come from `LICENSE.md`, not from memory of how BUSL
  usually works.
- "What counts as commercial" is labelled guidance and points at the licence text for the test.
- **`BUSL 1.1` is not the headline.** Most UE developers have never met this licence; leading
  with the acronym buries the offer. The plain sentence leads, the acronym is cited under it.
- The Fab button says `soon` because the listing is in review — it does not claim a date.

## Files touched

| File | Change |
| --- | --- |
| `site/.vitepress/theme/commerce.js` | new — the constants |
| `site/.vitepress/theme/LicenseBox.vue` | new — the Free \| Commercial readout |
| `site/.vitepress/theme/index.js` | register `LicenseBox` |
| `site/.vitepress/theme/Home.vue` | hero eyebrow, section 08, footer licence column, status digit |
| `site/.vitepress/theme/style.css` | `.vc-lic*` styles + the disabled-button modifier |
| `site/license.md` | new — the FAQ page |
| `site/.vitepress/config.mjs` | nav `License` |
| `COMMERCIAL.md` | model + price |

## Verification

- `npm run build` in `site/` green; `/license` present in `.vitepress/dist`.
- `npm run preview` + Playwright: section 08 and `/license` screenshotted in **both** themes —
  the accent column and the disabled button must hold contrast in light and dark.
- Narrow viewport: `.vc-vs` collapses to one column at its existing breakpoint
  (`style.css:1060`); the two-button commercial column must not overflow.
- Every internal link resolves; `LICENSE.md` / `COMMERCIAL.md` cites point at the public blob.
- The `mailto:` fallback opens with its subject pre-filled, and the Fab button is not focusable
  and not clickable.
- Grep the *sources* for a `69.99` literal. Exactly two files may match: `commerce.js`, and the
  frontmatter `description` of `license.md` — a meta tag cannot import a module, and that is the
  single documented exception, commented in the file itself.

## Found during implementation

**The `.vp-doc` specificity trap, and it was a real defect, not a theoretical one.** The
component renders in two CSS contexts, and on `/license` VitePress's own `.vp-doc a` rule
(0,1,1 — `color: var(--vp-c-brand-1)`, `font-weight: 500`, a 250 ms colour transition) outranks
`.vc-btn-primary` (0,1,0). Measured on the built page before the fix: the *Buy a licence* button
rendered `rgb(138, 83, 0)` text on an `rgb(138, 83, 0)` background — **contrast ratio 1:1, the
label invisible** — and the *Download* button drifted to the brand amber in both themes. The
landing page was correct throughout, because it sets `markdownStyles: false` and has no prose
wrapper, so a screenshot of the landing alone would have passed.

Fixed by taking the CTA rules to three classes and restating the palette; re-measured, and all
four contexts (landing/`/license` × light/dark) now report identical computed colours. The
`.vc-btn-soon` rule had to go to three classes in the same pass, or the new generic CTA colour
would have outranked *it* and drawn the disabled button at full ink.

The lesson generalises past this component: **any component shared between the landing and a
markdown page has to be checked on the markdown page**, because that is the only one of the two
that is inside `.vp-doc`.

## Out of scope

Fab store copy (`VaCuus-jne`) · `MarketplaceURL` in the descriptor (`VaCuus-bs3`) · README and
`docs/buyer/setup.md` download links (`VaCuus-684`) · the analytics privacy note (`VaCuus-p3o`) ·
any change to `LICENSE.md`'s BUSL parameters.
