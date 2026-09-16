#!/usr/bin/env python3
"""Verify a linear format-patch series using a temporary Git index."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


class VerificationError(Exception):
    pass


class Git:
    def __init__(self, repo):
        self.repo = repo

    def run(self, *args, env=None):
        result = subprocess.run(
            ["git", "-C", str(self.repo), *args],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode:
            detail = (result.stderr or result.stdout).strip()
            raise VerificationError(f"git {args[0]} failed: {detail}")
        return result.stdout.strip()

    def commit(self, revision):
        return self.run("rev-parse", "--verify", "--end-of-options", revision + "^{commit}")

    def tree(self, commit):
        return self.run("rev-parse", commit + "^{tree}")

    def linear_series(self, base, head):
        self.run("merge-base", "--is-ancestor", base, head)
        rows = self.run("rev-list", "--reverse", "--parents", f"{base}..{head}")
        if not rows:
            raise VerificationError("The selected commit range is empty.")
        commits = []
        parent = base
        for row in rows.splitlines():
            fields = row.split()
            if len(fields) != 2 or fields[1] != parent:
                raise VerificationError("The commit range must be a linear series without merges.")
            commits.append(fields[0])
            parent = fields[0]
        return commits


def read_series(patch_dir):
    patches = []
    seen = set()
    for line in (patch_dir / "series").read_text().splitlines():
        name = line.strip()
        if not name or name.startswith("#"):
            continue
        path = (patch_dir / name).resolve()
        try:
            path.relative_to(patch_dir)
        except ValueError as exc:
            raise VerificationError(f"Patch is outside the patch directory: {name}") from exc
        if Path(name).is_absolute() or path in seen or not path.is_file():
            raise VerificationError(f"Invalid, duplicate, or missing patch: {name}")
        patches.append(path)
        seen.add(path)
    if not patches:
        raise VerificationError("The series file contains no patches.")
    return patches


def verify(args):
    git = Git(args.repo.resolve())
    base = git.commit(args.base)
    head = git.commit(args.head)
    commits = git.linear_series(base, head)
    patches = read_series(args.patch_dir.resolve())
    if len(patches) != len(commits):
        raise VerificationError(
            f"Patch count {len(patches)} differs from commit count {len(commits)}."
        )

    # Resolve references once, so a concurrent branch update cannot change the comparison.
    trees = [git.tree(commit) for commit in commits]
    previous = None
    if args.previous_head:
        previous_head = git.commit(args.previous_head)
        previous = git.linear_series(base, previous_head)
        if len(previous) != len(commits):
            raise VerificationError("Previous and current series have different lengths.")
        for old, tree in zip(previous, trees):
            if git.tree(old) != tree:
                raise VerificationError(f"Code tree changed after rewriting {old[:12]}.")

    git.run("diff", "--check", base, head)
    report = [f"Base: {base}", f"Head: {head}", f"Patches: {len(patches)}"]
    with tempfile.TemporaryDirectory(prefix="kernel-cve-series-") as scratch:
        env = os.environ.copy()
        env["GIT_INDEX_FILE"] = str(Path(scratch) / "index")
        git.run("read-tree", base, env=env)
        for position, (patch, commit, tree) in enumerate(zip(patches, commits, trees)):
            with patch.open("rb") as stream:
                first_line = stream.readline().rstrip(b"\r\n")
            match = re.fullmatch(rb"From ([0-9a-f]{40}|[0-9a-f]{64}) Mon Sep 17 00:00:00 2001", first_line)
            if not match or match.group(1).decode() != commit:
                raise VerificationError(f"Patch commit ID does not match {commit[:12]}: {patch.name}")
            git.run("apply", "--cached", "--check", str(patch), env=env)
            git.run("apply", "--cached", str(patch), env=env)
            applied_tree = git.run("write-tree", env=env)
            if applied_tree != tree:
                raise VerificationError(f"Applied tree differs from {commit[:12]}: {patch.name}")
            report.append(f"PASS: {patch.name} matches {commit[:12]}; tree {tree}.")
            if previous:
                report.append(f"PASS: {previous[position][:12]} -> {commit[:12]} has an unchanged tree.")

    report.append(f"Result tree: {trees[-1]}")
    report.append("PASS: each exported patch reproduces its committed tree.")
    if previous:
        report.append("PASS: all previous and current commit trees match.")
    print("\n".join(report))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd(), help="Target Git repository")
    parser.add_argument("--base", required=True, help="Commit immediately before the series")
    parser.add_argument("--head", default="HEAD", help="Last commit in the series (default: HEAD)")
    parser.add_argument("--patch-dir", type=Path, required=True, help="Directory containing series and patch files")
    parser.add_argument("--previous-head", help="Old head for per-commit tree comparison after message edits")
    args = parser.parse_args()
    try:
        verify(args)
    except (VerificationError, OSError, UnicodeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
