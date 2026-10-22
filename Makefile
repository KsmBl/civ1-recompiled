# Civilization (1991) recompiled -- native build
#
#   make            build ./civ (Linux, SDL2)
#   make lift       regenerate src/gen/ from the original game files
#   make dos        build civ.exe for DOS (needs DJGPP in toolchain/, see README)
#
GAMEDIR ?= ../CIV
CC      ?= cc
OPT     ?= -O2
CFLAGS  += $(OPT) -g -std=gnu11 -fno-strict-aliasing -fwrapv -Iruntime -Isrc/gen -Isrc/native \
           -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-function
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

RUNTIME := runtime/cpu.c runtime/dispatch.c runtime/loader.c runtime/dos.c \
           runtime/bios.c runtime/hw.c runtime/mouse.c runtime/audio.c \
           runtime/video.c runtime/wide.c runtime/gfx.c runtime/script.c runtime/interp.c runtime/main.c
PLATFORM := runtime/platform_sdl.c
GEN := $(wildcard src/gen/*.c)
NATIVE := $(wildcard src/native/*.c)

OBJDIR := build/obj
OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(RUNTIME) $(PLATFORM) $(GEN) $(NATIVE))

civ: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(SDL_LIBS) -lz -lm

$(OBJDIR)/%.o: %.c runtime/cpu.h runtime/runtime.h src/gen/modules.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c -o $@ $<

# debug variant that reports writes to watched memory (MEMWATCH)
WOBJDIR := build/objw
WOBJS := $(patsubst %.c,$(WOBJDIR)/%.o,$(RUNTIME) $(PLATFORM) $(GEN) $(NATIVE))

civ_watch: $(WOBJS)
	$(CC) $(CFLAGS) -o $@ $(WOBJS) $(SDL_LIBS) -lz -lm

$(WOBJDIR)/%.o: %.c runtime/cpu.h runtime/runtime.h src/gen/modules.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -O1 -DMEMWATCH $(SDL_CFLAGS) -c -o $@ $<

run: civ
	./civ

lift:
	tools/py tools/lift.py $(GAMEDIR) src/gen

clean:
	rm -rf $(OBJDIR) civ

TESTDIR := build/difftest
test: $(TESTDIR)/harness
	$(TESTDIR)/harness $(TESTDIR)/cases.bin

$(TESTDIR)/tests.c $(TESTDIR)/cases.bin: tools/lift.py tools/difftest.py tools/x86dec.py
	tools/py tools/difftest.py $(GAMEDIR) $(TESTDIR)

$(TESTDIR)/harness: tests/harness.c $(TESTDIR)/tests.c runtime/cpu.h runtime/cpu_ops.inc
	$(CC) -O1 -g -std=gnu11 -fwrapv -Iruntime -Isrc/gen -Itests -o $@ tests/harness.c $(TESTDIR)/tests.c

FTESTDIR := build/functest
FTOBJS := $(patsubst src/gen/%.c,$(FTESTDIR)/obj/%.o,$(filter-out src/gen/dispatch.c,$(GEN)))

functest: $(FTESTDIR)/harness
	$(FTESTDIR)/harness $(FTESTDIR)

$(FTESTDIR)/fcases.bin $(FTESTDIR)/funcs_list.c: tools/lift.py tools/functest.py tools/x86dec.py
	tools/py tools/functest.py $(GAMEDIR) $(FTESTDIR)

$(FTESTDIR)/obj/%.o: src/gen/%.c runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) -O1 -std=gnu11 -fno-strict-aliasing -fwrapv -DLOADSEG=0 -Iruntime -Isrc/gen -w -c -o $@ $<

$(FTESTDIR)/harness: tests/funcharness.c $(FTESTDIR)/funcs_list.c $(FTOBJS)
	$(CC) -O1 -g -std=gnu11 -fwrapv -DLOADSEG=0 -Iruntime -Isrc/gen -o $@ tests/funcharness.c $(FTESTDIR)/funcs_list.c $(FTOBJS)

# ---------------------------------------------------------------- DOS
DJGPP   ?= toolchain/djgpp
DOSCC   := $(DJGPP)/bin/i586-pc-msdosdjgpp-gcc
DOSOBJ  := build/dosobj
DOSSRC  := $(filter-out runtime/platform_sdl.c,$(RUNTIME)) runtime/platform_dos.c $(GEN) $(NATIVE)
DOSOBJS := $(patsubst %.c,$(DOSOBJ)/%.o,$(DOSSRC))

dos: civ.exe

civ.exe: $(DOSOBJS)
	$(DOSCC) -o build/civ_dos.exe $(DOSOBJS) -lm
	$(DJGPP)/i586-pc-msdosdjgpp/bin/exe2coff build/civ_dos.exe
	cat toolchain/cwsdpmi/bin/CWSDSTUB.EXE build/civ_dos > $@

$(DOSOBJ)/%.o: %.c runtime/cpu.h runtime/runtime.h src/gen/modules.h
	@mkdir -p $(dir $@)
	$(DOSCC) -O2 -std=gnu11 -fno-strict-aliasing -fwrapv -DNO_PNG -Iruntime -Isrc/gen -Isrc/native \
	    -Wno-unused-label -c -o $@ $<

.PHONY: run lift clean dos test functest
