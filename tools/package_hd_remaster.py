"""Build the bundled mod from an upstream artwork checkout and the original ARENA0.BK / VS.BK."""

import argparse
from io import BytesIO
from pathlib import Path
from struct import unpack_from
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from PIL import Image


def scene_palette(path):
    data = path.read_bytes()
    width, height = unpack_from("<HH", data, 5)
    offset = 9
    while data[offset + 4] < 50:
        next_offset, = unpack_from("<I", data, offset)
        if not offset < next_offset <= len(data) - 5:
            raise ValueError(f"Invalid animation offset in {path}")
        offset = next_offset
    # Five-byte end marker, background pixels, then the number of palettes.
    start = offset + 5 + width * height
    raw = data[start + 1:start + 769]
    if not data[start] or len(raw) != 768 or max(raw) > 63:
        raise ValueError(f"Invalid VGA palette in {path}")
    return bytes((value << 2) | (value >> 4) for value in raw)


def indexed_sprite(path, palette):
    with Image.open(path) as image:
        if image.mode == "P":
            return path.read_bytes()
        rgba = image.convert("RGBA")
    # The remaster art uses game colours, stored as RGBA with binary alpha.
    alpha = rgba.getchannel("A")
    if any(level not in (0, 255) for _, level in alpha.getcolors()):
        raise ValueError(f"Expected binary transparency in {path}")
    rgb = rgba.convert("RGB")
    if rgb.getcolors(256) is None:
        raise ValueError(f"Expected palette artwork in {path}")
    indexed = rgb.quantize(dither=Image.Dither.NONE)
    source = indexed.getpalette()
    lut = [0] * 256
    # Slots 160–255 contain the game's sprite colours. Lower slots can
    # change with the fighter, arena background and lighting effects.
    for _, index in indexed.getcolors():
        color = source[index * 3:index * 3 + 3]
        lut[index] = min(range(160, 256), key=lambda k: sum(
            (a - b) ** 2 for a, b in zip(color, palette[k * 3:k * 3 + 3])
        ))
    result = indexed.point(lut)
    result.putpalette(palette)
    result.paste(0, mask=alpha.point(lambda a: 255 - a))
    output = BytesIO()
    result.save(output, format="PNG", transparency=0, bits=8)
    return output.getvalue()


def build(art, resources, output):
    arena = scene_palette(resources / "ARENA0.BK")
    versus = scene_palette(resources / "VS.BK")
    entries = {"manifest.ini": b'name = "HD Remaster"\nmod_api = "1.0"\nversion = "1.0.0"\n'}
    for folder in ("players", "scenes"):
        for path in sorted((art / folder).rglob("*")):
            if path.is_file():
                name = path.relative_to(art).as_posix()
                palette = versus if name.startswith("scenes/VS/") else arena
                entries[name] = indexed_sprite(path, palette) if path.suffix == ".png" else path.read_bytes()
    animations = {"round": 6, "you_lose": 8, "you_win": 9, "fight": 10, "ready": 11}
    for label, animation in animations.items():
        entries[f"scenes/common/{animation}/0.png"] = indexed_sprite(art / f"{label}_hd.png", arena)
    for number in range(1, 8):
        entries[f"scenes/common/7/{number - 1}.png"] = indexed_sprite(art / f"{number}_hd.png", arena)

    with ZipFile(output, "w") as archive:
        for name, data in sorted(entries.items()):
            archive.writestr(ZipInfo(name), data, compress_type=ZIP_DEFLATED)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("art", type=Path, help="openomf-hdremaster-mod checkout")
    parser.add_argument("resources", type=Path, help="original OMF2097 resources directory")
    parser.add_argument("output", type=Path, help="destination mod ZIP")
    args = parser.parse_args()
    build(args.art, args.resources, args.output)
