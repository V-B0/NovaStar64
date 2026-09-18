#!/usr/bin/env python3
"""Compare two NovaStar64 state traces and report the first divergence.

Typical use — validate the recompiler against the interpreter:

    NOVA64_TRACE_STATE=/tmp/interp.trace \\
        ./build/nova64 rom.z64 --frames 600 --interpreter
    NOVA64_TRACE_STATE=/tmp/recomp.trace \\
        ./build/nova64 rom.z64 --frames 600 --recompiler
    scripts/compare-trace.py /tmp/interp.trace /tmp/recomp.trace

Or check that a rewind or a loaded save state replays exactly. The trace of a
run that went back in time records some frames twice, and each repeat must
match what was recorded the first time:

    NOVA64_TRACE_STATE=/tmp/rewind.trace NOVA64_TRACE_RDRAM=1 ./build/nova64
    # ...play a game in the launcher and rewind or load a save state...
    scripts/compare-trace.py --replay /tmp/rewind.trace

Exit status is 0 when the traces agree and 1 when they diverge, so this can be
used directly as a regression check in CI.
"""

import argparse
import sys

# "count" is an alignment column, deliberately not in FIELDS.
ALIGN = "count"
FIELDS = ["pc", "gpr", "hilo", "fpr", "cp0", "rdram"]


def load_passes(path):
    """Returns a list of {frame: {field: value}}, one per pass through a trace.

    A new pass starts wherever the frame number fails to increase, i.e. where
    the run went back in time (a rewind or a loaded save state).
    """
    passes, frames, previous = [], {}, None
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
            if previous is not None and frame <= previous:
                passes.append(frames)
                frames = {}
            frames[frame] = dict(zip([ALIGN] + FIELDS, parts[1:]))
            previous = frame
    passes.append(frames)
    return passes


def load(path):
    """Returns {frame: {field: value}} for a trace with no rewinds."""
    passes = load_passes(path)
    if len(passes) > 1:
        sys.exit(f"{path} goes back in time {len(passes) - 1} time(s); use --replay for it")
    return passes[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("baseline", help="reference trace (e.g. the interpreter)")
    ap.add_argument("candidate", nargs="?", help="trace to check against it")
    ap.add_argument("--replay", action="store_true",
                    help="check the frames a single trace recorded more than once")
    ap.add_argument("--ignore", action="append", default=[], metavar="FIELD",
                    help=f"field to skip; repeatable. One of: {', '.join(FIELDS)}")
    ap.add_argument("--max-report", type=int, default=5,
                    help="how many diverging frames to print (default: 5)")
    args = ap.parse_args()

    for field in args.ignore:
        if field not in FIELDS:
            sys.exit(f"unknown field {field!r}; expected one of {', '.join(FIELDS)}")

    checked = [f for f in FIELDS if f not in args.ignore]

    if args.replay:
        if args.candidate:
            sys.exit("--replay takes a single trace")
        passes = load_passes(args.baseline)
        if len(passes) < 2:
            sys.exit(f"{args.baseline} never goes back in time; nothing to replay")
        status = 0
        for i in range(1, len(passes)):
            status |= compare(passes[i - 1], passes[i], f"{args.baseline} pass {i}",
                              f"{args.baseline} pass {i + 1}", checked, args)
            print()
        return status

    if not args.candidate:
        sys.exit("give a candidate trace, or use --replay")
    return compare(load(args.baseline), load(args.candidate),
                   args.baseline, args.candidate, checked, args)


def compare(base, cand, base_name, cand_name, checked, args):
    """Prints the comparison of two traces; returns 0 if they agree, else 1."""

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

    print(f"baseline : {base_name}  ({len(base)} frames)")
    print(f"candidate: {cand_name}  ({len(cand)} frames)")
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
