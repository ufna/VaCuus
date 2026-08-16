#!/usr/bin/env node
// Sits at EXACTLY the whitelisted package-relative path
// (Web/packages/cli/bin/vacuus.mjs) so fab_scan's self-test 2 proves the whitelist
// ADMITS what it names: this shebang must NOT be reported in whitelist mode. Not the
// real CLI — see Web/packages/cli/bin/vacuus.mjs in the plugin tree for that.
//
// It used to be Source/VaCuusJs/gen_relays.sh, and moved here when Fab review round 1
// took the two gen_relays.sh out of the package (bead VaCuus-1hy): a fixture pointing
// at a path the whitelist no longer names would have failed the self-test, and a
// whitelist keeping the entry alive for the fixture's sake would have gone on forgiving
// a file that must now be reported if it ever comes back.
