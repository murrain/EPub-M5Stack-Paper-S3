#!/usr/bin/env python3
"""
Download and pre-process public-domain paintings for the EPub-M5Stack-Paper-S3
sleep-screen wallpaper feature.

Output: SDCard/wallpapers/*.png + *.txt sidecar files.
Each PNG is 540×960, 8-bit grayscale, Floyd-Steinberg dithered to 16 levels.
Each TXT sidecar has four lines: Title / Artist / Date / Blurb.

To deploy: copy the SDCard/wallpapers/ folder to the root of the device's SD card.
"""

import os
import sys
import time
import urllib.request
import urllib.error

try:
    from PIL import Image, ImageEnhance, ImageOps
except ImportError:
    sys.exit("Pillow not found — run: pip install Pillow")

# Very large Google Art Project scans are fine — we immediately downsample.
Image.MAX_IMAGE_PIXELS = None

# ── Target screen dimensions ───────────────────────────────────────────────────
W, H = 540, 960

# ── Paintings ─────────────────────────────────────────────────────────────────
# (stem, url, title, artist, date, blurb)
PAINTINGS = [
    (
        "hokusai_great_wave",
        "https://upload.wikimedia.org/wikipedia/commons/a/a5/Tsunami_by_hokusai_19th_century.jpg",
        "The Great Wave off Kanagawa",
        "Katsushika Hokusai",
        "c. 1831",
        "Part of the Thirty-six Views of Mount Fuji series. Mount Fuji is visible in the background, "
        "dwarfed by the wave — a metaphor for nature's power over human endeavour.",
    ),
    (
        "friedrich_wanderer",
        "https://upload.wikimedia.org/wikipedia/commons/b/b9/Caspar_David_Friedrich_-_Wanderer_above_the_sea_of_fog.jpg",
        "Wanderer above the Sea of Fog",
        "Caspar David Friedrich",
        "1818",
        "The solitary figure gazes from a mountain peak into a sea of mist. "
        "A defining image of German Romanticism, it captures the Sublime — the awe of standing before overwhelming nature.",
    ),
    (
        "rembrandt_self_portrait",
        "https://upload.wikimedia.org/wikipedia/commons/b/bd/Rembrandt_van_Rijn_-_Self-Portrait_-_Google_Art_Project.jpg",
        "Self-Portrait at the Age of 63",
        "Rembrandt van Rijn",
        "1669",
        "One of Rembrandt's last self-portraits, painted the year of his death. "
        "His unsparing honesty about age set a standard for psychological depth that changed portraiture forever.",
    ),
    (
        "vermeer_pearl_earring",
        "https://upload.wikimedia.org/wikipedia/commons/d/d7/Meisje_met_de_parel.jpg",
        "Girl with a Pearl Earring",
        "Johannes Vermeer",
        "c. 1665",
        "Sometimes called the Mona Lisa of the North. "
        "The girl's sideways glance and enormous pearl have captivated viewers for centuries; her identity remains unknown.",
    ),
    (
        "durer_melencolia",
        "https://upload.wikimedia.org/wikipedia/commons/1/14/Melencolia_I_%28Durero%29.jpg",
        "Melencolia I",
        "Albrecht Dürer",
        "1514",
        "One of the most analysed images in art history. "
        "Its scattered tools, magic square, and brooding winged figure are thought to embody the melancholy of creative genius.",
    ),
    (
        "munch_scream",
        "https://upload.wikimedia.org/wikipedia/commons/c/c5/Edvard_Munch%2C_1893%2C_The_Scream%2C_oil%2C_tempera_and_pastel_on_cardboard%2C_91_x_73_cm%2C_National_Gallery_of_Norway.jpg",
        "The Scream",
        "Edvard Munch",
        "1893",
        "Munch described a moment of existential panic on a walk — 'I felt an unending scream piercing through nature.' "
        "The swirling sky became an icon of modern anxiety.",
    ),
    (
        "davinci_lady_ermine",
        "https://upload.wikimedia.org/wikipedia/commons/f/f9/Lady_with_an_Ermine_-_Leonardo_da_Vinci_-_Google_Art_Project.jpg",
        "Lady with an Ermine",
        "Leonardo da Vinci",
        "c. 1489–90",
        "One of only four surviving female portraits by Leonardo. "
        "The subject is Cecilia Gallerani, mistress of Ludovico Sforza; the ermine is both her heraldic emblem and a symbol of purity.",
    ),
    (
        "goya_saturn",
        "https://upload.wikimedia.org/wikipedia/commons/8/82/Francisco_de_Goya%2C_Saturno_devorando_a_su_hijo_%281819-1823%29.jpg",
        "Saturn Devouring His Son",
        "Francisco de Goya",
        "1819–23",
        "One of Goya's Black Paintings, painted directly on the walls of his house near Madrid. "
        "The mythological horror becomes a vision of madness and destructive obsession.",
    ),
]

