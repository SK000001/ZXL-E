CC      = gcc
CXX     = g++
CFLAGS  ?= -O3 -march=native -Wall -Wextra -Wno-unused-parameter -std=c11
CXXFLAGS ?= -O3 -march=native -Wall -Wextra -std=c++14
LDFLAGS ?= -lz -lstdc++ -lpthread

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
    src/mp3.c \
    src/pdf.c

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

# MinGW generator on Windows; cmake's default (Unix Makefiles) elsewhere.
ifeq ($(OS),Windows_NT)
CMAKE_GEN = -G "MinGW Makefiles"
endif

preflate-deps:
	@if [ ! -d $(PREFLATE_DIR) ]; then \
	  mkdir -p third_party && \
	  git clone --depth 1 https://github.com/deus-libri/preflate.git $(PREFLATE_DIR); \
	fi
	@if ! grep -q '<cstdint>' $(PREFLATE_DIR)/preflate_seq_chain.h; then \
	  sed -i 's|#include <algorithm>|#include <algorithm>\n#include <cstdint>|' $(PREFLATE_DIR)/preflate_seq_chain.h; \
	fi
	@if ! grep -q '_ftelli64 ftello' $(PREFLATE_DIR)/support/filestream.cpp; then \
	  sed -i '1s/^/#ifndef _WIN32\n#define _ftelli64 ftello\n#define _fseeki64 fseeko\n#endif\n/' $(PREFLATE_DIR)/support/filestream.cpp; \
	fi
	@mkdir -p $(PREFLATE_DIR)/build
	@cd $(PREFLATE_DIR)/build && cmake $(CMAKE_GEN) -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ..
	@cd $(PREFLATE_DIR)/build && cmake --build . --target preflate -j 4

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

PACKJPG_DIR = third_party/packjpg

# packjpg-deps: optional; enables the packJPG JPEG codec (tried alongside
# brunsli, smaller blob wins). Same build pattern as packMP3 (same author).
packjpg-deps:
	@if [ ! -d $(PACKJPG_DIR) ]; then \
	  mkdir -p third_party && \
	  git clone --depth 1 https://github.com/packjpg/packJPG.git $(PACKJPG_DIR); \
	fi
	@cd $(PACKJPG_DIR)/source && $(MAKE) RES= || true
	@echo "packjpg built. Add $(PWD)/$(PACKJPG_DIR)/source to PATH, or use tests/bench.sh which auto-detects."

ZPAQ_DIR = third_party/zpaq

# zpaq-deps: required for `zxle pack --slow` (zpaq -m5 final-step). Pulls the
# upstream Windows binary (zpaq 7.15) and aliases zpaq64.exe -> zpaq.exe so the
# bare `zpaq` invocation works on PATH.
zpaq-deps:
	@if [ ! -x $(ZPAQ_DIR)/zpaq.exe ] && [ ! -x $(ZPAQ_DIR)/zpaq ]; then \
	  mkdir -p $(ZPAQ_DIR) && \
	  curl -fsSL -o /tmp/zpaq715.zip http://mattmahoney.net/dc/zpaq715.zip && \
	  unzip -j -o /tmp/zpaq715.zip "zpaq64.exe" -d $(ZPAQ_DIR) >/dev/null && \
	  cp $(ZPAQ_DIR)/zpaq64.exe $(ZPAQ_DIR)/zpaq.exe && \
	  rm -f /tmp/zpaq715.zip; \
	fi
	@echo "zpaq ready at $(PWD)/$(ZPAQ_DIR)/. Add to PATH for --slow mode, or use tests/bench.sh which auto-detects."

SEVENZIP_DIR = third_party/7zip

# 7zip-deps: required only for the 7-Zip competitor section of bench.sh.
# Pulls the standalone console build (7zr.exe, .7z-only) from 7-zip.org.
7zip-deps:
	@if [ ! -x $(SEVENZIP_DIR)/7zr.exe ] && [ ! -x $(SEVENZIP_DIR)/7zr ]; then \
	  mkdir -p $(SEVENZIP_DIR) && \
	  curl -fsSL -o $(SEVENZIP_DIR)/7zr.exe https://www.7-zip.org/a/7zr.exe; \
	fi
	@echo "7zr ready at $(PWD)/$(SEVENZIP_DIR)/."

PRECOMP_DIR = third_party/precomp

# precomp-deps: required only for the precomp competitor section of bench.sh.
# Pulls the upstream Windows binary (precomp 0.4.7).
precomp-deps:
	@if [ ! -x $(PRECOMP_DIR)/precomp.exe ] && [ ! -x $(PRECOMP_DIR)/precomp ]; then \
	  mkdir -p $(PRECOMP_DIR) && \
	  curl -fsSL -o /tmp/precomp.zip https://github.com/schnaader/precomp-cpp/releases/download/v0.4.7/precomp.zip && \
	  unzip -j -o /tmp/precomp.zip "windows/precomp.exe" -d $(PRECOMP_DIR) >/dev/null && \
	  rm -f /tmp/precomp.zip; \
	fi
	@echo "precomp ready at $(PWD)/$(PRECOMP_DIR)/."

# Convenience: fetch real-world fixtures (silesia, real .deb, real .tar.xz)
# into tests/corpus/. Idempotent (skips files that already exist).
real-fixtures:
	@bash tests/fetch_real_fixtures.sh

# Convenience: fetch + build everything a fresh clone needs to run the full
# bench. ~10 min on a fresh machine (preflate + brunsli cmake builds dominate).
all-deps: preflate-deps brunsli-deps packmp3-deps packjpg-deps zpaq-deps precomp-deps 7zip-deps real-fixtures
	@echo
	@echo "All deps ready. Next:"
	@echo "  make                          # builds zxle"
	@echo "  bash tests/make_fixtures.sh   # regenerates synthetic fixtures"
	@echo "  bash tests/bench.sh           # runs the default bench"
	@echo "  ZXLE_SILESIA=1 ZXLE_SLOW=1 bash tests/bench.sh   # full bench"

clean:
	rm -f $(BIN) src/*.o tests/*.zxle tests/*.tmp

.PHONY: all clean preflate-deps brunsli-deps packmp3-deps packjpg-deps zpaq-deps precomp-deps 7zip-deps real-fixtures all-deps
