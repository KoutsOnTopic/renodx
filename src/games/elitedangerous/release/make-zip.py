"""Pack the Starlight release zip from a built addon.

Usage: python make-zip.py <renodx-elitedangerous.addon64> <version> <out.zip>

The zip holds four files at its root, ready to drag into the game folder:
  renodx-elitedangerous.addon64    the addon given on the command line
  renodx-starlight.ini             ../renodx-starlight.ini (class table, policies)
  renodx-starlight-signatures.ini  the seed in this folder (see STARLIGHT-ARCHITECTURE.md, section 12)
  renodx-starlight-README.txt      README.txt in this folder, with the version,
                                   the seed's entry count and the MIT text filled in
Text files get CRLF line endings: players open them in Notepad, and the addon
itself rewrites the signatures file in text mode (CRLF on Windows). Every
entry gets one fixed timestamp, so the four files depend only on the
repository and the addon; the zip's own bytes also depend on Python's zlib
(the release workflow pins Python 3.13).
"""
import hashlib
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.dirname(HERE)  # src/games/elitedangerous
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(GAME)))  # repository root
ZIP_TIME = (2026, 1, 1, 0, 0, 0)


def crlf(data):
    return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


def read(path):
    with open(path, "rb") as f:
        return f.read()


def readme(version, seed):
    license_text = read(os.path.join(ROOT, "LICENSE")).decode("utf-8").replace("\r\n", "\n")
    # the permission text: everything after "MIT License" and the copyright line
    head, _, body = license_text.partition("Permission is hereby granted")
    if not head.startswith("MIT License") or not body:
        sys.exit("unexpected LICENSE layout")
    mit = ("Permission is hereby granted" + body).strip("\n")
    # entries as the addon reads them: comments start at '#' or ';'
    count = sum(1 for line in seed.decode("utf-8").splitlines() if "=" in re.split("[#;]", line)[0])
    text = read(os.path.join(HERE, "README.txt")).decode("utf-8")
    text = text.replace("__VERSION__", version).replace("__SIGCOUNT__", str(count))
    text = text.replace("__MIT__", "\n".join("  " + line if line else "" for line in mit.split("\n")))
    if "__" in text:
        sys.exit("placeholder left in README.txt")
    return text.encode("utf-8")


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    addon, version, out = sys.argv[1:]
    seed = read(os.path.join(HERE, "renodx-starlight-signatures.ini"))
    files = {
        "renodx-elitedangerous.addon64": read(addon),
        "renodx-starlight.ini": crlf(read(os.path.join(GAME, "renodx-starlight.ini"))),
        "renodx-starlight-signatures.ini": crlf(seed),
        "renodx-starlight-README.txt": crlf(readme(version, seed)),
    }
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            info = zipfile.ZipInfo(name, ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            info.create_system = 0  # as on Windows, whichever OS packs it
            z.writestr(info, data)
    for name, data in files.items():
        print(f"{hashlib.sha256(data).hexdigest()}  {len(data):>9}  {name}")
    print(f"{hashlib.sha256(read(out)).hexdigest()}  {os.path.getsize(out):>9}  {os.path.basename(out)}")


if __name__ == "__main__":
    main()
