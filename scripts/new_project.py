#!/usr/bin/env python3
"""Create a new Qt QML + bgfx Agent-ready desktop app from this starter kit."""

from __future__ import annotations

import argparse
import os
import re
import shutil
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

SKIP_DIRS = {
    ".git",
    ".build",
    ".build-release",
    ".build-release-no-agent",
    ".install-testbridge",
    ".pytest_cache",
    ".vs",
    ".vscode",
    "CMakeFiles",
    "artifacts",
    "assets",
    "__pycache__",
}

SKIP_SUFFIXES = {
    ".exe",
    ".dll",
    ".lib",
    ".pdb",
    ".obj",
    ".png",
    ".jpg",
    ".jpeg",
    ".bin",
    ".ico",
}

TEXT_SUFFIXES = {
    ".cmake",
    ".cpp",
    ".h",
    ".hpp",
    ".json",
    ".md",
    ".py",
    ".qml",
    ".sc",
    ".sh",
    ".txt",
    ".yml",
    ".yaml",
}


def snake_name(value: str) -> str:
    name = re.sub(r"[^A-Za-z0-9]+", "_", value).strip("_").lower()
    return name or "qt_bgfx_agent_app"


def slug_name(value: str) -> str:
    name = re.sub(r"[^A-Za-z0-9]+", "-", value).strip("-").lower()
    return name or "qt-bgfx-agent-app"


def pascal_name(value: str) -> str:
    parts = re.split(r"[^A-Za-z0-9]+", value)
    return "".join(part[:1].upper() + part[1:] for part in parts if part) or "QtBgfxAgentApp"


def should_skip(path: Path, include_external: bool) -> bool:
    rel = path.relative_to(ROOT)
    parts = set(rel.parts)
    if parts & SKIP_DIRS:
        return True
    if any(part.startswith(".build-") for part in rel.parts):
        return True
    if not include_external and rel.parts and rel.parts[0] == "external":
        return True
    if path.name in {"CMakeCache.txt", "cmake_install.cmake", "CMakeUserPresets.json"}:
        return True
    if path.suffix.lower() in SKIP_SUFFIXES:
        return True
    return False


def copy_tree(destination: Path, include_external: bool) -> None:
    destination = destination.resolve()
    for current, dir_names, file_names in os.walk(ROOT):
        current_path = Path(current)
        if current_path == destination or destination in current_path.parents:
            dir_names[:] = []
            continue

        kept_dirs: list[str] = []
        for dir_name in dir_names:
            child = current_path / dir_name
            if child == destination or destination in child.parents:
                continue
            if should_skip(child, include_external):
                continue
            kept_dirs.append(dir_name)
        dir_names[:] = kept_dirs

        rel_dir = current_path.relative_to(ROOT)
        if rel_dir.parts:
            (destination / rel_dir).mkdir(parents=True, exist_ok=True)

        for file_name in file_names:
            src = current_path / file_name
            if should_skip(src, include_external):
                continue
            rel = src.relative_to(ROOT)
            dst = destination / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            try:
                shutil.copy2(src, dst)
            except OSError as exc:
                raise RuntimeError(f"failed to copy {src} -> {dst}: {exc}") from exc


def rewrite_text(destination: Path, display_name: str, slug: str, target: str, module: str) -> None:
    replacements = {
        "testbridge-lab": slug,
        "testbridge_lab": target,
        "TestBridge Lab": display_name,
        "TestBridgeLab": module,
        "testbridge-lab-smoke": f"{slug}-smoke",
    }
    for path in destination.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        if "external" in path.relative_to(destination).parts:
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        updated = text
        for old, new in replacements.items():
            updated = updated.replace(old, new)
        if updated != text:
            path.write_text(updated, encoding="utf-8", newline="")


def main() -> int:
    parser = argparse.ArgumentParser(description="Create a new Agent-ready Qt QML + bgfx app.")
    parser.add_argument("name", help="Human-readable project name, e.g. 'Terrain Studio'")
    parser.add_argument("destination", type=Path, help="Output directory to create")
    parser.add_argument("--no-external", action="store_true", help="Do not copy external/bgfx.cmake")
    parser.add_argument("--force", action="store_true", help="Allow destination to already exist if empty")
    args = parser.parse_args()

    destination = args.destination.resolve()
    if destination.exists() and any(destination.iterdir()) and not args.force:
        raise SystemExit(f"destination is not empty: {destination}")
    destination.mkdir(parents=True, exist_ok=True)

    display = args.name
    slug = slug_name(display)
    target = snake_name(display)
    module = pascal_name(display)

    copy_tree(destination, include_external=not args.no_external)
    rewrite_text(destination, display, slug, target, module)

    print(f"created {display}")
    print(f"  directory: {destination}")
    print(f"  cmake project: {slug}")
    print(f"  app target: {target}")
    print(f"  qml module prefix: {module}")
    print("next: read AGENTS.md and docs/TEMPLATE_USAGE.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
