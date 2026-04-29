CC      = gcc
CXX     = g++
CFLAGS  ?= -O3 -march=native -Wall -Wextra -Wno-unused-parameter -std=c11
CXXFLAGS ?= -O3 -march=native -Wall -Wextra -std=c++14
LDFLAGS ?= -lz -lstdc++

PREFLATE_DIR = third_party/preflate
PREFLATE_LIB = $(PREFLATE_DIR)/build/libpreflate.a
PREFLATE_INC = -I$(PREFLATE_DIR)

SRC_C   = src/zxle.c
SRC_CXX = src/preflate_shim.cpp
OBJ_C   = src/zxle.o
OBJ_CXX = src/preflate_shim.o
BIN     = zxle$(if $(filter Windows_NT,$(OS)),.exe,)

all: $(BIN)

$(BIN): $(OBJ_C) $(OBJ_CXX) $(PREFLATE_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ_C) $(OBJ_CXX) $(PREFLATE_LIB) $(LDFLAGS)

$(OBJ_C): $(SRC_C)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_CXX): $(SRC_CXX)
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

clean:
	rm -f $(BIN) src/*.o tests/*.zxle tests/*.tmp

.PHONY: all clean preflate-deps
