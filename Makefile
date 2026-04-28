CC      = gcc
CFLAGS  ?= -O3 -march=native -Wall -Wextra -Wno-unused-parameter -std=c11
LDFLAGS ?= -lz

SRC = src/zxle.c
BIN = zxle$(if $(filter Windows_NT,$(OS)),.exe,)

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(BIN) tests/*.zxle tests/*.tmp

.PHONY: all clean
