#!/usr/bin/env python3
"""Prove that a change touched only comments and layout.

Compares the code of every tracked C++ file with the same file at a git revision, after removing
comments and whitespace. Unlike asm-fingerprint.py it also covers files that are not compiled on this
machine (Windows-only code, headers nobody includes here). Adjacent string literals count as one
("a" "b" == "ab"), macro line-continuation backslashes are ignored, and `using` declarations are compared
as a set, because clang-format sorts them.

    scripts/verify/code-equivalence.py --base <git revision>

It also checks that no credit to another project was lost: for each file the number of mentions of
Project64, Mupen64Plus, GLideN64 and the like, and of licence and copyright notices, must not go down.
Exit status 1 if anything differs.
"""
import argparse
import re
import subprocess
import sys

TOKEN = re.compile(r'''
    (?P<raw>(?:u8|u|U|L)?R"(?P<d>[^()\\\s]{0,16})\((?:.|\n)*?\)(?P=d)")
  | (?P<comment>//(?:[^\n\\]|\\.|\\\n)*|/\*(?:.|\n)*?\*/)
  | (?P<str>(?:u8|u|U|L)?"(?:\\.|[^"\\\n])*")
  | (?P<num>0[xX][0-9a-fA-F']+[uUlL]*|\d[\d']*(?:\.\d*)?(?:[eE][+-]?\d+)?[uUlLfF]*|\.\d+(?:[eE][+-]?\d+)?[fF]?)
  | (?P<chr>(?:u8|u|U|L)?'(?:\\.|[^'\\\n])+')
  | (?P<id>[A-Za-z_]\w*)
  | (?P<ws>\s+)
  | (?P<other>.)
''', re.X)

CREDITS = {
    "project64": r"project\s?64", "pj64": r"pj64", "mupen": r"mupen", "glideN64": r"glide?n64",
    "angrylion": r"angrylion", "rt64": r"\brt64\b", "rice": r"rice\s?video", "gpl": r"\bgpl\b|gnu general",
    "copyright": r"copyright", "licence": r"licen[sc]e", "spdx": r"spdx",
}


def tokens(text):
    out = []
    for m in TOKEN.finditer(text):
        if m.lastgroup in ("comment", "ws", "d"):
            continue
        t = m.group(0)
        if t == "\\":
            continue
        if t.startswith('"') and out and out[-1].startswith('"') and not out[-1].startswith('R"'):
            out[-1] = out[-1][:-1] + t[1:]
            continue
        out.append(t)
    # using declarations, as a sorted set apart from the rest
    rest, usings, i = [], [], 0
    while i < len(out):
        if out[i] == "using":
            j = out.index(";", i) if ";" in out[i:] else len(out) - 1
            usings.append(" ".join(out[i:j + 1]))
            i = j + 1
        else:
            rest.append(out[i])
            i += 1
    return rest, sorted(usings)


def git(*a):
    return subprocess.run(["git", *a], capture_output=True, text=True, errors="replace").stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", required=True, help="the git revision to compare with")
    args = ap.parse_args()
    files = [f for f in git("ls-files", "nova64").splitlines()
             if f.endswith((".cpp", ".hpp", ".h", ".inc", ".mm", ".m", ".c"))
             and not re.match(r"nova64/(external|thirdparty|ThirdParty)/", f)]
    bad = 0
    for f in files:
        before = git("show", f"{args.base}:{f}")
        if not before:
            continue                                   # a new file
        try:
            now = open(f, errors="replace").read()
        except FileNotFoundError:
            print("removed:", f)
            continue
        (a, ua), (b, ub) = tokens(before.lstrip("﻿")), tokens(now.lstrip("﻿"))
        if a != b or ua != ub:
            bad += 1
            i = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
            print(f"code differs: {f} near token {i}: {a[max(0, i - 3):i + 4]} -> {b[max(0, i - 3):i + 4]}")
        for what, rx in CREDITS.items():
            if len(re.findall(rx, now, re.I)) < len(re.findall(rx, before, re.I)):
                bad += 1
                print(f"credit lost: {f}: fewer mentions of {what}")
    print(f"{len(files)} files compared with {args.base}: {bad} problem(s)")
    sys.exit(1 if bad else 0)


main()
