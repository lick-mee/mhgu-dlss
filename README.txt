mhgu-dlss - NVIDIA DLAA and DLSS 5 neural rendering for MHGU Recomp

Install
  1. Extract this zip into the folder with mhgurecomp.exe (vulkan-1.dll and mhgudlss_nvngx.dll go next to it).
  2. Put NVIDIA's nvngx_dlss.dll there too, and for neural rendering nvngx_dlssnr.dll. They aren't in this zip.
  3. Start the game. Needs an NVIDIA RTX card and a current driver.

Everything is off at first. Press Insert in game, open "Plugin UI" and then "DLSS" to switch DLAA and neural
rendering on and pick the models. Your choices are saved to [DLSS] in mhgurecomp.ini and kept at the next start.

mhgu-dlss.log next to the exe says what was found and what failed.

Uninstall: delete vulkan-1.dll and mhgudlss_nvngx.dll.
