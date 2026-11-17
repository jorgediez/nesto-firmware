"""The README's pictures: some of the simulator's screens, made small.

    python3 tools/readme_images.py [OUTDIR] [--panel CMD]

Takes the screens below with tools/screens.py, as CI does, then scales each to
240 pixels and cuts it to a palette, so a dozen of them weigh a few hundred KB
in the repository rather than megabytes. Run it (scripts/dev.sh
readme-images) when the screens change, and commit docs/images/.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

# The README's grid, in order: (screen in tools/screens.py, file in docs/images).
PICTURES = [
    ("home", "home"),
    ("light", "light"),
    ("light-colour-temperature", "light-colour-temperature"),
    ("climate", "climate"),
    ("cover", "cover"),
    ("home-fan", "switch"),
    ("home-scene", "scene"),
    ("notice-open", "notification"),
    ("standby-analog", "clock-analog"),
    ("standby-digital", "clock-digital"),
    ("wifi-keyboard", "wifi-keyboard"),
    ("settings-info", "technical-info"),
]
SIZE = 240
COLOURS = 128


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("outdir", type=Path, nargs="?", default=Path("/src/docs/images"))
    parser.add_argument("--panel", default="/build/sim/nesto", help="how to run the panel")
    args = parser.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)
    screens = Path(__file__).with_name("screens.py")
    with tempfile.TemporaryDirectory() as shots:
        names = [screen for screen, _ in PICTURES]
        subprocess.run(
            [sys.executable, str(screens), shots, "--panel", args.panel, "--only", *names],
            check=True,
        )
        for screen, picture in PICTURES:
            image = Image.open(Path(shots) / f"{screen}.png").convert("RGB")
            image = image.resize((SIZE, SIZE), Image.Resampling.LANCZOS)
            image = image.quantize(
                COLOURS, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE
            )
            out = args.outdir / f"{picture}.png"
            image.save(out, optimize=True)
            print(f"{out}: {out.stat().st_size // 1024} KiB")


if __name__ == "__main__":
    main()
