# Bare-metal build of Wordle.
#
#   make              build wordle.elf (the kernel)
#   make run          boot it in QEMU/KVM
#   make iso          bootable GRUB CD/USB image (wordle.iso)
#   make floppy       1.44 MB boot floppy (wordle-floppy.img)
#   make efi          wordle.efi, a UEFI application
#   make run-efi      boot wordle.efi under OVMF
#
# The game (../wordle.cpp) is compiled unchanged against the normal libstdc++
# headers; overrides/ stands in for SDL2, SDL_ttf and the file streams, and
# runtime.cpp/stdhooks.cpp/mathf.c supply the few library symbols those
# headers need. Nothing from libc/libstdc++ is linked except three small,
# self-contained libstdc++ objects: the std::map tree helpers (tree.o) and
# std::unordered_set's hashing (hashtable_c++0x.o, hash_bytes.o).

CXX      ?= g++
CC       ?= gcc
GAME     := ..
BUILD    ?= build/x86_64

GRUB_MKIMAGE  ?= $(shell command -v grub2-mkimage || command -v grub-mkimage)
GRUB_MKRESCUE ?= $(shell command -v grub2-mkrescue || command -v grub-mkrescue)
GRUB_I386     ?= $(firstword $(wildcard /usr/lib/grub/i386-pc /usr/lib/grub2/i386-pc))

# PIE=1 builds the position-independent kernel that wordle.efi embeds.
ifeq ($(PIE),1)
CODEGEN  := -fpie
BOOTDEFS := -DEFI
else
CODEGEN  := -fno-pic -fno-pie -mcmodel=kernel
BOOTDEFS :=
endif

LIBGCC    := $(shell $(CXX) -m64 -print-libgcc-file-name)
LIBSTDCXX := $(shell $(CXX) -m64 -print-file-name=libstdc++.a)

BASE     := -m64 $(CODEGEN) -mno-red-zone -msse2 -O2 -fno-stack-protector -fno-asynchronous-unwind-tables \
            -fno-builtin-printf -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DNDEBUG
CXXFLAGS := $(BASE) -std=gnu++17 -fno-rtti -fno-threadsafe-statics -D_GLIBCXX_ASSERTIONS \
            -Ioverrides -I$(GAME) -I. -Wno-write-strings
# Interrupt handlers never touch SSE (the stubs only save integer registers).
IRQFLAGS := $(filter-out -msse2,$(CXXFLAGS)) -mgeneral-regs-only
CFLAGS   := $(BASE) -w
LDFLAGS  := -nostdlib -static -no-pie -T linker.ld -z max-page-size=0x1000 --no-warn-rwx-segments

KERNEL := wordle.elf
ISO    := wordle.iso
FLOPPY := wordle-floppy.img

PLATFORM := kernel sdl runtime stdhooks audio pci usb
LIBOBJS  := tree.o hashtable_c++0x.o hash_bytes.o

OBJS := $(BUILD)/boot.o $(BUILD)/irq.o $(BUILD)/mathf.o \
        $(addprefix $(BUILD)/,$(addsuffix .o,$(PLATFORM))) \
        $(BUILD)/game/wordle.o \
        $(addprefix $(BUILD)/lib/,$(LIBOBJS))

HDRS := $(wildcard *.hpp) fonts.h overrides/SDL2/SDL.h overrides/SDL2/SDL_ttf.h overrides/stream_shim.h

all: $(KERNEL)

$(BUILD)/boot.o: boot.S
	@mkdir -p $(BUILD)
	$(CXX) -m64 $(BOOTDEFS) -c $< -o $@

