#!/usr/bin/env python3
"""Compare two NovaStar64 state traces and report the first divergence.

Typical use — validate the recompiler against the interpreter:

    NOVA64_TRACE_STATE=/tmp/interp.trace \\
        ./build/nova64 rom.z64 --frames 600 --interpreter
    NOVA64_TRACE_STATE=/tmp/recomp.trace \\
        ./build/nova64 rom.z64 --frames 600 --recompiler
    scripts/compare-trace.py /tmp/interp.trace /tmp/recomp.trace

Exit status is 0 when the traces agree and 1 when they diverge, so this can be
used directly as a regression check in CI.
"""

import argparse
import sys

# "count" is an alignment column, deliberately not in FIELDS.
ALIGN = "count"
FIELDS = ["pc", "gpr", "hilo", "fpr", "cp0", "rdram"]


def load(path):
    """Returns {frame: {field: value}} for one trace file."""
    frames = {}
    with open(path) as handle:
        for lineno, line in enumerate(handle, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            expected = len(FIELDS) + 2  # frame + count + fields
            if len(parts) != expected:
                sys.exit(f"{path}:{lineno}: expected {expected} fields, got {len(parts)}")
            try:
                frame = int(parts[0])
            except ValueError:
                sys.exit(f"{path}:{lineno}: frame number is not an integer")
            frames[frame] = dict(zip([ALIGN] + FIELDS, parts[1:]))
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("baseline", help="reference trace (e.g. the interpreter)")
    ap.add_argument("candidate", help="trace to check against it")
    ap.add_argument("--ignore", action="append", default=[], metavar="FIELD",
                    help=f"field to skip; repeatable. One of: {', '.join(FIELDS)}")
    ap.add_argument("--max-report", type=int, default=5,
                    help="how many diverging frames to print (default: 5)")
    args = ap.parse_args()

    for field in args.ignore:
        if field not in FIELDS:
            sys.exit(f"unknown field {field!r}; expected one of {', '.join(FIELDS)}")

    checked = [f for f in FIELDS if f not in args.ignore]

    base = load(args.baseline)
    cand = load(args.candidate)

    if not base or not cand:
        sys.exit("at least one trace contains no frames — did tracing actually run?")

    shared = sorted(set(base) & set(cand))
    if not shared:
        sys.exit("traces share no frame numbers — they may be from different runs")

    misaligned = [f for f in shared if base[f][ALIGN] != cand[f][ALIGN]]

    divergences = []
    for frame in shared:
        differing = [f for f in checked
                     if base[frame][f] != cand[frame][f]
                     # '-' means the field was not recorded in that run
                     and "-" not in (base[frame][f], cand[frame][f])]
        if differing:
            divergences.append((frame, differing))

    only_base = sorted(set(base) - set(cand))
    only_cand = sorted(set(cand) - set(base))

    print(f"baseline : {args.baseline}  ({len(base)} frames)")
    print(f"candidate: {args.candidate}  ({len(cand)} frames)")
    print(f"compared : {len(shared)} shared frames on [{', '.join(checked)}]")
    if args.ignore:
        print(f"ignored  : {', '.join(args.ignore)}")
    print()

    if misaligned:
        first = misaligned[0]
        print(f"ALIGNMENT: {len(misaligned)} of {len(shared)} frames were sampled at "
              f"different emulated instants (CP0 Count differs).")
        print(f"  first at frame {first}: "
              f"baseline count={base[first][ALIGN]} candidate={cand[first][ALIGN]}")
        print("  State differences on those frames are INCONCLUSIVE — the engines were")
        print("  simply caught at different points, not necessarily disagreeing.")
        print()
    else:
        print("ALIGNMENT: all shared frames sampled at identical Count. "
              "Differences below are real.")
        print()

    if not divergences:
        print("MATCH — no divergence on the compared fields.")
        if only_base or only_cand:
            print(f"note: {len(only_base)} frame(s) only in baseline, "
                  f"{len(only_cand)} only in candidate (different run lengths).")
        return 0

    aligned_div = [(f, d) for f, d in divergences if base[f][ALIGN] == cand[f][ALIGN]]
    if aligned_div:
        af, afields = aligned_div[0]
        print(f"CONFIRMED divergence at frame {af} (same Count, different state) "
              f"on: {', '.join(afields)}")
        print(f"  {len(aligned_div)} diverging frame(s) are properly aligned.")
        print()

    first_frame, first_fields = divergences[0]
    print(f"DIVERGE — first mismatch at frame {first_frame} "
          f"on: {', '.join(first_fields)}")
    print(f"total diverging frames: {len(divergences)} of {len(shared)}")
    print()

    for frame, differing in divergences[:args.max_report]:
        print(f"  frame {frame}:")
        for field in differing:
            print(f"    {field:<6} baseline={base[frame][field]}  "
                  f"candidate={cand[frame][field]}")
    if len(divergences) > args.max_report:
        print(f"  ... and {len(divergences) - args.max_report} more")

    print()
    print("The first diverging frame is where to start looking: state was still "
          "identical the frame before it.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
