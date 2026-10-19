A16 := /Users/spot/opt/amiga16
A65 := /Users/spot/opt/amiga
SP  := /Users/spot/Code/UP-Term/upterm-ports/build/sysroot/UP-Term
CXX := $(A16)/bin/m68k-amigaos-g++
CC  := $(A16)/bin/m68k-amigaos-gcc
AR  := $(A16)/bin/m68k-amigaos-ar
GINC := $(A16)/lib/gcc/m68k-amigaos/16.1.1b/include
INCS := -nostdinc -isystem $(CURDIR)/compat/inc -isystem $(GINC)/c++ -isystem $(GINC)/c++/m68k-amigaos -isystem $(GINC)/c++/backward -isystem $(GINC) -isystem $(A65)/m68k-amigaos/ixemul/include -isystem $(SP)/include -isystem $(SP)/include/ncursesw
CPU ?= -m68040
# an FPU build (-m68881) links the FPU archives: ixemul's soft glue returns doubles in d0/d1
FPU := $(findstring -m68881,$(CPU))
IXLIB := $(A65)/m68k-amigaos/ixemul/lib$(if $(FPU),/libm020/libm881)
IXCOMPAT := $(if $(FPU),$(IXLIB),$(SP)/lib)/libixcompat.a
COMMON := -include $(CURDIR)/compat/ixcompat_cxx.h $(CPU) -O2 -fno-builtin -fno-tree-loop-distribute-patterns
