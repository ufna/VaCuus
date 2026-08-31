// The product's commercial facts, in one module because they land in four places: the hero
// eyebrow, the licence section, /license and the footer. Owner decision 2026-08-17 (bead
// VaCuus-1yc). COMMERCIAL.md carries the same terms in prose and is what a buyer is pointed
// at for the authoritative version, so the two files are edited together or not at all.

export const PRICE = '$69.99'
export const PRICE_UNIT = 'per project · perpetual'

// The direct channel: the checkout, which lives with the payment backend rather than on
// this site (ufna/vacuus-pay, private). Keeping the form there means no CORS, no JS in the
// static build, and no need to redeploy the site to change how a purchase is taken.
//
// buyHref() below still falls back to a prefilled mailto if this is ever set back to null,
// so the button stays a WORKING path rather than a disabled "coming soon".
export const BUY_URL = 'https://pay.vacuus.ufna.dev/buy'

// The store channel. Live as of 2026-08-31 (owner; bead VaCuus-bs3) — until then this was
// null, and that null is what rendered the Fab button disabled. This is Fab's short form;
// the canonical listing URL it resolves to is recorded in docs/fab/description.md, which is
// the one place that has to survive the short link. VaCuus.uplugin's MarketplaceURL carries
// the same URL and is set in the same pass: a descriptor pointing anywhere else sends the
// editor's own "Marketplace" button somewhere wrong.
export const FAB_URL = 'https://fab.com/s/6571fd1716eb'

// GitHub Releases is the download channel — all three engine archives plus SHA256SUMS.txt
// per release. No version number goes in the button label: the page names the version, and
// a hardcoded one rots (the status block said "1.0" for a week after 1.0.1 shipped).
export const DOWNLOAD_URL = 'https://github.com/ufna/VaCuus/releases'

// The transaction, as opposed to the licence: who sells, what arrives, what happens when a
// payment goes wrong. Its version date is stored on every order as the text the buyer
// accepted, so /terms and the checkout's TERMS_VERSION are edited together or not at all.
export const TERMS_URL = '/terms'

export const CONTACT_EMAIL = 'ufna@ufna.dev'
export const LICENSE_URL = 'https://github.com/ufna/VaCuus/blob/master/LICENSE.md'
export const COMMERCIAL_URL = 'https://github.com/ufna/VaCuus/blob/master/COMMERCIAL.md'

// The subject and body are filled in because the reply the author has to write is shorter
// when the request already names the terms it was quoted under and what it is for.
export function buyHref() {
  if (BUY_URL) return BUY_URL
  const subject = encodeURIComponent(`VaCuus commercial licence — ${PRICE} per project`)
  const body = encodeURIComponent(
    'Studio or name:\nTitle the licence is for:\nInvoice or purchase order needed:\n'
  )
  return `mailto:${CONTACT_EMAIL}?subject=${subject}&body=${body}`
}
