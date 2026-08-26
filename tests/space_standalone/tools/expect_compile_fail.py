#!/usr/bin/env python3
# Copyright 2026 Aethernet Inc.
"""Return 0 if the compiler command fails (expected compile-fail)."""

from __future__ import annotations

import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: expect_compile_fail.py <compiler> [args...]", file=sys.stderr)
        return 2
    proc = subprocess.run(sys.argv[1:], capture_output=True, text=True)
    if proc.returncode == 0:
        print("expected compile failure, but compilation succeeded", file=sys.stderr)
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        return 1
    print("compile failed as expected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
