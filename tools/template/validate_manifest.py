#!/usr/bin/env python3
"""Validate the starter-kit template manifest without third-party packages."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


REQUIRED = {
    "name": str,
    "version": str,
    "description": str,
    "sourceTargets": list,
    "agentCapabilities": list,
    "requiredObjectNames": list,
    "verification": list,
    "recipes": list,
}


def cmake_test_names(root: Path) -> set[str]:
    names: set[str] = set()
    for cmake_path in root.rglob("CMakeLists.txt"):
        rel = cmake_path.relative_to(root)
        if rel.parts and rel.parts[0] in {".build", ".build-release", ".build-release-no-agent", "external"}:
            continue
        text = cmake_path.read_text(encoding="utf-8")
        names.update(re.findall(r"NAME\s+([A-Za-z0-9_.-]+)", text))
    return names


def validate_manifest(root: Path, manifest_path: Path) -> list[str]:
    errors: list[str] = []
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    for key, expected_type in REQUIRED.items():
        if key not in manifest:
            errors.append(f"missing required key: {key}")
            continue
        if not isinstance(manifest[key], expected_type):
            errors.append(f"{key} must be {expected_type.__name__}")

    template_dir = manifest_path.parent
    for recipe in manifest.get("recipes", []):
        path = template_dir / recipe
        if not path.exists():
            errors.append(f"recipe not found: {recipe}")

    defined_tests = cmake_test_names(root)
    for test_name in manifest.get("verification", []):
        if test_name not in defined_tests:
            errors.append(f"verification test not registered in CMakeLists.txt: {test_name}")

    for object_name in manifest.get("requiredObjectNames", []):
        if object_name not in (root / "src/app/qml/Main.qml").read_text(encoding="utf-8"):
            errors.append(f"required objectName not found in sample QML: {object_name}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path("templates/qt-qml-bgfx/template.manifest.json"),
    )
    args = parser.parse_args()

    root = args.root.resolve()
    manifest = args.manifest
    if not manifest.is_absolute():
        manifest = root / manifest

    errors = validate_manifest(root, manifest)
    if errors:
        for error in errors:
            print(error)
        return 1
    print(f"validated {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