# ── 16-level grayscale palette for Floyd-Steinberg dithering ──────────────────
_pal_img = Image.new("P", (1, 1))
_pal_data: list[int] = []
for _i in range(16):
    _v = round(_i * 255 / 15)
    _pal_data += [_v, _v, _v]
_pal_data += [0, 0, 0] * (256 - 16)
_pal_img.putpalette(_pal_data)


def crop_to_portrait(img: Image.Image) -> Image.Image:
    src_r = img.width / img.height
    tgt_r = W / H
    if src_r > tgt_r:
        new_w = int(img.height * tgt_r)
        x0 = (img.width - new_w) // 2
        img = img.crop((x0, 0, x0 + new_w, img.height))
    elif src_r < tgt_r:
        new_h = int(img.width / tgt_r)
        y0 = (img.height - new_h) // 2
        img = img.crop((0, y0, img.width, y0 + new_h))
    return img


def process(src_path: str, dst_path: str) -> None:
    img = Image.open(src_path).convert("RGB")
    img = crop_to_portrait(img)
    img = img.resize((W, H), Image.LANCZOS)
    img = img.convert("L")
    # Stretch the histogram to the full tonal range first (cutoff=2 ignores
    # the top/bottom 2% of pixels so specular highlights and deep shadows
    # don't pin the stretch).  This lifts shadow detail in dark paintings
    # like the Rembrandt without washing out well-lit ones.
    img = ImageOps.autocontrast(img, cutoff=2)
    img = ImageEnhance.Contrast(img).enhance(1.15)
    dithered = img.convert("RGB").quantize(
        palette=_pal_img, dither=Image.Dither.FLOYDSTEINBERG
    )
    dithered.convert("L").save(dst_path, "PNG", optimize=True, compress_level=9)
    print(f"  saved {os.path.basename(dst_path)}  ({os.path.getsize(dst_path) // 1024} KB)")


def write_sidecar(dst_dir: str, stem: str, title: str, artist: str,
                  date: str, blurb: str) -> None:
    path = os.path.join(dst_dir, stem + ".txt")
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"{title}\n{artist}\n{date}\n{blurb}\n")


def download(name: str, url: str, cache_dir: str) -> str | None:
    ext = os.path.splitext(url.split("?")[0])[1] or ".jpg"
    path = os.path.join(cache_dir, name + ext)
    if os.path.exists(path):
        print(f"  cached  {os.path.basename(path)}")
        return path
    print(f"  downloading {name} …", end=" ", flush=True)
    try:
        req = urllib.request.Request(
            url, headers={"User-Agent": "Mozilla/5.0 (EPub-M5Stack wallpaper tool)"}
        )
        with urllib.request.urlopen(req, timeout=60) as resp, open(path, "wb") as f:
            f.write(resp.read())
        print("done")
        time.sleep(2)
        return path
    except urllib.error.URLError as e:
        print(f"FAILED ({e})")
        return None


def main() -> None:
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root  = os.path.dirname(script_dir)
    out_dir    = os.path.join(repo_root, "SDCard", "wallpapers")
    cache_dir  = os.path.join(script_dir, ".wallpaper_cache")
    os.makedirs(out_dir,   exist_ok=True)
    os.makedirs(cache_dir, exist_ok=True)

    ok = 0
    for stem, url, title, artist, date, blurb in PAINTINGS:
        print(f"\n{stem}")
        src = download(stem, url, cache_dir)
        if not src:
            continue
        dst = os.path.join(out_dir, stem + ".png")
        try:
            process(src, dst)
            write_sidecar(out_dir, stem, title, artist, date, blurb)
            ok += 1
        except Exception as e:
            print(f"  ERROR: {e}")

    print(f"\n{ok}/{len(PAINTINGS)} wallpapers ready in {out_dir}")
    print("Copy SDCard/wallpapers/ to the root of the device SD card.")


if __name__ == "__main__":
    main()
