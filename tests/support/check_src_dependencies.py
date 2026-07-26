#!/usr/bin/env python3
"""Validate responsibility-qualified includes and the internal dependency DAG."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


RESPONSIBILITIES = (
    "common",
    "config",
    "storage",
    "components",
    "query",
    "indexing",
    "server",
    "app",
)

ALLOWED_DEPENDENCIES = {
    "common": {"common"},
    "config": {"common", "config"},
    "storage": {"common", "config", "storage"},
    "components": {"common", "config", "storage", "components"},
    "query": {"common", "config", "storage", "components", "query"},
    "indexing": {"common", "config", "storage", "components", "indexing"},
    "server": {
        "common",
        "config",
        "storage",
        "components",
        "query",
        "indexing",
        "server",
    },
    "app": set(RESPONSIBILITIES),
}

INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def validate(source_root: Path) -> list[str]:
    errors: list[str] = []
    for path in sorted(source_root.rglob("*")):
        if path.suffix not in {".c", ".h"}:
            continue
        relative = path.relative_to(source_root)
        responsibility = relative.parts[0]
        if responsibility not in RESPONSIBILITIES:
            errors.append(f"{relative}: source is outside a declared responsibility")
            continue
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            match = INCLUDE_PATTERN.match(line)
            if match is None:
                continue
            include = match.group(1)
            dependency = include.split("/", 1)[0]
            if "yappo_" in include and dependency not in RESPONSIBILITIES:
                errors.append(
                    f"{relative}:{line_number}: internal include must use a "
                    f"responsibility-qualified path: {include}"
                )
                continue
            if (
                dependency in RESPONSIBILITIES
                and dependency not in ALLOWED_DEPENDENCIES[responsibility]
            ):
                errors.append(
                    f"{relative}:{line_number}: {responsibility} must not include "
                    f"{dependency}: {include}"
                )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_root", type=Path)
    args = parser.parse_args()
    errors = validate(args.source_root.resolve())
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
