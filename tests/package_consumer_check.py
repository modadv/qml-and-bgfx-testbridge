from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def run(cmd: list[str], cwd: Path) -> None:
    subprocess.run(cmd, cwd=str(cwd), check=True)


def clean_directory(path: Path) -> None:
    root = ROOT.resolve()
    target = path.resolve()
    if root not in target.parents:
        raise RuntimeError(f"refusing to remove path outside repository: {target}")
    if target.exists():
        shutil.rmtree(target)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    install_dir = build_dir.parent / "package-consumer-install"
    consumer_build = build_dir.parent / "package-consumer-ctest"

    clean_directory(consumer_build)
    install_dir.mkdir(parents=True, exist_ok=True)

    run(["cmake", "--install", str(build_dir), "--config", args.config, "--prefix", str(install_dir)], ROOT)

    prefix_parts = [str(install_dir)]
    generator_candidates = [
        build_dir / "generators",
        build_dir / args.config / "generators",
    ]
    generators = next((path for path in generator_candidates if path.exists()), generator_candidates[0])
    if generators.exists():
        prefix_parts.append(str(generators))
    qt_bin = os.environ.get("TESTBRIDGE_QT_BIN")
    if qt_bin:
        prefix_parts.append(str(Path(qt_bin).parent))

    configure = [
        "cmake",
        "-S",
        str(ROOT / "tests/cmake_package_consumer"),
        "-B",
        str(consumer_build),
        f"-DCMAKE_BUILD_TYPE={args.config}",
        f"-DCMAKE_PREFIX_PATH={';'.join(prefix_parts)}",
    ]
    toolchain = generators / "conan_toolchain.cmake"
    if toolchain.exists():
        configure.append(f"-DCMAKE_TOOLCHAIN_FILE={toolchain}")
    run(configure, ROOT)
    build = ["cmake", "--build", str(consumer_build), "--config", args.config]
    if platform.system() == "Windows":
        build.extend(["--", "/m"])
    run(build, ROOT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
