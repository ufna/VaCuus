// The product's commercial facts, in one module because they land in four places: the hero
// eyebrow, the licence section, /license and the footer. Owner decision 2026-08-17 (bead
// VaCuus-1yc). COMMERCIAL.md carries the same terms in prose and is what a buyer is pointed
// at for the authoritative version, so the two files are edited together or not at all.

export const PRICE = '$69.99'
export const PRICE_UNIT = 'per project · perpetual'

// The direct channel. Null until the Heleket payment page exists, and buyHref() below then
// answers with a prefilled mailto — so the button is a WORKING path from the first deploy
// rather than a disabled "coming soon", which converts nothing and looks abandoned.
export const BUY_URL = null

// Null is what renders the Fab button disabled: the listing is in technical review (bead
// VaCuus-bs3). Filling this in turns the same markup into a live link, with no other edit.
export const FAB_URL = null

// GitHub Releases is the download channel — all three engine archives plus SHA256SUMS.txt
// per release. No version number goes in the button label: the page names the version, and
// a hardcoded one rots (the status block said "1.0" for a week after 1.0.1 shipped).
export const DOWNLOAD_URL = 'https://github.com/ufna/VaCuus/releases'

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
