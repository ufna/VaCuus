# The scan's planted-violation fixtures (M6 spec §2(e))

One file per violation class Tools/fab_scan.sh detects, each planted so it trips
EXACTLY ONE class — that is what lets the self-test demand an exact report, not a
superset. The scan runs here FIRST on every invocation and must name precisely these
six before its verdict on the real package counts: a scan that has never been seen
to fail is not evidence (the project's restore-the-bug standard, applied to the
scanner itself).

| File | Class it trips | Why only that class |
|---|---|---|
| `planted.exe` | EXTENSION | text content, no exec bit, no shebang |
| `planted_execbit.txt` | EXECBIT | committed mode 100755; .txt is not blacklisted, no shebang |
| `planted_shebang.sh` | SHEBANG | `#!` first bytes; no exec bit; .sh is not blacklisted |
| `planted_elf.bin` | ELF_MAGIC | first four bytes \x7fELF; text after; no bit, no blacklisted name, no shebang — the renamed-payload shape the class exists for |
| `node_modules/planted.js` | NODE_MODULES | the DIRECTORY trips; the .js inside is inert |
| `planted_lfs_pointer.txt` | LFS_POINTER | the pointer signature line; .gitattributes here disables LFS so it commits raw. The OID is a REAL object of this repo (DevUIBundle's), not zeros: GitHub's push-time scanner parses every pointer-shaped blob and rejects the ref if the object is unknown — a zero-OID plant made the whole repo unpushable (2026-08-02; the dry-run doc §8 carries the story). The scan itself never reads the OID |

**The sibling fixture `../scan-fixture-whitelist/`** exercises the shebang-whitelist
branch, which no plant above reaches (review round 1's gap): a shebang file at
EXACTLY a whitelisted package-relative path (`Source/VaCuusJs/gen_relays.sh` — must
be forgiven) beside a near-miss (`gen_relays_nearmiss.sh` — must be reported).
Self-test 2 demands exactly that one-line report, so the whitelist is seen to both
admit and refuse on every run.

These directories live under `Tools/`, which no BuildPlugin filter rule includes
(BuildPluginCommand.Automation.cs:459-472 plus Config/FilterPlugin.ini), so the
plants can never reach a package — verified by the dry-run inventory.
