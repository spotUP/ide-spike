include env.mk
# NCDIR=build/ncurses/obj/lib links the private ncurses copy (build/ncurses) instead of the sysroot one
NCDIR ?=
NCL := $(if $(NCDIR),-L$(NCDIR))
NCDEP := $(if $(NCDIR),$(NCDIR)/libncursesw.a)
S := src/scintilla/scintilla
L := src/lexilla/lexilla
C := $(wildcard src/scinterm/*)
SCI_SRCS := $(wildcard $(S)/src/*.cxx) $(C)/PlatCurses.cxx $(C)/ScintillaCurses.cxx
LEX_SRCS := $(wildcard $(L)/lexers/*.cxx) $(wildcard $(L)/lexlib/*.cxx) $(L)/src/Lexilla.cxx
SCI_OBJS := $(patsubst %.cxx,build/sci/%.o,$(notdir $(SCI_SRCS)))
LEX_OBJS := $(patsubst %.cxx,build/lex/%.o,$(notdir $(LEX_SRCS)))
DEFS := -DPTRDIFF_DOESNT_ALIAS_INT -DCURSES -DSCI_LEXER -DNDEBUG -D_XOPEN_SOURCE_EXTENDED
CXXF := $(COMMON) -std=c++17 $(INCS) $(DEFS) -I$(S)/include -I$(S)/src -I$(L)/include -I$(L)/lexlib -I$(C)
vpath %.cxx $(S)/src $(C) $(L)/lexers $(L)/lexlib $(L)/src
all: out/scidemo
build/sci/%.o: %.cxx ; @$(CXX) $(CXXF) -c $< -o $@ > logs/sci_$*.log 2>&1 || { echo FAIL $<; grep -m3 error logs/sci_$*.log; exit 1; }
build/lex/%.o: %.cxx ; @$(CXX) $(CXXF) -c $< -o $@ > logs/lex_$*.log 2>&1 || { echo FAIL $<; grep -m3 error logs/lex_$*.log; exit 1; }
build/wcwidth.o: $(C)/wcwidth.c ; $(CC) $(COMMON) $(INCS) -c $< -o $@
build/ixcxxrt.o: compat/ixcxxrt.c ; $(CC) $(COMMON) $(INCS) -c $< -o $@
build/ixlink.o: compat/ixlink.cxx ; $(CXX) $(CXXF) -c $< -o $@
build/libscintilla.a: $(SCI_OBJS) build/wcwidth.o build/ixlink.o ; rm -f $@; $(AR) rcs $@ $^
build/liblexilla.a: $(LEX_OBJS) ; rm -f $@; $(AR) rcs $@ $^
build/scidemo.o: scidemo.cxx ; $(CXX) $(CXXF) -c $< -o $@
out/scidemo: build/scidemo.o build/ixcxxrt.o build/libscintilla.a build/liblexilla.a $(NCDEP)
	$(CXX) $(CPU) -nostdlib $(NCL) -L$(IXLIB) -L$(A65)/m68k-amigaos/ixemul/lib -L$(SP)/lib $(A65)/m68k-amigaos/ixemul/lib/crt0.o build/ixcxxrt.o -Wl,--wrap=main -Wl,--wrap=write -Wl,--wrap=select -Wl,--wrap=_nc_flush_sp -Wl,--wrap=_nc_mvcur_sp -Wl,--wrap=vid_puts_sp -Wl,--wrap=_nc_scroll_optimize_sp -Wl,--wrap=_nc_hash_map_sp -Wl,--wrap=wcrtomb $< -Lbuild -lscintilla -llexilla -lncursesw $(IXCOMPAT) -lstdc++ -lm -lgcc -lc -lgcc -o $@ > logs/link.log 2>&1 || { tail -n 20 logs/link.log; exit 1; }

# lite variant: same objects, catalogue trimmed to 9 lexers (copy of vendor Lexilla.cxx, vendor untouched)
build/LexillaLite.cxx: $(L)/src/Lexilla.cxx tools_lite.py
	python3 tools_lite.py $< $@
build/lexlite/LexillaLite.o: build/LexillaLite.cxx ; @mkdir -p build/lexlite; $(CXX) $(CXXF) -c $< -o $@ > logs/lex_lite.log 2>&1 || { tail -n 20 logs/lex_lite.log; exit 1; }
build/liblexillalite.a: $(filter-out build/lex/Lexilla.o,$(LEX_OBJS)) build/lexlite/LexillaLite.o ; rm -f $@; $(AR) rcs $@ $^
out/scidemo-lite: build/scidemo.o build/ixcxxrt.o build/libscintilla.a build/liblexillalite.a $(NCDEP)
	$(CXX) $(CPU) -nostdlib $(NCL) -L$(IXLIB) -L$(A65)/m68k-amigaos/ixemul/lib -L$(SP)/lib $(A65)/m68k-amigaos/ixemul/lib/crt0.o build/ixcxxrt.o -Wl,--wrap=main -Wl,--wrap=write -Wl,--wrap=select -Wl,--wrap=_nc_flush_sp -Wl,--wrap=_nc_mvcur_sp -Wl,--wrap=vid_puts_sp -Wl,--wrap=_nc_scroll_optimize_sp -Wl,--wrap=_nc_hash_map_sp -Wl,--wrap=wcrtomb $< -Lbuild -lscintilla -llexillalite -lncursesw $(IXCOMPAT) -lstdc++ -lm -lgcc -lc -lgcc -o $@ > logs/link_lite.log 2>&1 || { tail -n 20 logs/link_lite.log; exit 1; }

# the rig runs the stripped copy (bench_rig.py stages out/<variant>.stripped); built here so it
# can never lag the binary it came from
out/%.stripped: out/% ; $(A16)/bin/m68k-amigaos-strip -o $@ $<
