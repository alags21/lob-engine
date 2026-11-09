#!/usr/bin/env python3
"""Guard against committing raw or large data (notably Nasdaq ITCH files).

Fails if any tracked or staged file is larger than the limit, or looks like a raw market-data or
archive file. Used by CI and by the optional pre-commit hook (scripts/hooks/pre-commit).

  python scripts/check_repo_size.py            # check everything tracked by git
  python scripts/check_repo_size.py --staged   # check only what is staged for commit
"""
import os, re, subprocess, sys

LIMIT_BYTES = 2 * 1024 * 1024
FORBIDDEN = re.compile(r"(\.gz$|\.zip$|\.7z$|\.tar$|\.bz2$|\.xz$|\.itch$|itch50|nasdaq_itch|\.pcap$|\.bin$)", re.I)
ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True).stdout.strip() or "."


def files(staged):
    cmd = ["git", "diff", "--cached", "--name-only", "--diff-filter=ACM"] if staged else ["git", "ls-files"]
    return [f for f in subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT).stdout.splitlines() if f]


def main():
    bad = []
    for f in files("--staged" in sys.argv):
        p = os.path.join(ROOT, f)
        if FORBIDDEN.search(f):
            bad.append(f"{f}: file name looks like raw market data or an archive")
        elif os.path.exists(p) and os.path.getsize(p) > LIMIT_BYTES:
            bad.append(f"{f}: {os.path.getsize(p):,} bytes exceeds the {LIMIT_BYTES:,} byte limit")
    if bad:
        print("repository size/data guard FAILED:\n  " + "\n  ".join(bad))
        print("Raw Nasdaq ITCH data must stay outside the repository; commit only counts, checksums and summaries.")
        return 1
    print("repository size/data guard ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
