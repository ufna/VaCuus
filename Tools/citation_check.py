#!/usr/bin/env python3
"""Tools/citation_check.py -- the file:line citation gate (bead VaCuus-akj.6.37).

    python3 Tools/citation_check.py            # gate: exit 0 clean, 1 violations, 2 abort
    python3 Tools/citation_check.py --drift     # advisory worklist, always exit 0
    python3 Tools/citation_check.py --selftest  # just the planted-fixture leg

WHY THIS EXISTS. The house rule is "comments explain WHY and cite file:line, and you
open every line you cite". Citations are correct when written and rot silently when the
cited file moves: nothing compiles them, nothing tests them, and a stale one is WORSE
than none because it looks audited. The sweep that filed this bead found 51 stale
citations in 180 -- one of them pointing at a line that stated the OPPOSITE of the claim
it was offered as evidence for (VaCuusRender's LoadingPhase, cited at VaCuus.uplugin:31,
which is VaCuusJs's "Default"; VaCuusRender's PostConfigInit is :36).

WHAT THIS CAN AND CANNOT DO, stated up front because the gap is the whole design.
  MECHANISABLE  "the cited file exists" and "the cited line exists". No judgement, so
                it can gate. This is the cheap class: a moved/renamed/deleted file, a
                range that runs off the end.
  NOT           "the cited line still SAYS what the comment claims". That needs a
                reader. The sweep found this is where nearly all the rot lives: 51
                stale citations, and only ONE would have been caught by line-count
                alone. So the gate is a floor, not a proof -- and --drift exists to
                point a reader at the citations most likely to have rotted.

--drift RANKS RATHER THAN JUDGES. For citations into this repo's own non-vendored
files it compares two git facts: when the citing line was last written (blame) and when
the cited REGION last changed (log -L). A cited region that changed after its citation
was written is a drift SUSPECT -- not a defect. On the sweep that filed the bead this
ranking had 47 suspects of which 41 were genuinely stale; the 6 false positives were
regions that moved but still said the same thing. It is a worklist, so it never fails
the build, and its recall is NOT total: a citation written after the last edit to its
target can still be wrong (three of the 51 were), because being written late says
nothing about being written correctly.

SCOPE. Source/ and Config/, minus the vendored trees' OWN internals (RmlUi's and
quickjs's comments cite their upstream layout, which is not ours to keep true).

FAIL-CLOSED CONTRACT.
  - The gate judges only citations that resolve INSIDE this repo -- always present,
    always checkable, and the class that actually drifts. Citations into the engine,
    the host project or the content repo are counted and listed as EXTERNAL, never
    silently passed and never failed on a machine that lacks those trees.
  - A citation that resolves to more than one file in this repo is AMBIGUOUS and is
    checked against EVERY candidate; it fails if the line is missing from all of them.
  - Anything that cannot be read at all aborts (exit 2) rather than reporting clean
    over the part it managed to see.
  - THE CHECK MUST BE SEEN TO FAIL BEFORE IT MAY PASS: every gate run first runs
    itself over Tools/citation-fixture/, which carries one planted citation per
    violation class plus one deliberately-clean citation, and aborts unless the report
    is EXACT. A fixture that stops reproducing means the checker stopped checking.

KNOWN LIMITS, stated rather than discovered.
  - Prose that merely looks like a citation ("vacuus://x.rml:3" inside a quoted string)
    is indistinguishable from the real thing to a regex; ALLOW carries the handful of
    those, each with its reason.
  - A citation to a line that exists but is blank, or in the wrong function, passes the
    gate. That is the NOT row above, and --drift is the mitigation.
"""

import argparse
import collections
import io
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

SCAN_ROOTS = ["Source", "Config"]

# The vendored trees' own sources. Their comments cite THEIR upstream file layout; we
# neither own those citations nor re-line those files, so they are out of contract.
SKIP_PREFIX = [
    "Source/ThirdParty/RmlUi/Source",
    "Source/ThirdParty/RmlUi/Include",
    "Source/ThirdParty/RmlUi/Backends",
    "Source/ThirdParty/quickjs-ng",
]
SKIP_DIRS = {".git", "Intermediate", "Binaries", "DerivedDataCache", "node_modules"}

