# The scan's planted-violation fixture (M6 spec §2(e))

One file per violation class Tools/fab_scan.sh detects, each planted so it trips
EXACTLY ONE class — that is what lets the self-test demand an exact report, not a
superset. The scan runs here FIRST on every invocation and must name precisely these
five before its verdict on the real package counts: a scan that has never been seen
to fail is not evidence (the project's restore-the-bug standard, applied to the
scanner itself).

| File | Class it trips | Why only that class |
|---|---|---|
| `planted.exe` | EXTENSION | text content, no exec bit, no shebang |
| `planted_execbit.txt` | EXECBIT | committed mode 100755; .txt is not blacklisted, no shebang |
| `planted_shebang.sh` | SHEBANG | `#!` first bytes; no exec bit; .sh is not blacklisted |
| `node_modules/planted.js` | NODE_MODULES | the DIRECTORY trips; the .js inside is inert |
| `planted_lfs_pointer.txt` | LFS_POINTER | the pointer signature line; .gitattributes here disables LFS so it commits raw |

This directory lives under `Tools/`, which no BuildPlugin filter rule includes
(BuildPluginCommand.Automation.cs:459-472 plus Config/FilterPlugin.ini), so the
plants can never reach a package — verified by the dry-run inventory.
