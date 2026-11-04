"""Pack the release zip: the two built DLLs and a README, to extract next to mhgurecomp.exe.
NVIDIA's own DLLs are never packed."""
import os
import sys
import zipfile


def main(version):
    here = os.path.dirname(os.path.abspath(__file__))
    dst = os.path.join(here, 'dist', 'mhgu-dlss-v%s.zip' % version)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in ('vulkan-1.dll', 'mhgudlss_nvngx.dll'):
            z.write(os.path.join(here, 'build', name), name)
        z.write(os.path.join(here, 'README.txt'), 'mhgu-dlss-README.txt')
    print('wrote %s' % dst)


if __name__ == '__main__':
    main(sys.argv[1])