$(BUILD)/irq.o: irq.cpp hw.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(IRQFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# overrides/DejaVuMono_stub.h stands in for the embedded TTF (see the file).
$(BUILD)/game/wordle.o: $(GAME)/wordle.cpp $(GAME)/words.h $(GAME)/answers.h overrides/DejaVuMono_stub.h $(HDRS)
	@mkdir -p $(BUILD)/game
	$(CXX) $(CXXFLAGS) -include overrides/DejaVuMono_stub.h -c $< -o $@

$(BUILD)/mathf.o: mathf.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# std::map's tree balancing and std::unordered_set's hashing live in
# libstdc++; those objects are position-independent and self-contained, so
# take just them.
$(BUILD)/lib/%.o:
	@mkdir -p $(BUILD)/lib
	cd $(BUILD)/lib && ar x $(LIBSTDCXX) $*.o

# Link as ELF64, then wrap as ELF32 so QEMU's -kernel and GRUB's multiboot
# loader both accept it. The 32-bit entry stub switches to long mode.
$(KERNEL): $(OBJS) linker.ld
	ld $(LDFLAGS) $(OBJS) $(LIBGCC) -o $(BUILD)/wordle64.elf
	objcopy -O elf32-i386 $(BUILD)/wordle64.elf $@

# ---------------------------------------------------------------- UEFI
# wordle.efi = a small UEFI loader (gnu-efi) with the kernel embedded.
EFI_INC  ?= $(firstword $(wildcard /usr/include/efi))
EFI_CRT0 ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'crt0-efi-x86_64.o' -o -path '*gnuefi*x64*crt0.o' \) 2>/dev/null))
EFI_LDS  ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'elf_x86_64_efi.lds' -o -name 'elf_x64_efi.lds' \) 2>/dev/null))
EFI_LIBS ?= $(shell find /usr/lib /usr/lib64 \( -name libgnuefi.a -o -name libefi.a \) 2>/dev/null | sort -u)
EFI_BUILD := build/efi
EFI_CFLAGS := -I$(EFI_INC) -I$(EFI_INC)/x86_64 -I$(EFI_BUILD) -fpic -ffreestanding -fno-stack-protector \
              -fno-stack-check -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -O2 -Wall

efi: wordle.efi

# The kernel again, position-independent, built in its own directory.
$(EFI_BUILD)/kernel.elf: FORCE
	$(MAKE) --no-print-directory PIE=1 BUILD=$(EFI_BUILD)/k $(EFI_BUILD)/k/kernel-pie.elf
	cp $(EFI_BUILD)/k/kernel-pie.elf $@
FORCE:

$(BUILD)/kernel-pie.elf: $(OBJS) efi/kernel.ld
	ld -nostdlib -pie --no-dynamic-linker -z notext -z max-page-size=0x1000 --no-warn-rwx-segments \
	   -T efi/kernel.ld $(OBJS) $(LIBGCC) -o $@

$(EFI_BUILD)/kernel.bin $(EFI_BUILD)/kernel_layout.h: $(EFI_BUILD)/kernel.elf
	objcopy -O binary $< $(EFI_BUILD)/kernel.bin
	@nm $< | awk '/ __kernel_end$$/ {print "#define KERNEL_MEM_SIZE 0x" $$1} \
	              / efi_entry64$$/  {print "#define KERNEL_ENTRY 0x" $$1} \
	              / __rela_start$$/ {print "#define KERNEL_RELA_START 0x" $$1} \
	              / __rela_end$$/   {print "#define KERNEL_RELA_END 0x" $$1}' > $(EFI_BUILD)/kernel_layout.h
	@test $$(grep -c define $(EFI_BUILD)/kernel_layout.h) = 4 || { echo "kernel layout symbols missing"; exit 1; }

wordle.efi: efi/loader.c efi/image.S $(EFI_BUILD)/kernel.bin $(EFI_BUILD)/kernel_layout.h
	@test -n "$(EFI_CRT0)" -a -n "$(EFI_LDS)" || { echo "gnu-efi not found (Debian: gnu-efi, Fedora: gnu-efi-devel)"; exit 1; }
	gcc $(EFI_CFLAGS) -c efi/loader.c -o $(EFI_BUILD)/loader.o
	gcc -c -DKERNEL_BIN='"$(EFI_BUILD)/kernel.bin"' efi/image.S -o $(EFI_BUILD)/image.o
	ld -nostdlib -znocombreloc -shared -Bsymbolic -T $(EFI_LDS) $(EFI_CRT0) \
	   $(EFI_BUILD)/loader.o $(EFI_BUILD)/image.o $(EFI_LIBS) $(EFI_LIBS) -o $(EFI_BUILD)/wordle.so
	objcopy -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym -j .rel -j .rela \
	        -j '.rel.*' -j '.rela.*' -j .reloc -O efi-app-x86_64 --subsystem=10 $(EFI_BUILD)/wordle.so $@

