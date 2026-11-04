#!/bin/sh
# Builds vulkan-1.dll (the mhgu-dlss proxy). --install copies it next to mhgurecomp.exe.
set -e
cd "$(dirname "$0")"
ZIG=${ZIG:-H:/Games/MHGU/tools/zig-x86_64-windows-0.17.0/zig.exe}
SDK=${SDK:-H:/Games/MHGU/tools/sdk}
python gen_exports.py
mkdir -p build
"$ZIG" cc -target x86_64-windows-gnu -shared -O2 -Wall -Wno-unused-function \
	-I"$SDK/vulkan-headers/include" -I"$SDK/dlss/include" \
	-o build/vulkan-1.dll src/proxy.c src/ngx.c src/exports.S -ladvapi32 -luser32 -lversion
# The DLSS 5 runtime only takes calls from a module named *nvngx.dll, and checks the return address:
# no optimisation, so no call in it becomes a tail call.
"$ZIG" cc -target x86_64-windows-gnu -shared -O0 -fno-optimize-sibling-calls -Wall 	-I"$SDK/vulkan-headers/include" -o build/mhgudlss_nvngx.dll src/nr_satellite.c
rm -f build/*.lib build/*.pdb
echo built build/vulkan-1.dll build/mhgudlss_nvngx.dll
if [ "$1" = "--install" ]; then cp build/vulkan-1.dll build/mhgudlss_nvngx.dll .. && echo installed into ..; fi
