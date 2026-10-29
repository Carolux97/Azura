#!/usr/bin/env python3
"""
Downloads the Noto fonts the runtime's own menus fall back to, and cuts them down to the
characters they need, into runtime/assets/fonts/.

Barlow (the menus' font) only draws Latin scripts. The runtime merges these behind it
(OnConfigureFonts in runtime/src/azura_app.h):

  NotoSansCyrillic-{Medium,SemiBold}.ttf  Cyrillic, always (Russian, and Cyrillic gamertags);
  NotoSans{JP,KR,TC}-Medium.ttf           the player's language only, when it is Japanese,
                                          Korean or traditional Chinese.

The CJK fonts keep the characters of each language's usual national character set (Shift-JIS,
KS X 1001 plus every Hangul syllable, Big5), punctuation and full-width forms, and one weight,
which keeps them at about 11 MB together instead of about 38 MB.

Needs fontTools (`pip install fonttools`). Usage: python3 scripts/subset_fonts.py
"""
import os
import re
import sys
import tempfile
import urllib.request

from fontTools import subset

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "runtime", "assets", "fonts")

# Google Fonts only serves TTF (rather than WOFF2) to clients it does not recognise as browsers.
USER_AGENT = "Mozilla/4.0"

CYRILLIC = set(range(0x0400, 0x0530)) | set(range(0x1C80, 0x1C90)) | set(range(0x2DE0, 0x2E00)) | set(
    range(0xA640, 0xA6A0))
# Punctuation, CJK symbols and kana, and full-width forms, which every CJK font keeps.
CJK_COMMON = set(range(0x2000, 0x2070)) | set(range(0x3000, 0x3100)) | set(range(0xFF00, 0xFFF0))


def double_byte_set(codec):
    """The characters a double-byte national encoding can write."""
    chars = set()
    for lead in range(0x81, 0xFF):
        for trail in range(0x40, 0xFF):
            try:
                chars.update(ord(c) for c in bytes([lead, trail]).decode(codec))
            except UnicodeDecodeError:
                pass
    return chars


def font_url(family, weight):
    request = urllib.request.Request(
        f"https://fonts.googleapis.com/css2?family={family.replace(' ', '+')}:wght@{weight}",
        headers={"User-Agent": USER_AGENT})
    css = urllib.request.urlopen(request).read().decode()
    found = re.search(r"url\((https://[^)]+\.ttf)\)", css)
    if not found:
        sys.exit(f"No TTF for {family} {weight}")
    return found.group(1)


def make(family, weight, characters, out_name, work_dir):
    source = os.path.join(work_dir, out_name)
    urllib.request.urlretrieve(font_url(family, weight), source)
    options = subset.Options()
    options.layout_features = ["*"]
    options.hinting = False
    options.name_IDs = ["*"]
    font = subset.load_font(source, options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=characters)
    subsetter.subset(font)
    out = os.path.join(OUT_DIR, out_name)
    subset.save_font(font, out, options)
    print(f"{out_name}: {os.path.getsize(out) // 1024} KB")


def main():
    japanese = double_byte_set("shift_jis") | CJK_COMMON
    korean = double_byte_set("euc_kr") | set(range(0xAC00, 0xD7A4)) | set(range(0x3130, 0x3190)) | CJK_COMMON
    chinese = double_byte_set("big5") | CJK_COMMON
    with tempfile.TemporaryDirectory() as work_dir:
        make("Noto Sans", 500, CYRILLIC, "NotoSansCyrillic-Medium.ttf", work_dir)
        make("Noto Sans", 600, CYRILLIC, "NotoSansCyrillic-SemiBold.ttf", work_dir)
        make("Noto Sans JP", 500, japanese, "NotoSansJP-Medium.ttf", work_dir)
        make("Noto Sans KR", 500, korean, "NotoSansKR-Medium.ttf", work_dir)
        make("Noto Sans TC", 500, chinese, "NotoSansTC-Medium.ttf", work_dir)


if __name__ == "__main__":
    main()
