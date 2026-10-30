"""Launch the real desktop with each bundled offline translation catalog."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile


exe = Path(sys.argv[1]).resolve()
scratch = Path(sys.argv[2]).resolve()
environment = os.environ.copy()
environment.update({
    "QT_QPA_PLATFORM": "offscreen",
    "QT_QUICK_BACKEND": "software",
})

with tempfile.TemporaryDirectory(prefix="rifelove-localization-", dir=scratch) as temporary:
    root = Path(temporary)
    screenshots: dict[str, str] = {}
    for language in ("zh_CN", "zh_TW", "en", "ja"):
        data = root / language
        data.mkdir()
        (data / "language.ini").write_text(f"[ui]\nlanguage={language}\n", encoding="utf-8")
        screenshot = root / f"{language}.png"
        result = subprocess.run(
            [str(exe), "--smoke-screenshot", str(screenshot), "--data-dir", str(data)],
            cwd=root,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=20,
        )
        if result.returncode != 0 or not screenshot.is_file() or screenshot.stat().st_size < 1024:
            raise RuntimeError(
                f"Desktop startup failed for {language} (exit {result.returncode}):\n{result.stdout}"
            )
        screenshots[language] = hashlib.sha256(screenshot.read_bytes()).hexdigest()

    if len(set(screenshots.values())) != len(screenshots):
        raise RuntimeError(f"One or more selected locales rendered identically: {screenshots}")
