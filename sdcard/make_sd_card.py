#!/usr/bin/env python3
"""Makes the SD card for HIPI.

    sdcard/make_sd_card.py                   # writes HIPI-SD-card.zip in the project folder
    sdcard/make_sd_card.py --zip FILE        # ... or to FILE
    sdcard/make_sd_card.py --to /media/$USER/HIPI
                                              # copies straight onto a mounted card

What goes on the card:
    README.txt   from sdcard/
    resources/   the Tape view pictures, from resources/ (the other files
                 there -- buttons, logo, fonts -- are built into the firmware)
    lif/         the example cassette(s), from sdcard/lif/

HIPI creates CONFIG.TXT, screenshots/ and logs/ by itself.

The zip is reproducible: fixed time stamps and a fixed order, so it only
changes when one of the files in it does. The build (CMakeLists.txt,
target hipi_sd_card) runs this script whenever one of them changes.
"""
import argparse
import shutil
import sys
import zipfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent      # (this file is in sdcard/)

# The pictures HIPI reads from the card (see include/sd_paths.h and
# src/plotterview.cpp)
PICTURES = ["hp82161a.bmp", "tape-in.bmp", "open.bmp", "leds.bmp", "reels.bmp"]

# Every entry gets this time stamp, so the zip is the same byte for byte
# for the same contents
STAMP = (2026, 1, 1, 0, 0, 0)


def card_files():
    """(path on the card, source file) for everything on the card, in order."""
    files = [("README.txt", PROJECT / "sdcard" / "README.txt")]
    files += [(f"resources/{name}", PROJECT / "resources" / name) for name in PICTURES]
    lif = PROJECT / "sdcard" / "lif"
    files += [(f"lif/{p.name}", p) for p in sorted(lif.iterdir()) if p.is_file()]
    missing = [str(src) for _, src in files if not src.is_file()]
    if missing:
        sys.exit("make_sd_card: missing " + ", ".join(missing))
    return files


def write_zip(path, files):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    with zipfile.ZipFile(tmp, "w") as z:
        for folder in ("resources/", "lif/"):
            info = zipfile.ZipInfo(folder, STAMP)
            info.external_attr = (0o40755 << 16) | 0x10        # a directory
            z.writestr(info, b"")
        for name, src in files:
            info = zipfile.ZipInfo(name, STAMP)
            info.external_attr = 0o644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, src.read_bytes())
    tmp.replace(path)
    print(f"make_sd_card: {path} ({len(files)} files)")


def copy_to(card, files):
    if not card.is_dir():
        sys.exit(f"make_sd_card: {card} is not a folder -- is the card mounted?")
    for name, src in files:
        dst = card / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        print(f"  {name}")
    print(f"make_sd_card: {len(files)} files copied to {card}")


def main():
    ap = argparse.ArgumentParser(description="Makes the SD card for HIPI.")
    ap.add_argument("--zip", type=Path, default=PROJECT / "HIPI-SD-card.zip",
                    help="where to write the zip (default: HIPI-SD-card.zip in the project folder)")
    ap.add_argument("--to", type=Path, metavar="CARD",
                    help="copy the files straight to this folder (a mounted SD card) instead")
    args = ap.parse_args()
    files = card_files()
    if args.to:
        copy_to(args.to, files)
    else:
        write_zip(args.zip, files)


if __name__ == "__main__":
    main()
