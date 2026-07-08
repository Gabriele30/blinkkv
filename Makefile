CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Iinclude
AR ?= ar
ARFLAGS = rcs

BUILD_DIR := build
LIB := $(BUILD_DIR)/libblinkkv.a
STORE_TEST := $(BUILD_DIR)/test_store

CORE_SRCS := src/store.c src/hash.c
CORE_OBJS := $(CORE_SRCS:src/%.c=$(BUILD_DIR)/%.o)

.PHONY: all test clean

all: $(LIB)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(CORE_OBJS)
	$(AR) $(ARFLAGS) $@ $^

$(STORE_TEST): tests/test_store.c $(LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) tests/test_store.c $(LIB) -o $@

test: $(STORE_TEST)
	$(STORE_TEST)

clean:
	rm -rf $(BUILD_DIR)
