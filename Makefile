CC      = gcc
CXX     = g++
CFLAGS  ?= -O3 -march=native -Wall -Wextra -Wno-unused-parameter -std=c11
CXXFLAGS ?= -O3 -march=native -Wall -Wextra -std=c++14
LDFLAGS ?= -lz -lstdc++

PREFLATE_DIR = third_party/preflate
PREFLATE_LIB = $(PREFLATE_DIR)/build/libpreflate.a
PREFLATE_INC = -I$(PREFLATE_DIR)

# Each translation unit listed here. zxle.c is the driver; everything else is a
# format/helper module described in graph.md. Adding a new module: drop foo.c
# next to its foo.h, list foo.c in SRC_C, and update graph.md.
SRC_C = \
    src/zxle.c \
    src/util.c \
    src/deflate.c \
    src/zip.c \
    src/recipe.c \
    src/png.c \
    src/gz.c \
    src/bz2.c \
    src/zst.c \
    src/xz.c \
    src/tar.c \
    src/ar.c \
    src/jpeg.c \
    src/mp3.c

SRC_CXX = src/preflate_shim.cpp
OBJ_C   = $(SRC_C:.c=.o)
OBJ_CXX = $(SRC_CXX:.cpp=.o)
BIN     = zxle$(if $(filter Windows_NT,$(OS)),.exe,)

all: $(BIN)

$(BIN): $(OBJ_C) $(OBJ_CXX) $(PREFLATE_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ_C) $(OBJ_CXX) $(PREFLATE_LIB) $(LDFLAGS)

# All C sources share the same flags; pattern rule for any src/*.c.
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(PREFLATE_INC) -c -o $@ $<

$(PREFLATE_LIB):
	@echo "preflate not built. Run 'make preflate-deps' first."
	@exit 1

preflate-deps:
	@if [ ! -d $(PREFLATE_DIR) ]; then \
	  mkdir -p third_party && \
	  git clone --depth 1 https://github.com/deus-libri/preflate.git $(PREFLATE_DIR); \
	fi
	@if ! grep -q '<cstdint>' $(PREFLATE_DIR)/preflate_seq_chain.h; then \
	  sed -i 's|#include <algorithm>|#include <algorithm>\n#include <cstdint>|' $(PREFLATE_DIR)/preflate_seq_chain.h; \
	fi
	@mkdir -p $(PREFLATE_DIR)/build
	@cd $(PREFLATE_DIR)/build && cmake -G "MinGW Makefiles" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ..
	@cd $(PREFLATE_DIR)/build && mingw32-make preflate || true

BRUNSLI_DIR = third_party/brunsli

brunsli-deps:
	@if [ ! -d $(BRUNSLI_DIR) ]; then \
	  mkdir -p third_party && \
	  git clone --depth 1 --recurse-submodules https://github.com/google/brunsli.git $(BRUNSLI_DIR); \
	fi
	@mkdir -p $(BRUNSLI_DIR)/build
	@cd $(BRUNSLI_DIR)/build && cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ..
	@cd $(BRUNSLI_DIR)/build && mingw32-make -j4 cbrunsli dbrunsli
	@echo "brunsli built. Add $(PWD)/$(BRUNSLI_DIR)/build/artifacts to PATH, or use tests/bench.sh which auto-detects."

PACKMP3_DIR = third_party/packmp3

packmp3-deps:
	@if [ ! -d $(PACKMP3_DIR) ]; then \
	  mkdir -p third_party && \
	  git clone --depth 1 https://github.com/packjpg/packMP3.git $(PACKMP3_DIR); \
	fi
	@cd $(PACKMP3_DIR)/source && $(MAKE) RES= || true
	@echo "packmp3 built. Add $(PWD)/$(PACKMP3_DIR)/source to PATH, or use tests/bench.sh which auto-detects."

clean:
	rm -f $(BIN) src/*.o tests/*.zxle tests/*.tmp

.PHONY: all clean preflate-deps brunsli-deps packmp3-deps
