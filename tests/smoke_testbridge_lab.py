import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_smoke_testbridge_lab import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main())
