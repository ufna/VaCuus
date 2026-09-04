---
title: Privacy
description: What this site collects about a visit, which service collects it, what is deliberately switched off, and how to opt out.
prev: false
next: false
---

<!-- Version 2026-09-05. Describes the tag in .vitepress/config.mjs as it is on that date; if
     the tag changes (webvisor, clickmap, a second service), this page changes with it. -->

# Privacy

**This site counts visits with Yandex Metrica.** That is the one request a page here makes to
anyone but this domain: fonts, styles and images are served from vacuus.ufna.dev itself.

## What the counter collects

- The pages you open and the order you open them in. A page view is sent when a page loads
  and again on each navigation inside the site.
- The address you arrived from (the referrer) and the outbound link you leave by, if any.
- Your browser and device class, screen size and language, and an approximate location that
  Yandex derives from your IP address.
- A click map: where on a page you click.
- Metrica sets its own cookies (named `_ym_*`) to tell a returning browser from a new one.

**Session Replay is off.** Metrica can record a visitor's whole session — pointer movement,
scrolling, clicks and form input — and this site's tag disables it (`webvisor: false` in
[the tag source](https://github.com/ufna/VaCuus/blob/master/site/.vitepress/config.mjs)), with
the matching switch off in the counter's settings.

## What it does not collect

Nothing you type. This site has no forms and sets no cookies of its own. Buying a commercial
licence happens on a separate checkout; what that needs — an email address and a name for the
certificate — is described on the [terms of sale](/terms) page.

## Who sees it

The statistics are visible to the site's author, Vladimir Alyamkin, in the Metrica interface.
The data is processed by Yandex under its own terms:
[Yandex privacy policy](https://yandex.com/legal/confidential/) and
[Metrica terms of use](https://yandex.com/legal/metrica_termsofuse/).

## How to opt out

- Yandex's own [opt-out add-on](https://yandex.com/support/metrica/general/opt-out.html) blocks
  Metrica counters on every site in that browser.
- Any content blocker that blocks `mc.yandex.ru` does the same. The site works fully without
  the tag.
- A browser set to block third-party scripts sends nothing to the counter.

Questions about this page: <a href="mailto:ufna@ufna.dev">ufna@ufna.dev</a>.
