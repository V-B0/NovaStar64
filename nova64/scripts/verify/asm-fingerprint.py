#!/usr/bin/env python3
"""Prove that a change did not alter the compiled code.

Compiles every translation unit of the program to assembly (no debug information) and compares the
result with a baseline made before the change. Meant for changes that should not affect the program:
formatting, comments, renames of comments. The only differences allowed are the line numbers that
logging and assert macros embed (`__LINE__`), which show up as `mov` immediates; they are ignored.

    # 1. configure a build that exports compile commands, and build it once (generated files are needed)
    cmake -S nova64 -B /tmp/fp-build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    cmake --build /tmp/fp-build
    # 2. on the code before the change
    scripts/verify/asm-fingerprint.py dump /tmp/fp-base --build /tmp/fp-build
    # 3. after the change
    scripts/verify/asm-fingerprint.py check /tmp/fp-base --build /tmp/fp-build

Add path substrings after the directory to look at only some files. Exit status 1 if anything differs.
Do this on every CPU the code is built for: a file compiled only for x86-64 is not seen by an ARM64 build.
"""
import argparse
import concurrent.futures as cf
import os
import re
import shlex
import subprocess
import sys


def entries(build, repo, filters):
    import json
    with open(os.path.join(build, "compile_commands.json")) as f:
        db = json.load(f)
    out = []
    for e in db:
        path = os.path.realpath(os.path.join(e["directory"], e["file"]))
        if not path.startswith(repo + "/") or "/build" in path[len(repo):] or "/_deps/" in path:
            continue
        rel = path[len(repo) + 1:]
        if filters and not any(s in rel for s in filters):
            continue
        out.append((rel, e))
    return out


def assembly(item):
    rel, e = item
    argv, skip, cleaned = shlex.split(e["command"]), 0, []
    for a in argv:
        if skip:
            skip -= 1
        elif a in ("-o", "-MT", "-MF", "-MQ"):
            skip = 1
        elif a not in ("-c", "-MD", "-MMD", "-MP"):
            cleaned.append(a)
    r = subprocess.run(cleaned + ["-S", "-g0", "-o", "-"], cwd=e["directory"], capture_output=True, text=True)
    if r.returncode != 0:
        return rel, None, r.stderr[-300:]
    text = "\n".join(l for l in r.stdout.splitlines() if not l.lstrip().startswith((".file", ".ident")))
    return rel, text + "\n", ""


def normalised(text):
    text = re.sub(r"(?m)^(\s*mov\s+[wx]\d+, )#-?\d+.*$", r"\1#N", text)                                  # AArch64
    return re.sub(r"(?m)^(\s*mov[lqwb]?\s+)\$-?\d+(,\s*\S+)\s*(##.*)?$", r"\1$N\2", text)               # x86-64


def name(base, rel):
    return os.path.join(base, rel.replace("/", "__") + ".s")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["dump", "check"])
    ap.add_argument("baseline", help="folder holding (dump) or compared against (check) the baseline assembly")
    ap.add_argument("filters", nargs="*", help="only files whose path contains one of these")
    ap.add_argument("--build", required=True, help="build folder containing compile_commands.json")
    ap.add_argument("--repo", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."),
                    help="the nova64 source folder (default: this checkout)")
    args = ap.parse_args()
    repo = os.path.realpath(args.repo)
    items = entries(args.build, repo, args.filters)
    failed, differ = [], []
    os.makedirs(args.baseline, exist_ok=True)
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for rel, text, err in pool.map(assembly, items):
            if text is None:
                failed.append((rel, err))
            elif args.command == "dump":
                open(name(args.baseline, rel), "w").write(text)
            else:
                base = open(name(args.baseline, rel)).read()
                if text != base and normalised(text) != normalised(base):
                    differ.append(rel)
    verb = "written" if args.command == "dump" else "compared"
    print(f"{len(items)} translation units {verb}; {len(differ)} differ, {len(failed)} failed to compile")
    for rel in differ:
        print("  differs:", rel)
    for rel, err in failed:
        print("  failed:", rel, err.strip().splitlines()[-1] if err.strip() else "")
    sys.exit(1 if differ or failed else 0)


main()
