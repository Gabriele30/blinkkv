CC ?= cc
AR ?= ar
ARFLAGS ?= rcs

BASE_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Iinclude
CFLAGS ?= $(BASE_CFLAGS) -O2

BUILD_DIR := build
LIB := $(BUILD_DIR)/libblinkkv.a
STORE_TEST := $(BUILD_DIR)/test_store
BENCH_BIN := $(BUILD_DIR)/bench_baseline

CORE_SRCS := src/store.c src/hash.c
CORE_OBJS := $(CORE_SRCS:src/%.c=$(BUILD_DIR)/%.o)

.PHONY: all test debug release sanitize test_ndebug bench clean

all: $(LIB)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(CORE_OBJS)
	$(AR) $(ARFLAGS) $@ $^

$(STORE_TEST): tests/test_store.c $(LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_store.c $(LIB) -o $@

$(BENCH_BIN): bench/bench_baseline.c $(LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) bench/bench_baseline.c $(LIB) -o $@

test: $(STORE_TEST)
	$(STORE_TEST)

bench: $(BENCH_BIN)
	$(BENCH_BIN)

test_ndebug: clean
	$(MAKE) test CFLAGS="$(BASE_CFLAGS) -O2 -DNDEBUG"

debug: clean
	$(MAKE) test CFLAGS="$(BASE_CFLAGS) -g -O0 -DDEBUG"

release: clean
	$(MAKE) all CFLAGS="$(BASE_CFLAGS) -O3 -DNDEBUG"

sanitize: clean
	$(MAKE) test CFLAGS="$(BASE_CFLAGS) -fsanitize=address,undefined -g -fno-omit-frame-pointer"

clean:
	rm -rf $(BUILD_DIR)
