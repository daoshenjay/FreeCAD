# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the 96x96 previews used by the texture browser.

    pixi run python src/Mod/Material/tools/make_thumbs.py

Writes ``Resources/textures/_thumbs/<category>/<name>.jpg`` next to the images;
the installer copies the whole ``Resources`` tree.
"""

import os
import sys

from PySide import QtCore, QtGui

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Resources", "textures")
SIZE = 96


def make(source, target):
    image = QtGui.QImage(source)
    if image.isNull():
        return False
    scaled = image.scaled(
        SIZE,
        SIZE,
        QtCore.Qt.AspectRatioMode.KeepAspectRatioByExpanding,
        QtCore.Qt.TransformationMode.SmoothTransformation,
    )
    left = max(0, (scaled.width() - SIZE) // 2)
    top = max(0, (scaled.height() - SIZE) // 2)
    cropped = scaled.copy(left, top, SIZE, SIZE)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    return cropped.save(target, "JPG", 80)


def main():
    made = failed = 0
    for category in ("pattern", "bump"):
        folder = os.path.join(ROOT, category)
        if not os.path.isdir(folder):
            continue
        for filename in sorted(os.listdir(folder)):
            source = os.path.join(folder, filename)
            if not os.path.isfile(source):
                continue
            stem = os.path.splitext(filename)[0]
            target = os.path.join(ROOT, "_thumbs", category, stem + ".jpg")
            if make(source, target):
                made += 1
            else:
                failed += 1
                print("failed: {}".format(filename))
    print("thumbnails: {} written, {} failed".format(made, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