# ---------------------------------------------------------------- images
iso: $(ISO)
$(ISO): $(KERNEL) grub.cfg
	@rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/wordle.elf
	cp grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/iso 2>/dev/null

# GRUB's core image with the kernel and grub.cfg packed inside it (as a
# compressed memdisk), behind GRUB's boot sector. No filesystem.
floppy: $(FLOPPY)
$(FLOPPY): $(KERNEL) grub.cfg
	@rm -rf $(BUILD)/fd && mkdir -p $(BUILD)/fd/boot/grub
	cp $(KERNEL) $(BUILD)/fd/boot/wordle.elf
	cp grub.cfg $(BUILD)/fd/boot/grub/grub.cfg
	tar -C $(BUILD)/fd -cf $(BUILD)/memdisk.tar boot
	@# GRUB loads its core image (modules + this compressed memdisk) below
	@# 640 KB, so it must stay under 480 KB, well before the floppy is full.
	$(GRUB_MKIMAGE) -O i386-pc -o $(BUILD)/core.img -p '(memdisk)/boot/grub' \
	    -m $(BUILD)/memdisk.tar memdisk tar normal configfile multiboot vbe video video_fb boot \
	    || { echo "$@: the kernel no longer fits GRUB's 480 KB floppy core image; use 'make iso' or shrink words.h/fonts.h"; exit 1; }
	cat $(GRUB_I386)/boot.img $(BUILD)/core.img > $@
	truncate -s 1474560 $@
	@echo "$@: GRUB core image $$(( $$(stat -c %s $(BUILD)/core.img) / 1024 )) KB of its 480 KB limit"

# ---------------------------------------------------------------- QEMU
# SOUND=hda (default), ac97 or none. ARGS adds kernel options, e.g. ARGS=debug.
AUDIODEV ?= $(if $(filter Darwin,$(shell uname)),coreaudio,pa)
QSOUND_ac97 := -audiodev $(AUDIODEV),id=snd0 -device AC97,audiodev=snd0
QSOUND_hda  := -audiodev $(AUDIODEV),id=snd0 -device intel-hda -device hda-output,audiodev=snd0
QSOUND_none :=
QUSB_xhci   := -device qemu-xhci -device usb-kbd -device usb-mouse
QUSB_none   :=
SOUND    ?= hda
USB      ?= xhci
KVM      ?= $(if $(wildcard /dev/kvm),-enable-kvm,)
QEMU     := qemu-system-x86_64 $(KVM) -m 64 -serial stdio $(QSOUND_$(SOUND)) $(QUSB_$(USB))

run: $(KERNEL)
	$(QEMU) -kernel $(KERNEL) -append "audio=$(SOUND) $(ARGS)"

run-iso: $(ISO)
	$(QEMU) -cdrom $(ISO)

run-floppy: $(FLOPPY)
	$(QEMU) -fda $(FLOPPY) -boot a

OVMF ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
          /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/qemu/ovmf-x86_64.bin))
OVMF_VARS ?= $(firstword $(wildcard $(subst CODE,VARS,$(OVMF))))
run-efi: wordle.efi
	@test -n "$(OVMF)" -a -n "$(OVMF_VARS)" || { echo "OVMF firmware not found (Debian: ovmf, Fedora: edk2-ovmf); set OVMF= and OVMF_VARS="; exit 1; }
	@rm -rf $(BUILD)/esp && mkdir -p $(BUILD)/esp/EFI/BOOT
	cp wordle.efi $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	$(QEMU) -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	        -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd -drive format=raw,file=fat:rw:$(BUILD)/esp

clean:
	rm -rf build $(KERNEL) $(ISO) $(FLOPPY) wordle.efi

.PHONY: all iso floppy efi run run-iso run-floppy run-efi clean FORCE
