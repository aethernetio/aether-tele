#!/usr/bin/env python3
# Copyright 2026 Aethernet Inc.
"""Copy standalone sources, reindex, build revision B, prove checksum isolation."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def run(cmd, cwd=None):
    print("+", " ".join(cmd))
    subprocess.check_call(cmd, cwd=cwd)


def checksum_from_decode(exe: Path, space: str, blob: Path) -> tuple[int, str]:
    proc = subprocess.run(
        [str(exe), f"--phase=decode", "--space", space, str(blob)],
        capture_output=True,
        text=True,
        check=False,
    )
    return proc.returncode, proc.stdout + proc.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, help="space_standalone directory")
    parser.add_argument("--lib-root", required=True, help="aether-tele root")
    parser.add_argument("--report", required=True)
    parser.add_argument("--space", default="network")
    parser.add_argument("--build-dir", help="existing build dir for revision A exe")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    lib = Path(args.lib_root).resolve()
    report = Path(args.report).resolve()

    with tempfile.TemporaryDirectory(prefix="space_revB_") as tmp:
        dst = Path(tmp) / "space_standalone"
        shutil.copytree(
            root,
            dst,
            ignore=shutil.ignore_patterns("build", "CMakeFiles", "*.blob"),
        )
        run(
            [
                sys.executable,
                str(dst / "tools" / "reindex.py"),
                "--space",
                args.space,
                "--root",
                str(dst),
                "--report",
                str(report),
                "--apply",
            ]
        )
        bdir = Path(tmp) / "buildB"
        run(
            [
                "cmake",
                "-S",
                str(dst),
                "-B",
                str(bdir),
                "-DAE_TELE_BUILD_TESTS=OFF",
                "-DAE_TELE_BUILD_EXAMPLES=OFF",
                f"-DAE_TELE_SOURCE_DIR={lib}",
                "-DAE_TELE_SPACE_REINDEX_BUILD=ON",
            ]
        )
        run(["cmake", "--build", str(bdir), "--config", "Release", "--target", "space_standalone"])
        exe_b = bdir / "Release" / "space_standalone.exe"
        if not exe_b.exists():
            exe_b = bdir / "space_standalone"
        out_b = Path(tmp) / "outB"
        run([str(exe_b), "--phase=write", f"--dir={out_b}"])
        blob_b = out_b / f"{args.space}.blob"

        exe_a = None
        if args.build_dir:
            cand = Path(args.build_dir) / "Release" / "space_standalone.exe"
            if not cand.exists():
                cand = Path(args.build_dir) / "space_standalone.exe"
            if not cand.exists():
                cand = Path(args.build_dir) / "space_standalone"
            exe_a = cand
        blob_a = report.parent / f"{args.space}.blob"
        if exe_a and exe_a.exists() and blob_a.exists():
            rc_ab, out_ab = checksum_from_decode(exe_a, args.space, blob_b)
            rc_ba, out_ba = checksum_from_decode(exe_b, args.space, blob_a)
            rc_aa, out_aa = checksum_from_decode(exe_a, args.space, blob_a)
            rc_bb, out_bb = checksum_from_decode(exe_b, args.space, blob_b)
            print("A decoder vs B blob rc", rc_ab)
            print(out_ab)
            print("B decoder vs A blob rc", rc_ba)
            print(out_ba)
            print("A decoder vs A blob rc", rc_aa)
            print(out_aa[:400])
            print("B decoder vs B blob rc", rc_bb)
            print(out_bb[:400])
            if rc_ab == 0 or rc_ba == 0:
                print("ERROR: silent cross-revision decode", file=sys.stderr)
                return 1
            if rc_aa != 0 or rc_bb != 0:
                print("ERROR: matching revision failed", file=sys.stderr)
                return 1
            loc = [
                sys.executable,
                str(root / "tools" / "source_locations.py"),
                "--space",
                args.space,
            ]
            run(loc + ["--root", str(root), "--exe", str(exe_a), "--blob", str(blob_a)])
            run(loc + ["--root", str(dst), "--exe", str(exe_b), "--blob", str(blob_b)])
        print("reindex_revision ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
