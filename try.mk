include env.mk
all: ; $(CXX) $(COMMON) -std=c++17 $(INCS) -c t2.cpp -o out/t2.o
