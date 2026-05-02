from pathlib import Path
import json
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def test_template_manifest_is_valid() -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools/template/validate_manifest.py"),
            "--root",
            str(ROOT),
        ],
        cwd=str(ROOT),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert result.returncode == 0, result.stdout


def test_template_manifest_links_existing_docs() -> None:
    manifest = json.loads(
        (ROOT / "templates/qt-qml-bgfx/template.manifest.json").read_text(encoding="utf-8")
    )
    for recipe in manifest["recipes"]:
        assert (ROOT / "templates/qt-qml-bgfx" / recipe).exists()
    assert (ROOT / "AGENTS.md").exists()
    assert (ROOT / "docs/AGENT_WORKFLOWS.md").exists()
