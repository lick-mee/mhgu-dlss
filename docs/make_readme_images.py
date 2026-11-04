"""Builds the README comparison images from the 4K screenshots.

python -I docs/make_readme_images.py <folder with the PNGs>
Writes docs/img/card-*.webp (split picture + zoomed panels) and docs/img/flip-*.webp (animated off/DLAA).
The save name on the character screen is pixelated."""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'img')
SLATE = (34, 48, 47)
PARCH = (239, 227, 194)
AMBER = (245, 184, 61)
DEEP = (24, 34, 33)
FONT = 'C:/Windows/Fonts/segoeuib.ttf'
W = 1920  # card width


def font(size):
    return ImageFont.truetype(FONT, size)


def load(folder, name, hide_name=False):
    im = Image.open(os.path.join(folder, name)).convert('RGB')
    if hide_name:
        box = (220, 468, 945, 570)
        r = im.crop(box)
        w, h = r.size
        im.paste(r.resize((w // 24, h // 24), Image.BILINEAR).resize((w, h), Image.NEAREST), box)
    return im


def pill(d, xy, text, size, fg, anchor):
    f = font(size)
    l, t, r, b = d.textbbox(xy, text, font=f, anchor=anchor)
    pad = size // 3
    d.rounded_rectangle((l - pad, t - pad // 1.4, r + pad, b + pad // 1.4), radius=pad, fill=DEEP)
    d.text(xy, text, font=f, fill=fg, anchor=anchor)


def card(a, b, la, lb, spot, zoom_px, path, title, crop=None):
    """a | b split at the middle of crop (default: the whole frame), a marker on spot, and both zoomed below."""
    crop = crop or (0, 0, a.width, a.height)
    fh = W * (crop[3] - crop[1]) // (crop[2] - crop[0])
    top = Image.new('RGB', (W, fh))
    sa, sb = a.crop(crop).resize((W, fh), Image.LANCZOS), b.crop(crop).resize((W, fh), Image.LANCZOS)
    top.paste(sa.crop((0, 0, W // 2, fh)), (0, 0))
    top.paste(sb.crop((W // 2, 0, W, fh)), (W // 2, 0))

    gap, pad = 16, 24
    pw = (W - 2 * pad - gap) // 2
    ph = pw * 9 // 16
    title_h = 76
    im = Image.new('RGB', (W, title_h + fh + pad + ph + pad), SLATE)
    d = ImageDraw.Draw(im)
    d.text((pad, title_h // 2), title, font=font(40), fill=PARCH, anchor='lm')
    im.paste(top, (0, title_h))
    d.rectangle((W // 2 - 3, title_h, W // 2 + 2, title_h + fh), fill=PARCH)
    # at the bottom by the divider: the game's HUD sits in the top corners
    pill(d, (W // 2 - 26, title_h + fh - 34), la, 40, PARCH, 'rb')
    pill(d, (W // 2 + 26, title_h + fh - 34), lb, 40, AMBER, 'lb')

    # zoom: zoom_px game pixels wide around spot, nearest-neighbour so the stairs stay visible
    sx, sy = int(spot[0] * a.width), int(spot[1] * a.height)
    zw, zh = zoom_px, zoom_px * 9 // 16
    box = (sx - zw // 2, sy - zh // 2, sx + zw // 2, sy + zh // 2)
    k = W / (crop[2] - crop[0])
    d.rectangle(((box[0] - crop[0]) * k, title_h + (box[1] - crop[1]) * k,
                 (box[2] - crop[0]) * k, title_h + (box[3] - crop[1]) * k), outline=AMBER, width=5)
    y = title_h + fh + pad
    for j, (src, lab, col) in enumerate(((a, la, PARCH), (b, lb, AMBER))):
        x = pad + j * (pw + gap)
        im.paste(src.crop(box).resize((pw, ph), Image.NEAREST), (x, y))
        d.rectangle((x, y, x + pw - 1, y + ph - 1), outline=col if j else (90, 105, 102), width=4)
        pill(d, (x + 22, y + 20), lab, 36, col, 'lt')
    im.save(path, 'WEBP', quality=90, method=6)
    print(path, os.path.getsize(path) // 1024, 'KB')


def flip(a, b, la, lb, spot, zoom_px, path, fw=1280, duration=1100):
    """Animated: a and b in turn, zoomed on spot (nearest-neighbour), or the whole frame when spot is None."""
    if spot:
        sx, sy = int(spot[0] * a.width), int(spot[1] * a.height)
        zw, zh = zoom_px, zoom_px * 9 // 16
        box = (sx - zw // 2, sy - zh // 2, sx + zw // 2, sy + zh // 2)
    fh = fw * 9 // 16
    frames = []
    for src, lab, col in ((a, la, PARCH), (b, lb, AMBER)):
        f = src.crop(box).resize((fw, fh), Image.NEAREST) if spot else src.resize((fw, fh), Image.LANCZOS)
        d = ImageDraw.Draw(f)
        pill(d, (30, fh - 28), lab, 52 * fw // 1280, col, 'lb')
        frames.append(f)
    frames[0].save(path, 'WEBP', save_all=True, append_images=frames[1:], duration=duration, loop=0, quality=92, method=6)
    print(path, os.path.getsize(path) // 1024, 'KB')


def main(folder):
    off = load(folder, 'NothingIngame.png')
    dlaa = load(folder, 'DLAAIngame.png')
    nr = load(folder, 'DLAANRIngame.png')
    s_off = load(folder, 'CharSelectNothing.png', True)
    s_dlaa = load(folder, 'CharSelectDLAA.png', True)
    card(off, dlaa, 'Off', 'DLAA', (.326, .468), 400, os.path.join(OUT, 'card-house-dlaa.webp'),
         'DLAA smooths the edges')
    flip(dlaa, nr, 'DLAA', 'DLAA + neural rendering', None, 0, os.path.join(OUT, 'flip-house-nr.webp'),
         fw=1600, duration=1500)
    card(s_off, s_dlaa, 'Off', 'DLAA', (.805, .61), 400, os.path.join(OUT, 'card-select-dlaa.webp'),
         'Character screen', crop=(1940, 160, 3800, 2120))
    flip(off, dlaa, 'Off', 'DLAA', (.417, .727), 320, os.path.join(OUT, 'flip-blade.webp'))


if __name__ == '__main__':
    main(sys.argv[1])
