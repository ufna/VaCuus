# Contributing

Issues and questions are welcome at <https://github.com/ufna/VaCuus/issues> — a report
that names the file, the line and what you observed is worth ten guesses.

Pull requests are accepted under one hard condition: **the CLA**. VaCuus is
dual-licensed (BUSL 1.1 publicly, commercial licenses sold separately), which is only
possible if the Licensor holds sufficient rights to every line in the tree. Read
[`CLA.md`](CLA.md) and include the line

```
I have read CLA.md and I agree to its terms.
```

in your first pull request's description. PRs without it will not be merged, however
good the code is.

Two repository rules worth knowing before you write anything:

- **Comments explain *why* and cite engine or RmlUi source as `file:line`** — and every
  cited line must have actually been opened. A comment that restates the code is noise.
- **Claims need evidence.** For a bugfix, the proof standard is restore-the-bug: break
  it deliberately, watch the specific test fail, restore, and report both outcomes. A
  test that has never been seen to fail is not yet evidence.

The developer-facing build/host-project notes live in `docs/dev/`; the buyer-facing
docs in `docs/buyer/` are the product and follow the same evidence rules.
