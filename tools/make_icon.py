"""Dev-build icon from the game's emblem picture on the CD.
usage: make_icon.py <picture> out.ico [black]
  black: keep a solid black square behind the emblem (default: transparent)
  picture: an LBM (Alien Odyssey's A.LBM) or an FLI/FLC animation - FLI:
  first frame, the emblem above the title text (Quarantine's TITLE.FLI:
  the golden biohazard sign), 320x200 pixels shown at 4:3 so stretched 1.2x."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image

src = sys.argv[1]
if src.lower().endswith(('.fli', '.flc')):
    from fli import frames
    w, h, pix, pal = next(frames(src))
    im = Image.frombytes('P', (w, h), pix)
    im.putpalette(pal)
    im = im.convert('RGBA')
    # the emblem: the topmost block of lit rows (stops at the first dark gap)
    px = im.load()
    rows = [any(sum(px[x, y][:3]) >= 60 for x in range(w)) for y in range(h)]
    y0 = rows.index(True)
    y1 = y0
    while y1 < h and rows[y1]:
        y1 += 1
    im = im.crop((0, y0, w, y1))
    im = im.resize((im.width, int(im.height * 1.2 + 0.5)), Image.LANCZOS)
elif src.lower().endswith(('.png', '.bmp')):
    im = Image.open(src).convert('RGBA')   # a picture already cut out of the game (dev builds)
else:
    from lbm import load
    im = load(src).convert('RGBA')
px = im.load()
for y in range(im.height):
    for x in range(im.width):
        r, g, b, a = px[x, y]
        if r + g + b < 60:
            px[x, y] = (0, 0, 0, 0)
bb = im.getbbox()
im = im.crop(bb)
side = max(im.size)
side += side // 16
sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
sq.paste(im, ((side - im.width) // 2, (side - im.height) // 2))
if side < 256:   # small emblems: scale up so every icon size exists
    sq = sq.resize((256, 256), Image.LANCZOS)
if len(sys.argv) > 3 and sys.argv[3] == 'black':
    bg = Image.new('RGBA', sq.size, (0, 0, 0, 255))
    bg.alpha_composite(sq)
    sq = bg
sq.save(sys.argv[2], sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
