CC     = gcc

# Arena element width: 32 (default) or 16. 16 halves the arena but caps the
# VARIABLE INDEX AT 32767, and exceeding that wraps silently -- the cubes come
# out well-formed and over the wrong literals. Five of our 39
# instances are past it (cross-13 110777 vars, sin_depth_miter_4 44098,
# mchess-25/35/45 39805), so 32 is the default and 16 is the opt-in.
# `make clean` is required when changing this: make cannot see the flag move.
ARENA_BITS ?= 32
ARENA_DEF   = -DDROOFIX_ARENA_BITS=$(ARENA_BITS)

CFLAGS = -O3 -Wall -Wextra -std=c11 $(ARENA_DEF)
TARGET = droofix
SRC    = main.c droofix.c

$(TARGET): $(SRC) droofix.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

debug: $(SRC) droofix.h
	NIX_HARDENING_ENABLE="" $(CC) -g -O0 -fsanitize=address,undefined \
	    -Wall -Wextra -std=c11 $(ARENA_DEF) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: clean debug
