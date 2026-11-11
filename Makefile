# PC Native port - build from an MSYS2 MINGW64 shell:  make -j16
#
# src/gen/<module> is the game's code, recompiled from its DOS programs by
# tools/recomp.py (python dosport.py gen); src/rt is the PC runtime that
# replaces DOS, the DOS extender, the BIOS and the hardware; src/game holds
# what is particular to this game (launcher, texts, menu items).
# port.mk (TITLE, SHORT, EXE, MODS, ICON_SRC) is written by dosport.py configure.
CC    = gcc
B     = build
PY   ?= python
CD   ?= gamedata/cd
include port.mk

$(shell mkdir -p $(B)/tmp)
export TMP  := $(shell cygpath -m "$(CURDIR)/$(B)/tmp")
export TEMP := $(TMP)

SDL_CFLAGS := $(shell sdl2-config --cflags | sed 's/-Dmain=SDL_main//')
SDL_LIBS   := $(shell sdl2-config --static-libs | sed 's/-lSDL2main//;s/-mwindows//')

CFLAGS_GEN := -std=gnu11 -O2 -g0 -fwrapv -fno-strict-aliasing -w -Isrc/rt
CFLAGS_RT  := -DSDL_MAIN_HANDLED -std=gnu11 -O2 -g -Wall -Wno-unused-function -fwrapv -fno-strict-aliasing -Isrc/rt $(SDL_CFLAGS)
LIBS       := -static $(SDL_LIBS) -ld3d9 -lwinmm -lgdi32 -lm

GEN_SRCS := $(wildcard src/gen/*/*.c)
RT_SRCS  := $(wildcard src/rt/*.c) $(wildcard src/game/*.c)
# the dev build gets the game's icon when ICON_SRC (an LBM on the CD) exists
RES_OBJ := $(if $(and $(ICON_SRC),$(wildcard $(ICON_SRC))),$(B)/game_res.o,)
OBJS := $(patsubst src/%.c,$(B)/%.o,$(GEN_SRCS) $(RT_SRCS))

all: $(EXE)

$(EXE): $(OBJS) $(RES_OBJ)
	$(CC) -mwindows -o $@ $^ $(LIBS)

$(B)/game.ico: $(ICON_SRC) tools/make_icon.py tools/lbm.py tools/fli.py
	$(PY) tools/make_icon.py $< $@ $(ICON_BG)

$(B)/game_res.o: src/rt/game.rc $(B)/game.ico
	windres -I $(B) $< -o $@

$(B)/gen/%.o: src/gen/%.c src/rt/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_GEN) -I$(dir $<) -c $< -o $@

$(B)/rt/%.o: src/rt/%.c src/rt/rt.h src/rt/cpu.h src/rt/game.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_RT) -c $< -o $@

$(B)/game/%.o: src/game/%.c src/rt/rt.h src/rt/cpu.h src/rt/game.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_RT) -c $< -o $@

# regenerate the recompiled code (after changing tools/ or tools/native_*.txt)
gen:
	$(PY) dosport.py gen

clean:
	rm -rf $(B) $(EXE)

.PHONY: all gen clean

# ---- release: game exe without the game's icon (setup puts it in from the
# user's CD) and the setup program with that exe inside ----
# file names can't hold ':' (titles like "Star Trek: ...")
SETUP_NAME ?= $(subst :,,$(TITLE)) PC Setup.exe
release: build/release/game.exe build/setup.exe
	cp build/setup.exe "$(SETUP_NAME)"

# the release exe keeps the game icon when the dev build has one (ICON_SRC)
build/release/game.exe: $(OBJS) $(RES_OBJ)
	@mkdir -p build/release
	$(CC) -mwindows -o $@ $^ $(LIBS)
	strip $@

SETUP_SRCS := $(wildcard installer/*.c)
SETUP_LIBS := -lcomctl32 -lcomdlg32 -lole32 -luuid -lshell32 -ladvapi32

$(B)/setup_res.o: installer/setup.rc installer/setup.manifest build/release/game.exe
	windres -I installer $< -o $@

build/setup.exe: $(SETUP_SRCS) installer/setup.h installer/install.h src/rt/game.h $(B)/setup_res.o
	$(CC) -O2 -Wall -Wno-format-truncation -mwindows -static -Isrc/rt -o $@ $(SETUP_SRCS) $(B)/setup_res.o $(SETUP_LIBS)

.PHONY: release
