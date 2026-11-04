"""Pack the release zip: the two built DLLs, a README and the illustrated guide, to extract next to
mhgurecomp.exe. NVIDIA's own DLLs are never packed."""
import os
import sys
import zipfile

GUIDE = 'mhgu-dlss-guide'
# the pictures the guide page shows (docs/img also holds the README's cards, which it doesn't use)
GUIDE_IMAGES = ('house-off.webp', 'house-dlaa.webp', 'house-nr.webp', 'select-off.webp', 'select-dlaa.webp')


def main(version):
    here = os.path.dirname(os.path.abspath(__file__))
    dst = os.path.join(here, 'dist', 'mhgu-dlss-v%s.zip' % version)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in ('vulkan-1.dll', 'mhgudlss_nvngx.dll'):
            z.write(os.path.join(here, 'build', name), name)
        z.write(os.path.join(here, 'README.txt'), 'mhgu-dlss-README.txt')
        z.write(os.path.join(here, 'docs', 'index.html'), GUIDE + '/index.html')
        for name in GUIDE_IMAGES:
            # WebP is already compressed: stored as is
            z.write(os.path.join(here, 'docs', 'img', name), GUIDE + '/img/' + name, zipfile.ZIP_STORED)
    print('wrote %s' % dst)


if __name__ == '__main__':
    main(sys.argv[1])
