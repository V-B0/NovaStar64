#!/usr/bin/env python3
"""Measure how much of NovaStar64's own C++ credits Project64 in its file header.

A source file counts as Project64-derived when one of its first 40 lines
mentions "Project64" (case-insensitive) -- that covers every attribution wording
in the tree ("Derived from", "Based on", "Ported from", ...) and deliberately
ignores passing mentions such as "PJ64-style word order". A derived file is
counted in full, so the figures are an upper bound: a file that credits
Project64 may contain a great deal of original work.

Usage:
    python3 nova64/scripts/measure-derivation.py              # plain table
    python3 nova64/scripts/measure-derivation.py --markdown   # for README.md
"""
import argparse
import os
import sys

EXTENSIONS = (".cpp", ".hpp", ".h", ".inc")
SUBSYSTEMS = ["N64SYS", "NovaRSP", "NovaGraphics", "NovaAudio", "NovaInput",
              "NovaLua", "UI", "common"]
HEADER_LINES = 40


def read_lines(path):
    with open(path, errors="replace") as f:
        return f.read().splitlines()


def is_derived(lines):
    return any("project64" in line.lower() for line in lines[:HEADER_LINES])


def measure(root):
    rows = []
    for name in SUBSYSTEMS + ["main.cpp"]:
        path = os.path.join(root, name)
        if os.path.isfile(path):
            files = [path]
        else:
            files = [os.path.join(dirpath, f)
                     for dirpath, _, names in os.walk(path)
                     for f in names if f.endswith(EXTENSIONS)]
        derived_files = derived_lines = total_lines = 0
        for f in files:
            lines = read_lines(f)
            total_lines += len(lines)
            if is_derived(lines):
                derived_files += 1
                derived_lines += len(lines)
        rows.append((name, derived_files, len(files), derived_lines, total_lines))
    return rows


def pct(part, whole):
    return f"{100 * part / whole:.0f}%" if whole else "0%"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--markdown", action="store_true", help="print a Markdown table")
    args = parser.parse_args()

    # scripts/ lives in nova64/, next to the source directories.
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    rows = measure(root)
    totals = [sum(r[i] for r in rows) for i in range(1, 5)]

    if args.markdown:
        print("| Subsystem | Files crediting Project64 | Lines in those files |")
        print("|---|---|---|")
        for name, df, tf, dl, tl in rows:
            label = "`main.cpp`" if name == "main.cpp" else f"`{name}/`"
            print(f"| {label} | {df} of {tf} ({pct(df, tf)}) | {dl:,} of {tl:,} ({pct(dl, tl)}) |")
        df, tf, dl, tl = totals
        print(f"| **Total** | **{df} of {tf} ({pct(df, tf)})** | **{dl:,} of {tl:,} ({pct(dl, tl)})** |")
    else:
        print(f"{'subsystem':<14}{'files':>12}{'lines':>20}")
        for name, df, tf, dl, tl in rows:
            print(f"{name:<14}{f'{df}/{tf}':>12}{f'{dl}/{tl}':>20}")
        df, tf, dl, tl = totals
        print(f"{'TOTAL':<14}{f'{df}/{tf}':>12}{f'{dl}/{tl}':>20}"
              f"   ({pct(df, tf)} of files, {pct(dl, tl)} of lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
