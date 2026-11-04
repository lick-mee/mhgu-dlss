"""README banner: the logo and the tagline on a rounded dark card, like the other mods' banners.

python -I docs/make_banner.py docs/banner.png   (reads docs/logo.webp)"""
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

W, H, R = 2400, 640, 36
FONT = r'C:\Windows\Fonts\bahnschrift.ttf'
PARCH, SLATE = (239, 229, 204), (163, 172, 182)
HERE = os.path.dirname(os.path.abspath(__file__))


def card():
    img = Image.new('RGBA', (W, H))
    top, bottom = (28, 26, 30), (20, 18, 22)
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / (H - 1)
        d.line([(0, y), (W, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    # a cool glow behind the logo, from its blue and violet
    glow = Image.new('L', (W, H))
    ImageDraw.Draw(glow).ellipse((-200, -260, 1100, 900), fill=70)
    glow = glow.filter(ImageFilter.GaussianBlur(170))
    img = Image.composite(Image.new('RGBA', (W, H), (46, 36, 78, 255)), img, glow)
    mask = Image.new('L', (W * 4, H * 4))
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, W * 4 - 1, H * 4 - 1), R * 4, fill=255)
    img.putalpha(mask.resize((W, H), Image.LANCZOS))
    return img


def logo(height):
    """The logo with its black background taken out: alpha from brightness, so glow and sparks fade in."""
    im = Image.open(os.path.join(HERE, 'logo.webp')).convert('RGB')
    r, g, b = im.split()
    bright = ImageChops.lighter(ImageChops.lighter(r, g), b)
    alpha = bright.point(lambda v: 0 if v < 10 else min(255, (v - 10) * 6))
    # solid inside the artwork: fill the logo's own body so dark letter shadows stay opaque
    body = bright.point(lambda v: 255 if v > 24 else 0).filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.MinFilter(9))
    body = body.filter(ImageFilter.GaussianBlur(2))
    alpha = ImageChops.lighter(alpha, ImageChops.multiply(body, body.point(lambda v: 255 if v > 250 else 0)))
    im.putalpha(alpha)
    bbox = alpha.point(lambda v: 255 if v > 40 else 0).getbbox()
    im = im.crop(bbox)
    return im.resize((round(im.width * height / im.height), height), Image.LANCZOS)


def main(out):
    img = card()
    lg = logo(560)
    img.alpha_composite(lg, (70, (H - lg.height) // 2))
    d = ImageDraw.Draw(img)
    x = 70 + lg.width + 90
    d.text((x, 205), 'DLAA and neural rendering', font=ImageFont.truetype(FONT, 96), fill=PARCH)
    d.text((x, 362), 'for Monster Hunter Generations Ultimate on MHGU Recomp', font=ImageFont.truetype(FONT, 44),
           fill=SLATE)
    right = d.textbbox((x, 362), 'for Monster Hunter Generations Ultimate on MHGU Recomp',
                       font=ImageFont.truetype(FONT, 44))[2]
    print('text ends at', right, 'of', W)
    img.save(out)


if __name__ == '__main__':
    main(sys.argv[1])
