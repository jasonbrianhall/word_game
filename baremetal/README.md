# Bare-metal Letterlock

Boots straight into Letterlock on x86_64 PCs and virtual machines: no OS, no
libc, no SDL. `../letterlock.cpp` is compiled unchanged; this folder supplies the
kernel and a software stand-in for the parts of SDL2 and SDL_ttf it uses.
Tested in QEMU (not yet on real hardware).

```
sudo apt install build-essential qemu-system-x86 grub-pc-bin grub-common xorriso mtools dosfstools gnu-efi ovmf
# Fedora: gcc-c++ qemu-system-x86 grub2-tools grub2-tools-extra grub2-pc-modules xorriso mtools dosfstools gnu-efi-devel edk2-ovmf
make run          # QEMU, direct kernel boot
make iso          # letterlock.iso: bootable CD / USB stick
make floppy       # letterlock-floppy.img: 1.44 MB FAT12 boot floppy (GRUB + gzipped kernel file)
make efi          # letterlock.efi: UEFI application (make run-efi tests it under OVMF)
```

**Input:** PS/2 or USB keyboard, and a PS/2 or USB mouse (USB devices on an
xHCI controller). In VirtualBox, click inside the window to capture the mouse
(the host key releases it). Esc quits the game; you then choose to play again
or reboot.

**Screen and sound:** 1024x768 (the game's 500x720 window, centered). Intel HD
Audio or AC97 (`make run SOUND=ac97`). Boot options: `audio=hda|ac97|off`,
`usb=off`.

**Memory:** 16 MB of RAM is enough. Statistics are kept in RAM until reboot.

**How it builds:** the game is compiled against the normal libstdc++/glibc
headers. `sdl.cpp` implements its SDL_Renderer, SDL_ttf, event and audio calls;
text comes from `fonts.h`, DejaVu Sans Mono pre-rasterized at the game's sizes
by `tools/gen_fonts.py`, so the embedded TTF is swapped for a stub at compile
time (`overrides/DejaVuMono_stub.h`) to keep the kernel floppy-sized.
`overrides/` also stands in for the file streams, and `runtime.cpp`,
`stdhooks.cpp` and `mathf.c` supply the few library functions the headers need.
Boot code and the HD Audio, AC97, PCI and xHCI drivers come from the Super
Mario Bros. and Willy the Worm bare-metal builds.