# Resolution index skips the same build outputs but KEEPS node_modules: comments in
# VaCuusJs cite preact's own sources by path, and those are real files here.
INDEX_SKIP_DIRS = {".git", "Intermediate", "Binaries", "DerivedDataCache"}

# Citations that are not citations, or whose target is genuinely outside every root.
# (citing-file-suffix, cited-spelling) -> why. Anything here is reported as ALLOWED,
# never as clean, so the list stays visible.
ALLOW = {
    ("VaCuusJsModules.cpp", "x.rml:3"):
        "not a citation -- the literal inline-script name \"vacuus://x.rml:3\", quoted in prose",
    ("VaCuusUMGWidget.h", "VaCuusUMGWidget.gen.cpp:22"):
        "UHT-generated, lives under Intermediate/ and is absent from a clean tree",
}

CITE = re.compile(
    r"(?<![A-Za-z0-9_])"
    r"((?:[A-Za-z0-9_.+-]+/)*[A-Za-z0-9_.+-]+\."
    r"(?:h|hpp|inl|c|cc|cpp|cs|py|sh|js|mjs|ts|rcss|rml|html|css|json|ini|md|txt|usf|ush|uplugin)"
    r")"
    r":([0-9]+(?:\s*[-,]\s*[0-9]+)*)"
)

Cite = collections.namedtuple("Cite", "file line target spec text")


class Abort(Exception):
    pass


def read_lines(path):
    try:
        with io.open(path, encoding="utf-8", errors="strict") as f:
            return f.read().split("\n")
    except UnicodeDecodeError:
        return None          # binary; not a citation carrier
    except OSError as e:
        raise Abort("cannot read %s: %s" % (path, e))


def walk_scan(root):
    """Files whose comments we own."""
    out = []
    for base in SCAN_ROOTS:
        start = os.path.join(root, base)
        if not os.path.isdir(start):
            continue
        for dp, dns, fns in os.walk(start):
            dns[:] = [d for d in dns if d not in SKIP_DIRS]
            rel = os.path.relpath(dp, root).replace(os.sep, "/")
            if any(rel == p or rel.startswith(p + "/") for p in SKIP_PREFIX):
                dns[:] = []
                continue
            for fn in fns:
                out.append(os.path.join(dp, fn))
    return sorted(out)


def build_index(root):
    """basename -> [repo-relative path]."""
    idx = collections.defaultdict(list)
    for dp, dns, fns in os.walk(root):
        dns[:] = [d for d in dns if d not in INDEX_SKIP_DIRS]
        for fn in fns:
            rel = os.path.relpath(os.path.join(dp, fn), root).replace(os.sep, "/")
            idx[fn].append(rel)
    return idx


def resolve(idx, target):
    """Candidates whose path ends with the cited path, component-wise; falls back to
    basename when the cited path is a hint rather than a suffix (engine-style
    'XInputDevice/XInputInterface.cpp')."""
    parts = target.split("/")
    cands = idx.get(parts[-1], [])
    exact = [c for c in cands if c.split("/")[-len(parts):] == parts]
    return exact or cands


def cited_lines(spec):
    return [int(x) for x in re.findall(r"\d+", spec)]


def collect(root):
    cites = []
    for path in walk_scan(root):
        lines = read_lines(path)
        if lines is None:
            continue
        rel = os.path.relpath(path, root).replace(os.sep, "/")
        for i, line in enumerate(lines, 1):
            for m in CITE.finditer(line):
                cites.append(Cite(rel, i, m.group(1), m.group(2), line.strip()))
    return cites


