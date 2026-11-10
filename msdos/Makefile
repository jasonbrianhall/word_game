# Letterlock for MS-DOS / FreeDOS (DJGPP, 32-bit DPMI)
#
#   make            letterlk.exe with CWSDPMI embedded (runs on its own)
#   make msdos_original
#                   letterlk.exe that needs CWSDPMI.EXE next to it (or any
#                   other DPMI host: Windows 9x, HDPMI32, ...)
#   make run        run it in DOSBox
#   make clean
#
# The compiler: a local DJGPP cross compiler (i586-pc-msdosdjgpp-g++ on the
# PATH) when there is one, otherwise the djfdyuruiry/djgpp Docker image.
# Force either with DOCKER=1 / DOCKER=0.
#
# The game (../letterlock.cpp) is compiled as is, drawing through
# ../baremetal/sdl.cpp, the bare-metal build's SDL2/SDL_ttf stand-in;
# dos_main.cpp replaces the bare-metal kernel (VESA/VGA video, BIOS keyboard,
# INT 33h mouse, Sound Blaster or PC speaker, IRQ0 timer).
#
# Needs a 386 with a 387 (or EMU387.DXE from DJGPP v2misc next to it); a
# Pentium or better to be pleasant. LETTERLK /NOSOUND, /SPEAKER, /VGA;
# SET LETTERVID=800x600x16 picks a VESA mode. Statistics are kept in the
# current directory.
GAME    := ..
BM      := ../baremetal

DOS_TARGET      = letterlk.exe
DOS_COFF        = letterlk
COFF_TARGET     = letterlk.coff
EMBEDDED_TARGET = embedded.exe

DJGPP_IMAGE = djfdyuruiry/djgpp
CSDPMI_URL  = http://na.mirror.garr.it/mirrors/djgpp/current/v2misc/csdpmi7b.zip

# The container writes into this folder, so it runs as the folder's owner.
USER_ID  = $(shell stat -c %u .)
GROUP_ID = $(shell stat -c %g .)

CROSS ?= i586-pc-msdosdjgpp-
ifeq ($(origin DOCKER),undefined)
DOCKER := $(if $(shell command -v $(CROSS)g++ 2>/dev/null),0,1)
endif

ifeq ($(DOCKER),1)
# The whole repository is mounted, so .. and ../baremetal are reachable.
RUN      = docker run --rm -v $(abspath ..):/src:z -w /src/msdos -u $(USER_ID):$(GROUP_ID) $(DJGPP_IMAGE) /bin/sh -c
CXX_DOS  = g++
EXE2COFF = exe2coff
else
RUN      = /bin/sh -c
CXX_DOS  = $(CROSS)g++
EXE2COFF = $(shell command -v $(CROSS)exe2coff 2>/dev/null || echo $(dir $(shell command -v $(CROSS)g++))../i586-pc-msdosdjgpp/bin/exe2coff)
endif

# letterlock.cpp's main() becomes letterlock_main(), called by dos_main.cpp;
# DejaVuMono_stub.h keeps the 450 KB TTF out (text comes from fonts.h).
# dos_compat.h papers over two DJGPP library differences (see the file).
ARCH     = -O2 -march=i386 -mtune=i586
CXXFLAGS = $(ARCH) -std=gnu++17 -w -Iinclude -I$(BM) -I$(GAME) -DMSDOS
GAMEFLAGS = -Dmain=letterlock_main -include dos_compat.h -include $(BM)/overrides/DejaVuMono_stub.h
LDFLAGS  = -s -Wl,--gc-sections

CXX_SRCS = dos_main.cpp $(BM)/sdl.cpp
HDRS     = $(GAME)/words.h $(GAME)/answers.h $(BM)/fonts.h $(BM)/platform.hpp $(BM)/audio.hpp \
           $(BM)/overrides/SDL2/SDL.h $(BM)/overrides/SDL2/SDL_ttf.h $(BM)/overrides/DejaVuMono_stub.h \
           include/SDL2/SDL.h include/SDL2/SDL_ttf.h dos_compat.h

BUILD_CMD = mkdir -p build && \
	$(CXX_DOS) $(CXXFLAGS) $(GAMEFLAGS) -c $(GAME)/letterlock.cpp -o build/letterlock.o && \
	$(CXX_DOS) $(CXXFLAGS) $(CXX_SRCS) build/letterlock.o $(LDFLAGS) -o $(DOS_TARGET)

all: msdos_embedded

pull-djgpp:
ifeq ($(DOCKER),1)
	docker pull $(DJGPP_IMAGE)
endif

get-csdpmi:
	@if [ ! -d "csdpmi" ]; then \
		wget $(CSDPMI_URL); \
		unzip -o csdpmi7b.zip -d csdpmi; \
	fi

$(COFF_TARGET): $(CXX_SRCS) $(GAME)/letterlock.cpp $(HDRS) | pull-djgpp
	$(RUN) "$(BUILD_CMD) && $(EXE2COFF) $(DOS_TARGET) && mv $(DOS_COFF) $(COFF_TARGET)"

# Needs CWSDPMI.EXE (or another DPMI host) at run time.
msdos_original: $(COFF_TARGET) get-csdpmi
	$(RUN) "$(BUILD_CMD)"
	cp csdpmi/bin/CWSDPMI.EXE .

# CWSDPMI's stub in front: one file that runs on plain DOS.
msdos_embedded: $(COFF_TARGET) get-csdpmi
	@if [ ! -f "csdpmi/bin/CWSDSTUB.EXE" ]; then \
		echo "ERROR: CWSDSTUB.EXE not found in csdpmi/bin directory!"; \
		exit 1; \
	fi
	cat csdpmi/bin/CWSDSTUB.EXE $(COFF_TARGET) > $(EMBEDDED_TARGET)
	mv $(EMBEDDED_TARGET) $(DOS_TARGET)
	chmod +x $(DOS_TARGET)
	@echo "Created $(DOS_TARGET) with embedded DPMI extender"

msdos: msdos_embedded

run: msdos_embedded
	@mkdir -p build/dosc
	cp $(DOS_TARGET) build/dosc/
	printf '[cpu]\ncore=dynamic\ncputype=auto\ncycles=max\n[dosbox]\nmachine=svga_s3\nmemsize=32\n[speaker]\npcspeaker=true\n' > build/dosbox.conf
	dosbox -conf build/dosbox.conf -c "mount c build/dosc" -c "c:" -c "$(DOS_TARGET)"

clean:
	rm -rf build $(DOS_TARGET) $(COFF_TARGET) $(EMBEDDED_TARGET) $(DOS_COFF) CWSDPMI.EXE || true
	rm -rf csdpmi csdpmi7b.zip csdpmi7b.zip.* || true

.PHONY: all pull-djgpp get-csdpmi msdos msdos_original msdos_embedded run clean
