---
title: License and pricing
description: VaCuus is source-available under the Business Source License 1.1 — free to build with, free to ship non-commercially, $69.99 per project to ship commercially. What counts as commercial, when to buy, and what one licence covers.
prev: false
next: false
---

<!-- The price appears in this file exactly once, in the frontmatter description above, because
     a meta tag cannot import a module. Everywhere else on this page the figure comes from
     <LicenseBox />, which reads .vitepress/theme/commerce.js — so a price change is that file
     plus this one line, and nothing else on the site. -->

# License and pricing

<LicenseBox />

VaCuus is **source-available**: not open source, and not shareware either. The whole plugin —
every line of it, all three engine versions, the 227-test automation suite — is public, free to
download, and free to read, modify and build with. What you buy is the right to **ship a
commercial product** with it.

The licence is the [Business Source License 1.1](https://github.com/ufna/VaCuus/blob/master/LICENSE.md);
`LICENSE.md` in the repository carries its text with this product's parameters filled in. This
page is that licence in plain English, in the order the questions actually get asked. **If this
page and `LICENSE.md` ever disagree, `LICENSE.md` wins.**

## What counts as commercial

The licence states the test itself: a purpose is noncommercial if it is *"not intended for or
directed toward commercial advantage or monetary compensation"* (`LICENSE.md`, Additional Use
Grant). Everything below is guidance for reading that sentence. It is useful, and it is not the
licence.

**Needs a commercial licence**

- A game you sell.
- A free-to-play game with in-app purchases, ads, or any other monetisation.
- Paid DLC or a paid expansion, even for a base game that is free.
- A project you are paid to build for a client, including work-for-hire.
- A tool, app or product you sell that embeds VaCuus.

**Does not**

- A game you release for free, with nothing monetised in it or around it.
- Coursework, teaching material, academic projects.
- A portfolio piece, or a game-jam entry.
- Anything that never leaves your machine or your studio: prototypes, evaluation, internal
  tools, engine experiments.

If your case sits between the two — a free game with a donation link, a free demo that leads to
a paid title, a publisher-funded prototype — ask: <ufna@ufna.dev>. A one-line answer in writing
costs nothing and beats guessing.

## When to buy

**Build against the free copy for as long as you like, and buy before the commercial release
goes out.** That order is the expected path and not a loophole — `COMMERCIAL.md` says so in as
many words — and the licence you buy covers that release.

There is nothing to install and nothing to activate. VaCuus has no licence key, no seat server,
and no check of any kind anywhere in the plugin: the purchase is a licence, not a DRM step. The
copy you develop with is the copy you ship.

## What one licence covers

- **Scope** — one title, any team size. A second title needs a second licence.
- **Term** — perpetual. It covers the release you ship; there is no renewal to forget.
- **Updates** — every **1.x** release, including engine versions added within it.
- **Engines** — Unreal Engine 5.6, 5.7 and 5.8, built and tested from one source tree.
- **Platforms** — Windows, macOS, Linux, Android and iOS. Consoles on request.
- **Source** — the full source, in every channel, bought or not.

## The Change Date

BUSL is a licence with an expiry date, and it expires in the buyer's favour. **Each released
version of VaCuus becomes MIT four years after it is first distributed publicly** — that is the
Change Date and Change License in `LICENSE.md`, not a promise made on a web page. Version 1.0,
published in 2026, is MIT in 2030: for everyone, with no action from you and no cooperation
needed from the author.

So the price buys the current four years of a version, not a permanent dependency on someone
else's goodwill. If this project were abandoned tomorrow, the code you shipped on still becomes
plain open source on a date you can already write down.

## Third-party components

RmlUi 6.x and QuickJS-ng are vendored in-tree under their own **MIT** licences and are never
subject to the BUSL terms — in any channel, whether or not you buy. Each ships with its licence
beside it, and
[`THIRD-PARTY-NOTICES.md`](https://github.com/ufna/VaCuus/blob/master/THIRD-PARTY-NOTICES.md)
is the full list.

## How to buy

**Direct** — the *Buy a licence* button at the top of this page, and the fastest route. Payment
is in cryptocurrency; the moment it confirms, a certificate naming you and your title is
emailed to you and put at a permanent link, as a page and as a PDF. The transaction itself —
what arrives, when, and what happens if a payment goes wrong — is set out in the
[terms of sale](/terms).

**Fab** — [the store listing](https://fab.com/s/6571fd1716eb) is live, and buying there works
too: the Fab EULA's licence covers commercial use of the purchased copy. That route installs
the plugin **into the engine** rather than into your project, with binaries Epic built from
this same source — [Setup](/docs/setup) has the difference in a paragraph.

**Invoice, purchase order, source escrow, custom terms, consoles** — <ufna@ufna.dev>. A studio
that needs paperwork rather than a payment page is expected, not an exception.

## The exact text

- [`LICENSE.md`](https://github.com/ufna/VaCuus/blob/master/LICENSE.md) — the BUSL 1.1 text
  with this product's parameters: the licensor, the noncommercial Additional Use Grant, the
  four-year Change Date, and MIT as the Change License.
- [`COMMERCIAL.md`](https://github.com/ufna/VaCuus/blob/master/COMMERCIAL.md) — who needs to
  buy and how, in prose.

Both live in the repository, and both outrank this page.