def check(root, external_roots):
    """-> (violations, tally). A violation is (class, Cite, detail)."""
    idx = build_index(root)
    ext_idx = {name: build_index(p) for name, p in external_roots.items()}
    linecount = {}

    def nlines(rel, base=root):
        key = (base, rel)
        if key not in linecount:
            p = os.path.join(base, rel)
            try:
                with open(p, "rb") as f:
                    data = f.read()
            except OSError as e:
                raise Abort("cannot read cited file %s: %s" % (p, e))
            linecount[key] = data.count(b"\n") + (1 if data and not data.endswith(b"\n") else 0)
        return linecount[key]

    violations = []
    tally = collections.Counter()

    for c in cites_of(root):
        key = (os.path.basename(c.file), "%s:%s" % (c.target, c.spec))
        if key in ALLOW:
            tally["ALLOWED"] += 1
            continue

        hi = max(cited_lines(c.spec))
        cands = resolve(idx, c.target)
        if [x for x in cands if nlines(x) >= hi]:
            tally["OK"] += 1
            continue

        # BASENAME COLLISIONS ARE THE COMMON CASE, NOT THE EDGE. The vendored RmlUi tree
        # ships Property.cpp, Texture.cpp and Platform.h, and so does the engine -- and
        # every one of those names is cited here meaning the ENGINE's file. An in-repo
        # match that is too short is therefore evidence of nothing until the external
        # trees have also been asked; resolving in-repo FIRST and stopping there reported
        # six sound citations as stale on this repo's first run.
        ext_hit = [n for n, ei in ext_idx.items()
                   if [x for x in resolve(ei, c.target)
                       if nlines(x, external_roots[n]) >= hi]]
        if ext_hit:
            tally["EXTERNAL"] += 1
            continue

        if cands:
            detail = ", ".join("%s has %d lines" % (x, nlines(x)) for x in cands[:4])
            if ext_idx:
                detail += " (and no %s file of that name reaches line %d)" % (
                    "/".join(sorted(ext_idx)), hi)
            violations.append(("STALE_RANGE", c, detail))
        elif ext_idx:
            violations.append(("UNRESOLVED", c, "no file of that name in this repo or "
                               + ", ".join(sorted(ext_idx))))
        else:
            tally["UNCHECKED"] += 1

    return violations, tally


_CITE_CACHE = {}


def cites_of(root):
    if root not in _CITE_CACHE:
        _CITE_CACHE[root] = collect(root)
    return _CITE_CACHE[root]


# --------------------------------------------------------------------------- selftest

FIXTURE = os.path.join(HERE, "citation-fixture")

# Exactly what the fixture must report. Changing the fixture without changing this is
# the failure the self-test exists to make loud.
EXPECT = [
    ("STALE_RANGE", "Source/planted.cpp", 12, "fixture_target.h:400"),
    ("UNRESOLVED", "Source/planted.cpp", 18, "ThisFileHasNeverExisted.cpp:7"),
]


def selftest():
    if not os.path.isdir(FIXTURE):
        print("ABORT: self-test fixture missing: %s" % FIXTURE, file=sys.stderr)
        return 2
    _CITE_CACHE.pop(FIXTURE, None)
    try:
        violations, tally = check(FIXTURE, {"pretend-engine": FIXTURE})
    except Abort as e:
        print("ABORT during self-test: %s" % e, file=sys.stderr)
        return 2

    got = sorted((k, c.file, c.line, "%s:%s" % (c.target, c.spec)) for k, c, _ in violations)
    want = sorted(EXPECT)
    if got != want:
        print("ABORT: self-test did not reproduce the planted violations.", file=sys.stderr)
        print("  expected: %r" % (want,), file=sys.stderr)
        print("  got:      %r" % (got,), file=sys.stderr)
        return 2
    if tally["OK"] < 1:
        print("ABORT: self-test saw no clean citation; the fixture's control is gone.",
              file=sys.stderr)
        return 2
    print("self-test: %d planted violations reproduced exactly, %d clean citation(s) "
          "left alone" % (len(want), tally["OK"]))
    return 0


# ------------------------------------------------------------------------------ drift

def git(*args):
    r = subprocess.run(["git", "-C", REPO] + list(args), capture_output=True, text=True)
    return r.stdout


