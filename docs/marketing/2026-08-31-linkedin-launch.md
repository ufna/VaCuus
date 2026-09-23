# LinkedIn launch post — VaCuus 1.0 on Fab

**Status: published 2026-08-31.**
https://www.linkedin.com/feed/update/urn:li:activity:7500262316974809101/
Posted from the personal profile, https://www.linkedin.com/in/alyamkin/ — links kept in
the body (not the first comment) and no image attached, so the link card is the visual.
Written 2026-08-31, the day the Fab listing cleared review (commit `c2c9872`).

Angle: **UI an AI agent can write.** Chosen over the performance number and over a
plain "1.0 is out" because it is the claim no other UE UI plugin can make, and because
the performance number still gets its paragraph inside this frame rather than competing
with it.

Every number below traces to a shipped source — `docs/buyer/perf-guide.md`, the README,
or `VaCuus.uplugin`. Nothing here promises something a buyer cannot re-run.

---

## Post (English, 2,188 characters — LinkedIn's limit is 3,000)

Your coding agent cannot see the screen.

That, not code generation, is the real obstacle to letting it build game UI. It writes a menu, the menu renders wrong, and nothing in the loop tells it so.

VaCuus 1.0 shipped on Fab today. It renders Unreal Engine interfaces from HTML and CSS — menus, HUDs, in-world panels — entirely off the game thread. And it is built so an agent can close that loop on its own.

The UI surface is plain text. A screen is an .rml document plus .rcss styles: files on disk, not editor assets. An agent reads them, writes them and diffs them like any other source. No graph to wire and no recompile — the editor reloads a changed document during PIE.

Then the failure modes are made visible without eyes:

→ Parse errors reach the Unreal log with file and line, in every configuration, Shipping included
→ Screens render headless at 1920×1080, so a pixel check is just another build step
→ A support matrix for the style language is generated from the exact engine in the package, so the agent checks ground truth instead of guessing from web habits
→ 227 automation tests ship inside the package, source and fixtures included
→ AGENTS.md sits at the package root, so agents find the rules on their own

Onboarding is one line in your project's CLAUDE.md or AGENTS.md.

None of it would matter if it were slow. Layout, styling and draw-command recording all run on a dedicated UI thread; the game thread only enqueues input and reads back a snapshot. At the 1,732-node reference HUD that costs the game thread 0.012 ms per frame. Measured — the budget table ships with the plugin, so you can re-run it on your own machine.

It is deliberately not a browser. No fetch, no <a href> navigation, no CSS Grid, and none of the hundreds of megabytes and the permanent security-update treadmill an embedded Chromium brings. Just the part game UI actually uses.

UE 5.6, 5.7 and 5.8. Windows, macOS, Linux, Android and iOS from one source tree; consoles on request. Full source in the package.

Free for non-commercial use. $69.99 per project for commercial.

Fab → https://fab.com/s/6571fd1716eb
Docs → https://vacuus.ufna.dev

#UnrealEngine #GameDev #AI #GameUI #Cpp

---

## Alternative openings

The first two lines are all that show before "…see more", so the hook is the only part
that has to work in isolation.

1. **As written** — "Your coding agent cannot see the screen."
   Names the problem before naming the product. Strongest, and the rest of the post is
   built to pay it off.
2. **"I spent a year making game UI boring enough for a machine to write."**
   More personal, more shareable, weaker for a technical audience — it reads as a
   process story and buries what the thing is.
3. **"Unreal UI in HTML and CSS, off the game thread. VaCuus 1.0 is on Fab."**
   Says what it is in one line. Use if the post is aimed at UE developers who already
   want this and do not need convincing that the problem exists.

## How it rendered, and the two decisions as taken

- **Links: kept in the body.** LinkedIn demotes posts carrying an outbound link, and the
  alternative was to end at the price line and put both URLs in the first comment. The
  body was chosen so the card would render. LinkedIn rewrote the Fab URL to its own
  shortener, `https://lnkd.in/endxkWcD`; the docs URL was left alone.
- **Image: none attached.** An uploaded image and a link card are mutually exclusive, and
  the card was worth more than either gallery shot. What rendered is the compact card —
  small thumbnail beside "VaCuus · fab.com" — which is what LinkedIn gives a long text
  post, not a full-width image.

## Two things worth knowing for the next post

1. **`.md` filenames become links.** LinkedIn auto-linkified `AGENTS.md` and `CLAUDE.md`
   into `http://agents.md/` and `http://claude.md/` — `.md` is Moldova's TLD, so any
   bare filename ending in it is read as a hostname. Three occurrences in this post are
   now blue and clickable, pointing at domains that have nothing to do with the plugin.
   `agents.md` happens to be the AGENTS.md spec site, so that one lands somewhere
   topical; `claude.md` is unknown. There is no escaping syntax — avoiding the bare
   token is the only fix, and it costs the sentence.
2. **The short Fab URL is now verified.** `docs/fab/description.md` records the canonical
   listing URL as taken on the owner's word, because fab.com answers a scriptless
   request with a Cloudflare challenge and the redirect could not be checked. LinkedIn's
   crawler resolved it: the post's link card points at
   `https://www.fab.com/listings/570382ba-6641-43b1-ada9-8fb0259f9a57`, character for
   character the URL written down in that file. The short form goes where it should.
