#!/usr/bin/env python3
"""Keep tests/liberty/dff.log.ok in step with dfflibmap's logmap_all().

WHY THIS EXISTS
---------------
Fork commit e31722fab added two rows to `logmap_all()` in
passes/techmap/dfflibmap.cc:

    logmap(ID($_DLATCH_N_));
    logmap(ID($_DLATCH_P_));

so `dfflibmap -info` reports those two primitives as unmapped against a
DFF-only library. It did not update `dff.log.ok`, the byte-exact golden the
dff.lib test diffs that output against, and `tests/liberty` was RED on every
image shipped between that commit and c85b963fe (vibeic-eda#111). The code was
right; the golden was stale.

WHY A DERIVED CHECK RATHER THAN A NOTE IN A FILE
------------------------------------------------
`dff.log.ok` is an UPSTREAM file carrying two fork-owned lines, which is exactly
what a daily `Merge upstream into main` drops on the floor without a symptom:
the merge is textually clean, the diff is two lines, and the only evidence is
the tail of a suite nobody reads. A comment cannot fail a build.

So this does not restate the golden — restating it would just be a second copy
to forget. It DERIVES the required cell list by parsing `logmap_all()` and
asserts the golden records exactly that list, in that order. It therefore fires
in both directions and stays quiet when upstream moves code and golden together:

    golden loses rows the code emits (a merge reverting ours)      -> FAIL
    a logmap() row added or removed without regenerating           -> FAIL
    upstream adds a primitive AND its golden row in one commit     -> pass

It lives at a path upstream does not have, so a merge cannot revert the guard
itself. Pure text analysis: no yosys binary, so it runs on an unbuilt tree.

Exit 0 in step, 1 drifted (with the offending rows named).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]

DFFLIBMAP_CC = REPO / "passes" / "techmap" / "dfflibmap.cc"
GOLDEN = HERE / "dff.log.ok"

#: `logmap(ID($_DFF_P_));` -> `$_DFF_P_`
LOGMAP_RE = re.compile(r"^\s*logmap\(ID\((\$\S+?)\)\);\s*$")
#: the line that opens the block logmap_all() prints
BLOCK_OPEN = "final dff cell mappings:"
#: `    unmapped dff cell: $_DFF_N_`
UNMAPPED_RE = re.compile(r"^ {4}unmapped dff cell: (\$\S+)$")
#: `    \dff _DFF_P_ (.CLK( C), ...` — logmap() prints the cell id with its
#: leading `$` stripped, so it is put back before comparing.
MAPPED_RE = re.compile(r"^ {4}\S+ (\S+) \(")


def cells_from_source(path: Path) -> list[str]:
    """The ordered cell ids logmap_all() emits, read off the C++ body."""
    text = path.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"^static void logmap_all\(\)\s*\{(.*?)^\}", text, re.S | re.M)
    if not m:
        raise SystemExit(f"{path}: could not locate the body of logmap_all()")
    cells = [hit.group(1) for hit in
             (LOGMAP_RE.match(l) for l in m.group(1).splitlines()) if hit]
    if not cells:
        # A parser that silently finds nothing would make this check pass on
        # everything, which is the failure mode it exists to prevent.
        raise SystemExit(f"{path}: logmap_all() parsed to zero cells — the parser is broken")
    return cells


def cells_from_golden(path: Path) -> list[str]:
    """The ordered cell ids the checked-in golden records."""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    try:
        start = next(i for i, l in enumerate(lines) if l.rstrip().endswith(BLOCK_OPEN))
    except StopIteration:
        raise SystemExit(f"{path}: no {BLOCK_OPEN!r} line — not a dfflibmap -info log")
    cells: list[str] = []
    for line in lines[start + 1:]:
        if not line.startswith("    "):
            break                       # `dfflegalize command line:` closes the block
        hit = UNMAPPED_RE.match(line)
        if hit:
            cells.append(hit.group(1))
            continue
        hit = MAPPED_RE.match(line)
        if hit:
            cells.append("$" + hit.group(1))
            continue
        raise SystemExit(f"{path}: unparsable mapping row: {line!r}")
    if not cells:
        raise SystemExit(f"{path}: the mapping block parsed to zero cells — the parser is broken")
    return cells


def main() -> int:
    src = cells_from_source(DFFLIBMAP_CC)
    gold = cells_from_golden(GOLDEN)

    if src == gold:
        print(f"OK: {GOLDEN.name} records all {len(src)} logmap_all() cells, in order")
        return 0

    print("FAIL: dfflibmap logmap_all() and tests/liberty/dff.log.ok have drifted apart")
    print(f"  source: {DFFLIBMAP_CC.relative_to(REPO)}  ({len(src)} cells)")
    print(f"  golden: {GOLDEN.relative_to(REPO)}  ({len(gold)} cells)")
    for c in [c for c in src if c not in gold]:
        print(f"  emitted by logmap_all() but absent from the golden: {c}")
    for c in [c for c in gold if c not in src]:
        print(f"  recorded in the golden but no longer emitted:       {c}")
    if set(src) == set(gold):
        print("  same set, different ORDER — the golden is byte-exact, so order matters")
    return 1


if __name__ == "__main__":
    sys.exit(main())
