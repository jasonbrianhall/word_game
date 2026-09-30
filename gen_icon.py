#!/usr/bin/env python3
"""Draws the Letterlock icon: a padlock whose body is a green letter tile with
an "L", in the game's colors and its embedded DejaVu Sans Mono font.

    python3 gen_icon.py        (needs Pillow) -> icon.png (512 px) and icon.ico (16-256 px)
"""
import base64, io, re
from PIL import Image, ImageDraw, ImageFont

S = 1024                                   # draw big, then downsample for smooth edges
BG, GRAY, GREEN, YELLOW, WHITE = (18, 18, 19), (129, 131, 132), (83, 141, 78), (181, 159, 59), (255, 255, 255)

src = open("DejaVuMono.h").read().split("DEJAVU_REGULAR_FONT_B64[] =", 1)[1].split(";", 1)[0]
ttf = base64.b64decode("".join(re.findall(r'"([A-Za-z0-9+/=]*)"', src)))

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle([0, 0, S - 1, S - 1], radius=180, fill=BG)

# Shackle: a thick gray arch.
cx, sw = S // 2, 86
d.arc([cx - 250, 120, cx + 250, 620], start=180, end=360, fill=GRAY, width=sw)
d.rectangle([cx - 250, 370, cx - 250 + sw, 520], fill=GRAY)
d.rectangle([cx + 250 - sw, 370, cx + 250, 520], fill=GRAY)

# Body: a green tile with a white "L", and a small yellow tile tucked in the corner.
d.rounded_rectangle([cx - 340, 470, cx + 340, 930], radius=70, fill=GREEN)
font = ImageFont.truetype(io.BytesIO(ttf), 420)
d.text((cx, 700), "L", font=font, fill=WHITE, anchor="mm")
d.rounded_rectangle([cx + 170, 760, cx + 300, 890], radius=24, fill=YELLOW)

icon = img.resize((512, 512), Image.LANCZOS)
icon.save("icon.png")
icon.save("icon.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print("wrote icon.png and icon.ico")
