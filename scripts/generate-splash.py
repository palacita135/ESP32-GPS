"""Embed palacj.png as a static color splash."""

from pathlib import Path

from PIL import Image, ImageOps


def main():
    """Generate flash-resident indexed color pixels and a static preview."""
    root = Path(__file__).resolve().parents[1]
    with Image.open(root / "palacj.png") as source:
        image = ImageOps.fit(
            ImageOps.exif_transpose(source).convert("RGB"),
            (480, 320),
            method=Image.Resampling.NEAREST,
        ).quantize(colors=256, method=Image.Quantize.MEDIANCUT)
    pixel_data = image.load()
    pixels = [
        pixel_data[x, y]
        for y in range(image.height)
        for x in range(image.width)
    ]
    palette = image.getpalette()
    colors = [
        ((palette[i] >> 3) << 11) | ((palette[i + 1] >> 2) << 5)
        | (palette[i + 2] >> 3)
        for i in range(0, 768, 3)
    ]
    lines = [
        "// Generated from palacj.png by scripts/generate-splash.py.",
        "#pragma once",
        "#include <Arduino.h>",
        "namespace BootSplash {",
        "constexpr int width = 480;",
        "constexpr int height = 320;",
    ]
    for name, kind, values, digits in (
        ("palette", "uint16_t", colors, 4),
        ("pixels", "uint8_t", pixels, 2),
    ):
        lines.append(f"const {kind} {name}[] PROGMEM = {{")
        for offset in range(0, len(values), 16):
            lines.append("\t" + ", ".join(
                f"0x{value:0{digits}x}" for value in values[offset:offset + 16]
            ) + ",")
        lines.append("};")
    lines.extend(["} // namespace BootSplash", ""])
    (root / "include" / "boot_splash.h").write_text(
        "\n".join(lines), encoding="utf-8"
    )
    preview_dir = root / ".pio"
    preview_dir.mkdir(exist_ok=True)
    image.convert("RGB").save(preview_dir / "splash-preview.png")


if __name__ == "__main__":
    main()
