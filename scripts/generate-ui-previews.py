"""Generate illustrative 480x320 README screens. Requires Pillow.

Digits follow racing_digits.h geometry; text approximates TFT fonts.
The map is a synthetic street diagram, not downloaded geographic data.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUTPUT = Path(__file__).resolve().parents[1] / "docs" / "images"
FONT_DIR = Path("C:/Windows/Fonts")


def font(size):
    path = FONT_DIR / "arial.ttf"
    return ImageFont.truetype(str(path), size) if path.exists() else ImageFont.load_default(size=size)


def text(draw, xy, value, color="#d3d3d3", size=16, anchor="lt"):
    draw.text(xy, value, fill=color, font=font(size), anchor=anchor)


def digits(draw, value):
    masks = [0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F]
    bars = [(8, 0, 64, 16), (64, 10, 16, 59), (64, 75, 16, 59),
            (8, 128, 64, 16), (0, 75, 16, 59), (0, 10, 16, 59), (8, 64, 64, 16)]
    left = 240 - ((len(value) - 1) * 94 + 100) // 2
    for index, character in enumerate(value):
        for bit, (x, y, width, height) in enumerate(bars):
            if not masks[int(character)] & (1 << bit):
                continue
            if height > width:
                points = [(x + width // 2, y), (x + width, y + 5),
                          (x + width, y + height - 5), (x + width // 2, y + height),
                          (x, y + height - 5), (x, y + 5)]
            else:
                points = [(x + 5, y), (x + width - 5, y), (x + width, y + height // 2),
                          (x + width - 5, y + height), (x + 5, y + height), (x, y + height // 2)]
            draw.polygon([(left + index * 94 + px + (144 - py) * 20 // 144,
                           68 + py) for px, py in points], fill="white")


def speed(value="86", warning=False, red=False):
    image = Image.new("RGB", (480, 320), "red" if red else "black")
    draw = ImageDraw.Draw(image)
    text(draw, (16, 16), "PALACITA 135")
    text(draw, (464, 16), "SPEED", anchor="rt")
    if warning:
        text(draw, (240, 16), "TOO FAST", "white", anchor="mt")
    digits(draw, value)
    text(draw, (240, 235), "km/h", size=26, anchor="mm")
    draw.line((16, 255, 464, 255), fill="white" if red else "#7b7d7b")
    text(draw, (16, 267), "TRIP 12.45 km")
    text(draw, (464, 267), "MAX " + ("180" if warning else "112"), anchor="rt")
    text(draw, (16, 293), "GPS FIX  SAT 8", "white" if red else "#00ff00")
    text(draw, (464, 297), "SD RECORD", "white" if red else "#7b7d7b", 10, "rt")
    return image


def map_screen():
    image = Image.new("RGB", (480, 320), "#eeeade")
    draw = ImageDraw.Draw(image)
    draw.rectangle((330, 32, 479, 175), fill="#cce4bf")
    draw.polygon([(0, 225), (145, 205), (210, 255), (290, 300), (240, 300),
                  (155, 250), (0, 270)], fill="#a9d6e8")
    roads = [[(0, 110), (480, 110)], [(0, 200), (480, 200)],
             [(100, 32), (100, 300)], [(240, 32), (240, 300)],
             [(370, 32), (370, 300)]]
    for road in roads:
        draw.line(road, fill="#c5c1b7", width=16)
        draw.line(road, fill="white", width=12)
    for x, y in [(125, 140), (275, 140), (125, 60), (275, 235), (400, 230)]:
        draw.rectangle((x, y, x + 36, y + 28), fill="#d7cec3", outline="#bfb4a6")
    text(draw, (280, 180), "Example street", "#777777", 11)
    draw.line([(100, 260), (100, 200), (240, 200), (240, 166)], fill="red", width=3)
    draw.ellipse((233, 159, 247, 173), fill="white")
    draw.ellipse((235, 161, 245, 171), fill="#0000ff")
    draw.rectangle((0, 0, 479, 31), fill="black")
    draw.rectangle((0, 300, 479, 319), fill="black")
    text(draw, (10, 7), "PALACITA 135 / MAP", "white")
    text(draw, (470, 7), "WiFi connected", "white", 16, "rt")
    text(draw, (8, 306), "(c) OpenStreetMap contributors", "white", 10)
    text(draw, (470, 306), "OSM tiles", "white", 10, "rt")
    return image


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    speed().save(OUTPUT / "speed-ui.png")
    map_screen().save(OUTPUT / "map-ui.png")
    red = speed("180", warning=True, red=True)
    black = speed("180", warning=True)
    red.save(OUTPUT / "speed-warning.png")
    red.save(OUTPUT / "speed-warning.gif", save_all=True, append_images=[black],
             duration=[400, 400], loop=0, disposal=2)


if __name__ == "__main__":
    main()
