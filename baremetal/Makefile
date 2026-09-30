# Bare-metal build of Letterlock.
#
#   make              build letterlock.elf (the kernel)
#   make run          boot it in QEMU/KVM
#   make iso          bootable GRUB CD/USB image (letterlock.iso)
#   make floppy       1.44 MB boot floppy (letterlock-floppy.img)
#   make efi          letterlock.efi, a UEFI application
#   make run-efi      boot letterlock.efi under OVMF
#
# The game (../letterlock.cpp) is compiled unchanged against the normal libstdc++
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
MKFS_FAT      ?= $(shell command -v mkfs.fat || command -v mkfs.vfat || echo /usr/sbin/mkfs.fat)

# PIE=1 builds the position-independent kernel that letterlock.efi embeds.
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

KERNEL := letterlock.elf
ISO    := letterlock.iso
FLOPPY := letterlock-floppy.img

PLATFORM := kernel sdl runtime stdhooks audio pci usb
LIBOBJS  := tree.o hashtable_c++0x.o hash_bytes.o

OBJS := $(BUILD)/boot.o $(BUILD)/irq.o $(BUILD)/mathf.o \
        $(addprefix $(BUILD)/,$(addsuffix .o,$(PLATFORM))) \
        $(BUILD)/game/letterlock.o \
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
$(BUILD)/game/letterlock.o: $(GAME)/letterlock.cpp $(GAME)/words.h $(GAME)/answers.h overrides/DejaVuMono_stub.h $(HDRS)
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
	ld $(LDFLAGS) $(OBJS) $(LIBGCC) -o $(BUILD)/letterlock64.elf
	objcopy -O elf32-i386 $(BUILD)/letterlock64.elf $@

# ---------------------------------------------------------------- UEFI
# letterlock.efi = a small UEFI loader (gnu-efi) with the kernel embedded.
EFI_INC  ?= $(firstword $(wildcard /usr/include/efi))
EFI_CRT0 ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'crt0-efi-x86_64.o' -o -path '*gnuefi*x64*crt0.o' \) 2>/dev/null))
EFI_LDS  ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'elf_x86_64_efi.lds' -o -name 'elf_x64_efi.lds' \) 2>/dev/null))
EFI_LIBS ?= $(shell find /usr/lib /usr/lib64 \( -name libgnuefi.a -o -name libefi.a \) 2>/dev/null | sort -u)
EFI_BUILD := build/efi
EFI_CFLAGS := -I$(EFI_INC) -I$(EFI_INC)/x86_64 -I$(EFI_BUILD) -fpic -ffreestanding -fno-stack-protector \
              -fno-stack-check -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -O2 -Wall

efi: letterlock.efi

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

letterlock.efi: efi/loader.c efi/image.S $(EFI_BUILD)/kernel.bin $(EFI_BUILD)/kernel_layout.h
	@test -n "$(EFI_CRT0)" -a -n "$(EFI_LDS)" || { echo "gnu-efi not found (Debian: gnu-efi, Fedora: gnu-efi-devel)"; exit 1; }
	gcc $(EFI_CFLAGS) -c efi/loader.c -o $(EFI_BUILD)/loader.o
	gcc -c -DKERNEL_BIN='"$(EFI_BUILD)/kernel.bin"' efi/image.S -o $(EFI_BUILD)/image.o
	ld -nostdlib -znocombreloc -shared -Bsymbolic -T $(EFI_LDS) $(EFI_CRT0) \
	   $(EFI_BUILD)/loader.o $(EFI_BUILD)/image.o $(EFI_LIBS) $(EFI_LIBS) -o $(EFI_BUILD)/letterlock.so
	objcopy -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym -j .rel -j .rela \
	        -j '.rel.*' -j '.rela.*' -j .reloc -O efi-app-x86_64 --subsystem=10 $(EFI_BUILD)/letterlock.so $@

# ---------------------------------------------------------------- images
iso: $(ISO)
$(ISO): $(KERNEL) grub.cfg
	@grep -q "multiboot /boot/$(KERNEL)" grub.cfg || { echo "grub.cfg doesn't boot /boot/$(KERNEL); update it (or check it in)"; exit 1; }
	@rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/letterlock.elf
	cp grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/iso 2>/dev/null

# GRUB's core image with the kernel and grub.cfg packed inside it (as a
# compressed memdisk), behind GRUB's boot sector. No filesystem.
floppy: $(FLOPPY)
$(FLOPPY): $(KERNEL) grub.cfg
	@grep -q "multiboot /boot/$(KERNEL)" grub.cfg || { echo "grub.cfg doesn't boot /boot/$(KERNEL); update it (or check it in)"; exit 1; }
	@# A plain FAT12 floppy: GRUB's boot sector (keeping the FAT header) in
	@# sector 0, GRUB's core image in the reserved sectors after it, and the
	@# kernel (gzipped; GRUB unpacks it) and grub.cfg as files. GRUB's image
	@# holds only GRUB, so it stays small whatever size the game grows to.
	$(GRUB_MKIMAGE) -O i386-pc -o $(BUILD)/core.img -p /boot/grub \
	    biosdisk fat gzio normal configfile multiboot vbe video video_fb boot
	gzip -9 -n -c $(KERNEL) > $(BUILD)/kernel.gz
	rm -f $@
	$(MKFS_FAT) -C -F 12 -f 2 -r 224 -R $$(( $$(stat -c %s $(BUILD)/core.img) / 512 + 2 )) -n LETTERLOCK $@ 1440 >/dev/null
	mmd -i $@ ::/boot ::/boot/grub
	mcopy -i $@ $(BUILD)/kernel.gz ::/boot/$(KERNEL)
	mcopy -i $@ grub.cfg ::/boot/grub/grub.cfg
	cp $(GRUB_I386)/boot.img $(BUILD)/boot.img
	dd if=$@ of=$(BUILD)/boot.img bs=1 skip=3 seek=3 count=59 conv=notrunc status=none
	dd if=$(BUILD)/boot.img of=$@ bs=512 count=1 conv=notrunc status=none
	dd if=$(BUILD)/core.img of=$@ bs=512 seek=1 conv=notrunc status=none
	@echo "$@: GRUB $$(( $$(stat -c %s $(BUILD)/core.img) / 1024 )) KB + kernel $$(( $$(stat -c %s $(BUILD)/kernel.gz) / 1024 )) KB (gzipped) of 1440 KB"

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
run-efi: letterlock.efi
	@test -n "$(OVMF)" -a -n "$(OVMF_VARS)" || { echo "OVMF firmware not found (Debian: ovmf, Fedora: edk2-ovmf); set OVMF= and OVMF_VARS="; exit 1; }
	@rm -rf $(BUILD)/esp && mkdir -p $(BUILD)/esp/EFI/BOOT
	cp letterlock.efi $(BUILD)/esp/EFI/BOOT/BOOTX64.EFI
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	$(QEMU) -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	        -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd -drive format=raw,file=fat:rw:$(BUILD)/esp

clean:
	rm -rf build $(KERNEL) $(ISO) $(FLOPPY) letterlock.efi

.PHONY: all iso floppy efi run run-iso run-floppy run-efi clean FORCE
