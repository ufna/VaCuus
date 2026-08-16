// The Yandex Metrika counter id for vacuus.ufna.dev.
//
// It lives here rather than in either consumer because both need it and a mismatch is
// silent: config.mjs injects the tag with it, theme/index.js sends the SPA pageviews to
// it, and a hit addressed to a counter that does not exist is simply dropped.
//
// A counter id is public by construction — it is served in the page source and appears in
// the query string of every request to mc.yandex.ru — so it is not infrastructure and does
// not belong in a secret.
export const METRIKA_ID = 111665991