def drift():
    tracked = set(git("ls-files").split("\n")) - {""}
    if not tracked:
        print("ABORT: not a git checkout; --drift needs history", file=sys.stderr)
        return 2
    byname = collections.defaultdict(list)
    for t in tracked:
        if not t.startswith("Source/ThirdParty/"):     # vendored trees are pinned
            byname[t.split("/")[-1]].append(t)

    blame_cache = {}

    def blame_time(path, line):
        if path not in blame_cache:
            times, cur = {}, 0
            for L in git("blame", "--line-porcelain", "-t", "--", path).split("\n"):
                if re.match(r"^[0-9a-f]{40} \d+ \d+", L):
                    cur = int(L.split()[2])
                elif L.startswith("author-time "):
                    times[cur] = int(L.split()[1])
            blame_cache[path] = times
        return blame_cache[path].get(line, 0)

    def region_time(path, lo, hi):
        for L in git("log", "-1", "--format=%at", "-L%d,%d:%s" % (lo, hi, path)).split("\n"):
            if L.strip().isdigit():
                return int(L.strip())
        return 0

    suspects = []
    scanned = 0
    for c in cites_of(REPO):
        parts = c.target.split("/")
        cands = byname.get(parts[-1], [])
        exact = [x for x in cands if x.split("/")[-len(parts):] == parts]
        pool = exact or cands
        if len(pool) != 1:
            continue
        tgt = pool[0]
        scanned += 1
        ls = cited_lines(c.spec)
        rt = region_time(tgt, min(ls), max(ls))
        bt = blame_time(c.file, c.line)
        if rt > bt:
            suspects.append((rt - bt, c, tgt, bt, rt))

    suspects.sort(reverse=True, key=lambda s: s[0])
    print("drift worklist: %d of %d in-repo citations have a cited region that changed "
          "AFTER the citation was written" % (len(suspects), scanned))
    print("(a suspect is a READING task, not a defect -- open the cited line)\n")
    import datetime
    fmt = lambda t: datetime.datetime.fromtimestamp(t).strftime("%Y-%m-%d") if t else "?"
    for _, c, tgt, bt, rt in suspects:
        print("%s:%d  cites %s:%s -> %s" % (c.file, c.line, c.target, c.spec, tgt))
        print("    written %s, cited region last changed %s" % (fmt(bt), fmt(rt)))
        print("    %s" % c.text[:150])
    return 0


# ------------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(add_help=True, description=__doc__.split("\n")[0])
    ap.add_argument("--selftest", action="store_true", help="run only the fixture leg")
    ap.add_argument("--drift", action="store_true",
                    help="advisory worklist of citations whose target moved after they were written")
    ap.add_argument("--engine", default=os.environ.get("UE_ROOT", "/w/Unreal/UnrealEngine"))
    ap.add_argument("--project", default=os.environ.get("VACUUS_PROJECT", "/w/Unreal/VcHost"))
    ap.add_argument("--content", default=os.environ.get("VACUUS_CONTENT", "/w/Unreal/VaCuusDemo"))
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if args.drift:
        return drift()

    rc = selftest()
    if rc != 0:
        return rc

    external = {}
    for name, path in (("engine", args.engine), ("project", args.project),
                       ("content", args.content)):
        if os.path.isdir(path):
            external[name] = path
    missing = [n for n in ("engine", "project", "content") if n not in external]

    try:
        violations, tally = check(REPO, external)
    except Abort as e:
        print("ABORT: %s" % e, file=sys.stderr)
        return 2

    total = sum(tally.values()) + len(violations)
    print("\n== citation gate ==")
    print("  scanned %d citations in %s" % (total, ", ".join(SCAN_ROOTS)))
    print("  %-10s %d  (resolve in this repo, cited line exists)" % ("in-repo:", tally["OK"]))
    print("  %-10s %d  (engine/host/content trees -- resolved, range not this gate's business)"
          % ("external:", tally["EXTERNAL"]))
    if tally["ALLOWED"]:
        print("  %-10s %d  (see ALLOW in this file, each with its reason)"
              % ("allowed:", tally["ALLOWED"]))
    if tally["UNCHECKED"]:
        print("  %-10s %d  (NOT judged: %s tree(s) absent, so 'unresolved' is not decidable)"
              % ("unchecked:", tally["UNCHECKED"], ", ".join(missing)))

    if violations:
        print("\n  %d VIOLATION(S) -- open each cited line and repoint it:" % len(violations))
        for kind, c, detail in violations:
            print("    %-12s %s:%d  cites %s:%s" % (kind, c.file, c.line, c.target, c.spec))
            print("                 %s" % detail)
            print("                 %s" % c.text[:140])
        return 1

    print("\n  CLEAN -- every in-repo citation points at a line that exists.")
    print("  This is a floor, not a proof: run --drift for the citations most likely")
    print("  to have rotted in place, and open them.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
